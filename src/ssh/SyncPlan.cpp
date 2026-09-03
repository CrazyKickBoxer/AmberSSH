#include "SyncPlan.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace amber
{

namespace
{

bool CaseInsensitiveFind(const std::string& hay, const char* needle)
{
    const size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || hay.size() < n)
        return false;
    for (size_t i = 0; i + n <= hay.size(); ++i)
    {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == n)
            return true;
    }
    return false;
}

// Depth of a relative path, so directories can be ordered outermost-first.
int Depth(const std::string& p)
{
    return static_cast<int>(std::count(p.begin(), p.end(), '/'));
}

} // namespace

const char* CompareStateName(CompareState s)
{
    switch (s)
    {
    case CompareState::Identical:   return "identical";
    case CompareState::LocalOnly:   return "local only";
    case CompareState::RemoteOnly:  return "remote only";
    case CompareState::LocalNewer:  return "local newer";
    case CompareState::RemoteNewer: return "remote newer";
    case CompareState::SizeDiffers: return "size differs";
    case CompareState::TypeDiffers: return "type differs";
    case CompareState::Uncertain:   return "uncertain";
    }
    return "unknown";
}

// -------------------------------------------------------------- globbing
namespace
{

// Recursive matcher over path segments. `**` consumes any number of them.
bool GlobHere(const char* pat, const char* str)
{
    while (*pat)
    {
        if (pat[0] == '*' && pat[1] == '*')
        {
            pat += 2;
            if (*pat == '/')
                ++pat;
            if (!*pat)
                return true;              // "**" at the end matches the rest
            for (const char* s = str;; ++s)
            {
                if (GlobHere(pat, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (*pat == '*')
        {
            ++pat;
            // A single star stops at a separator.
            for (const char* s = str;; ++s)
            {
                if (GlobHere(pat, s))
                    return true;
                if (!*s || *s == '/')
                    return false;
            }
        }
        if (!*str)
            return false;
        if (*pat == '?')
        {
            if (*str == '/')
                return false;
            ++pat;
            ++str;
            continue;
        }
        if (std::tolower(static_cast<unsigned char>(*pat)) !=
            std::tolower(static_cast<unsigned char>(*str)))
            return false;
        ++pat;
        ++str;
    }
    return *str == 0;
}

} // namespace

bool MatchGlob(const std::string& pattern, const std::string& path, bool isDir)
{
    if (pattern.empty())
        return false;
    std::string pat = pattern;
    // A trailing '/' means "directories only".
    bool dirOnly = false;
    if (pat.back() == '/')
    {
        dirOnly = true;
        pat.pop_back();
    }
    if (dirOnly && !isDir)
        return false;
    // A leading '/' anchors at the sync root; without it the pattern may
    // match at any depth, which is what a user means by "*.tmp".
    if (!pat.empty() && pat.front() == '/')
        return GlobHere(pat.c_str() + 1, path.c_str());
    if (GlobHere(pat.c_str(), path.c_str()))
        return true;
    // Unanchored: try every segment boundary.
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '/' && GlobHere(pat.c_str(), path.c_str() + i + 1))
            return true;
    return false;
}

bool Excluded(const CompareOptions& o, const std::string& relPath, bool isDir)
{
    for (const std::string& g : o.excludeGlobs)
        if (MatchGlob(g, relPath, isDir))
            return true;
    // A path excluded by one of its ancestors is excluded too, so an
    // exclusion of a directory really does keep its contents out.
    for (size_t i = 0; i < relPath.size(); ++i)
    {
        if (relPath[i] != '/')
            continue;
        const std::string ancestor = relPath.substr(0, i);
        for (const std::string& g : o.excludeGlobs)
            if (MatchGlob(g, ancestor, true))
                return true;
    }
    return false;
}

// ------------------------------------------------------------ comparison
std::vector<ComparePair> Compare(const std::vector<SyncEntry>& local,
                                 const std::vector<SyncEntry>& remote,
                                 const CompareOptions& o)
{
    // std::map so the result is in path order, which is also the order the
    // planner needs (a parent sorts before its children).
    std::map<std::string, ComparePair> byPath;
    for (const SyncEntry& e : local)
    {
        if (Excluded(o, e.path, e.dir))
            continue;
        ComparePair& p = byPath[e.path];
        p.path = e.path;
        p.local = e;
        p.haveLocal = true;
    }
    for (const SyncEntry& e : remote)
    {
        if (Excluded(o, e.path, e.dir))
            continue;
        ComparePair& p = byPath[e.path];
        p.path = e.path;
        p.remote = e;
        p.haveRemote = true;
    }

    std::vector<ComparePair> out;
    out.reserve(byPath.size());
    for (auto& [path, p] : byPath)
    {
        if (!p.haveRemote)
            p.state = CompareState::LocalOnly;
        else if (!p.haveLocal)
            p.state = CompareState::RemoteOnly;
        else if (p.local.dir != p.remote.dir)
            p.state = CompareState::TypeDiffers;
        else if ((p.local.link || p.remote.link) && !o.followLinks)
            // A symlink is not compared. Following one during a sync is how a
            // tool copies a filesystem into a subdirectory of itself.
            p.state = CompareState::Uncertain;
        else if (p.local.dir)
            p.state = CompareState::Identical;    // both directories: nothing to move
        else if (p.local.mtime == 0 || p.remote.mtime == 0)
            // Without a timestamp on both sides there is no basis for
            // "newer", and guessing is how a sync overwrites the good copy.
            p.state = (p.local.size == p.remote.size) ? CompareState::Uncertain
                                                      : CompareState::SizeDiffers;
        else
        {
            const int64_t diff = p.local.mtime - p.remote.mtime;
            const bool sameTime = diff <= o.mtimeToleranceSec &&
                                  -diff <= o.mtimeToleranceSec;
            if (sameTime)
            {
                if (p.local.size != p.remote.size)
                    p.state = CompareState::SizeDiffers;
                else
                    p.state = o.trustSizeAndTime ? CompareState::Identical
                                                 : CompareState::Uncertain;
            }
            else
                p.state = diff > 0 ? CompareState::LocalNewer
                                   : CompareState::RemoteNewer;
        }
        out.push_back(p);
    }
    return out;
}

// --------------------------------------------------------------- planner
const char* SyncActionName(SyncAction a)
{
    switch (a)
    {
    case SyncAction::None:          return "none";
    case SyncAction::Upload:        return "upload";
    case SyncAction::Download:      return "download";
    case SyncAction::ReplaceRemote: return "replace remote";
    case SyncAction::ReplaceLocal:  return "replace local";
    case SyncAction::MkdirRemote:   return "create remote directory";
    case SyncAction::MkdirLocal:    return "create local directory";
    case SyncAction::DeleteRemote:  return "DELETE remote";
    case SyncAction::DeleteLocal:   return "DELETE local";
    case SyncAction::Conflict:      return "conflict";
    case SyncAction::Skip:          return "skip";
    }
    return "unknown";
}

bool Deletes(SyncAction a)
{
    return a == SyncAction::DeleteRemote || a == SyncAction::DeleteLocal;
}

bool Destructive(SyncAction a)
{
    return Deletes(a) || a == SyncAction::ReplaceRemote ||
           a == SyncAction::ReplaceLocal;
}

bool SyncPlan::Ordered() const
{
    // Every directory creation must precede anything inside it, and every
    // deletion must follow everything inside it.
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const SyncStep& a = steps[i];
        for (size_t j = i + 1; j < steps.size(); ++j)
        {
            const SyncStep& b = steps[j];
            const bool bInsideA = b.path.size() > a.path.size() &&
                                  b.path.compare(0, a.path.size(), a.path) == 0 &&
                                  b.path[a.path.size()] == '/';
            if (!bInsideA)
                continue;
            // A creation of the parent before a child is right; a deletion of
            // the parent before a child is not.
            if (Deletes(a.action) && !Deletes(b.action))
                return false;
            if (Deletes(a.action) && Deletes(b.action))
                return false;      // parent deleted before its contents
        }
    }
    return true;
}

SyncPlan BuildPlan(const std::vector<ComparePair>& pairs, const SyncOptions& o)
{
    SyncPlan plan;
    std::vector<SyncStep> mkdirs, copies, deletes;

    const bool toRemote = o.direction == SyncDirection::LocalToRemote;
    const bool toLocal = o.direction == SyncDirection::RemoteToLocal;
    const bool twoWay = o.direction == SyncDirection::TwoWay;

    for (const ComparePair& p : pairs)
    {
        SyncStep s;
        s.path = p.path;
        s.why = CompareStateName(p.state);
        s.dir = (p.haveLocal && p.local.dir) || (p.haveRemote && p.remote.dir);

        switch (p.state)
        {
        case CompareState::Identical:
            s.action = SyncAction::Skip;
            break;

        case CompareState::LocalOnly:
            if (toRemote || twoWay)
            {
                if (p.local.dir)
                    s.action = SyncAction::MkdirRemote;
                else
                {
                    s.action = SyncAction::Upload;
                    s.bytes = p.local.size;
                }
            }
            else if (toLocal)
            {
                // Only the source's contents survive a mirror, and only when
                // the user asked for one.
                s.action = o.deleteExtraneous ? SyncAction::DeleteLocal
                                              : SyncAction::Skip;
            }
            break;

        case CompareState::RemoteOnly:
            if (toLocal || twoWay)
            {
                if (p.remote.dir)
                    s.action = SyncAction::MkdirLocal;
                else
                {
                    s.action = SyncAction::Download;
                    s.bytes = p.remote.size;
                }
            }
            else if (toRemote)
            {
                s.action = o.deleteExtraneous ? SyncAction::DeleteRemote
                                              : SyncAction::Skip;
            }
            break;

        case CompareState::LocalNewer:
            if (toRemote)
            {
                s.action = SyncAction::ReplaceRemote;
                s.bytes = p.local.size;
            }
            else if (toLocal)
            {
                // The destination is newer than the source. Replacing it
                // would throw away the newer copy, so a one-way sync in this
                // direction leaves it alone and says so.
                s.action = SyncAction::Skip;
                s.why = "local is newer — left alone";
            }
            else
            {
                s.action = SyncAction::ReplaceRemote;
                s.bytes = p.local.size;
            }
            break;

        case CompareState::RemoteNewer:
            if (toLocal)
            {
                s.action = SyncAction::ReplaceLocal;
                s.bytes = p.remote.size;
            }
            else if (toRemote)
            {
                s.action = SyncAction::Skip;
                s.why = "remote is newer — left alone";
            }
            else
            {
                s.action = SyncAction::ReplaceLocal;
                s.bytes = p.remote.size;
            }
            break;

        case CompareState::SizeDiffers:
            // The timestamps agree and the contents do not. Neither side can
            // be called newer, so a one-way sync overwrites the destination
            // and a two-way one has nothing to go on and must ask.
            if (toRemote)
            {
                s.action = SyncAction::ReplaceRemote;
                s.bytes = p.local.size;
            }
            else if (toLocal)
            {
                s.action = SyncAction::ReplaceLocal;
                s.bytes = p.remote.size;
            }
            else
                s.action = SyncAction::Conflict;
            break;

        case CompareState::TypeDiffers:
        case CompareState::Uncertain:
            // A file where a directory is expected, or a symlink, or a file
            // with no usable timestamp. Never resolved automatically: every
            // rule for these is wrong some of the time, and the wrong answer
            // destroys data.
            s.action = SyncAction::Conflict;
            break;
        }

        // Two-way conflict rules, applied only where the user chose one.
        if (twoWay && s.action == SyncAction::Conflict &&
            o.conflict != SyncOptions::ConflictRule::Ask &&
            p.state == CompareState::SizeDiffers)
        {
            switch (o.conflict)
            {
            case SyncOptions::ConflictRule::PreferLocal:
                s.action = SyncAction::ReplaceRemote;
                s.bytes = p.local.size;
                s.why = "conflict — local preferred";
                break;
            case SyncOptions::ConflictRule::PreferRemote:
                s.action = SyncAction::ReplaceLocal;
                s.bytes = p.remote.size;
                s.why = "conflict — remote preferred";
                break;
            case SyncOptions::ConflictRule::PreferNewer:
                // The sizes differ and the times do not, so there IS no newer.
                // The rule cannot apply and the conflict stands.
                break;
            case SyncOptions::ConflictRule::Ask:
                break;
            }
        }

        if (s.action == SyncAction::None)
            continue;
        if (s.action == SyncAction::MkdirLocal || s.action == SyncAction::MkdirRemote)
            mkdirs.push_back(std::move(s));
        else if (Deletes(s.action))
            deletes.push_back(std::move(s));
        else
            copies.push_back(std::move(s));
    }

    // Directories outermost-first, deletions innermost-first: that ordering
    // is what makes the plan safe to execute top to bottom.
    std::stable_sort(mkdirs.begin(), mkdirs.end(),
                     [](const SyncStep& a, const SyncStep& b)
                     { return Depth(a.path) < Depth(b.path); });
    std::stable_sort(deletes.begin(), deletes.end(),
                     [](const SyncStep& a, const SyncStep& b)
                     { return Depth(a.path) > Depth(b.path); });

    plan.steps.reserve(mkdirs.size() + copies.size() + deletes.size());
    for (auto& v : { &mkdirs, &copies, &deletes })
        for (SyncStep& s : *v)
            plan.steps.push_back(std::move(s));

    for (const SyncStep& s : plan.steps)
    {
        plan.bytes += s.bytes;
        if (s.action == SyncAction::Conflict)
            ++plan.conflicts;
        if (Destructive(s.action))
            ++plan.destructive;
        if (Deletes(s.action))
            ++plan.deletions;
    }
    return plan;
}

// ---------------------------------------------------------------- resume
const char* ResumeDecisionName(ResumeDecision d)
{
    switch (d)
    {
    case ResumeDecision::Fresh:   return "fresh";
    case ResumeDecision::Resume:  return "resume";
    case ResumeDecision::Restart: return "restart";
    }
    return "unknown";
}

ResumeDecision DecideResume(const ResumeCheck& c, std::string& why)
{
    why.clear();
    if (c.partialSize == 0)
    {
        why = "nothing transferred yet";
        return ResumeDecision::Fresh;
    }
    if (!c.serverSupportsResume)
    {
        why = "the server cannot seek — starting again";
        return ResumeDecision::Restart;
    }
    if (c.sourceSize == 0)
    {
        why = "the source size is unknown — starting again";
        return ResumeDecision::Restart;
    }
    if (c.partialSize > c.sourceSize)
    {
        // More bytes at the destination than the source has. Whatever is
        // there, it is not a prefix of this file.
        why = "the partial file is larger than the source — starting again";
        return ResumeDecision::Restart;
    }
    if (c.partialSize == c.sourceSize)
    {
        // Already the right length. It may still be the wrong content, which
        // is what verification is for — but there is nothing to append.
        why = "already complete — verify rather than resume";
        return ResumeDecision::Restart;
    }
    if (!c.haveRecord)
    {
        // The heart of "safe rather than optimistic". Without a record of the
        // source as it was when the partial was written, N bytes on disk
        // might be the first N bytes of a DIFFERENT version of the file, and
        // appending would produce a file that is corrupt in a way no size
        // check can detect.
        why = "no record of the source when the partial was written — "
              "starting again rather than guessing";
        return ResumeDecision::Restart;
    }
    if (c.recordedSize != c.sourceSize)
    {
        why = "the source has changed size since the partial was written — "
              "starting again";
        return ResumeDecision::Restart;
    }
    const int64_t diff = c.recordedMtime - c.sourceMtime;
    if (diff > c.mtimeToleranceSec || -diff > c.mtimeToleranceSec)
    {
        why = "the source has been modified since the partial was written — "
              "starting again";
        return ResumeDecision::Restart;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "resuming from %llu of %llu bytes",
             static_cast<unsigned long long>(c.partialSize),
             static_cast<unsigned long long>(c.sourceSize));
    why = buf;
    return ResumeDecision::Resume;
}

uint64_t ResumeOffset(const ResumeCheck& c, ResumeDecision d)
{
    return d == ResumeDecision::Resume ? c.partialSize : 0;
}

// ----------------------------------------------------------------- queue
const char* XferStateName(XferState s)
{
    switch (s)
    {
    case XferState::Queued:    return "queued";
    case XferState::Running:   return "running";
    case XferState::Verifying: return "verifying";
    case XferState::Done:      return "done";
    case XferState::Failed:    return "failed";
    case XferState::Cancelled: return "cancelled";
    case XferState::Paused:    return "paused";
    }
    return "unknown";
}

const char* VerifyStateName(VerifyState v)
{
    switch (v)
    {
    case VerifyState::NotRequested: return "-";
    case VerifyState::Pending:      return "checking";
    case VerifyState::Passed:       return "verified";
    case VerifyState::Failed:       return "MISMATCH";
    case VerifyState::Unavailable:  return "no hash available";
    }
    return "unknown";
}

bool TransferSucceeded(XferState s, VerifyState v, bool verifyRequired)
{
    if (s != XferState::Done)
        return false;
    if (!verifyRequired)
        return true;
    // Required verification that could not be performed is NOT success.
    // Calling it one would mean the guarantee the user asked for silently
    // became "we copied some bytes and hoped".
    return v == VerifyState::Passed;
}

// ----------------------------------------------------------------- retry
bool RetryableError(const std::string& error)
{
    if (error.empty())
        return false;
    // These fail the same way every time. Retrying them only delays the
    // report and hammers the server.
    static const char* kPermanent[] = {
        "permission denied", "no such file", "not found", "no space",
        "disk full", "quota", "file too large", "name too long",
        "is a directory", "not a directory", "read-only file system",
        "cancelled", "unsupported", "invalid handle",
    };
    for (const char* p : kPermanent)
        if (CaseInsensitiveFind(error, p))
            return false;
    // A dropped link, a timeout or a reset is exactly what retry is for.
    static const char* kTransient[] = {
        "timed out", "timeout", "connection", "reset", "broken pipe",
        "network", "socket", "would block", "eagain", "temporarily",
        "write failed", "read failed", "channel failure",
    };
    for (const char* t : kTransient)
        if (CaseInsensitiveFind(error, t))
            return true;
    // Unknown: retry once or twice rather than either giving up on a blip or
    // looping on something permanent. The attempt cap bounds it.
    return true;
}

bool ShouldRetry(const RetryPolicy& p, int attemptsMade, const std::string& error)
{
    if (attemptsMade >= p.maxAttempts)
        return false;
    if (attemptsMade < 1)
        return false;               // nothing has been attempted yet
    return RetryableError(error);
}

int RetryDelaySeconds(int attemptsMade)
{
    static const int kLadder[] = { 1, 3, 8 };
    const int n = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));
    if (attemptsMade < 1)
        attemptsMade = 1;
    return kLadder[std::min(attemptsMade, n) - 1];
}

// ---------------------------------------------------------- rate and ETA
uint64_t TransferRate(uint64_t done, double elapsedSec)
{
    // Under a fifth of a second there is not enough to divide by: an early
    // rate reading is noise, and noise in a speed column reads as a bug.
    if (elapsedSec < 0.2 || done == 0)
        return 0;
    return static_cast<uint64_t>(static_cast<double>(done) / elapsedSec);
}

int64_t TransferEta(uint64_t done, uint64_t total, double elapsedSec)
{
    if (total == 0 || done >= total)
        return 0;
    const uint64_t rate = TransferRate(done, elapsedSec);
    if (rate == 0)
        return -1;                  // not enough information to say
    return static_cast<int64_t>((total - done) / rate);
}

std::string FormatRate(uint64_t bytesPerSec)
{
    char b[32];
    if (bytesPerSec == 0)
        return "-";
    if (bytesPerSec < 1024ull)
        snprintf(b, sizeof(b), "%llu B/s", static_cast<unsigned long long>(bytesPerSec));
    else if (bytesPerSec < 1024ull * 1024ull)
        snprintf(b, sizeof(b), "%.1f KB/s", static_cast<double>(bytesPerSec) / 1024.0);
    else
        snprintf(b, sizeof(b), "%.1f MB/s",
                 static_cast<double>(bytesPerSec) / (1024.0 * 1024.0));
    return b;
}

std::string FormatEta(int64_t seconds)
{
    if (seconds < 0)
        return "-";
    char b[32];
    if (seconds < 60)
        snprintf(b, sizeof(b), "%llds", static_cast<long long>(seconds));
    else if (seconds < 3600)
        snprintf(b, sizeof(b), "%lldm%02llds", static_cast<long long>(seconds / 60),
                 static_cast<long long>(seconds % 60));
    else
        snprintf(b, sizeof(b), "%lldh%02lldm", static_cast<long long>(seconds / 3600),
                 static_cast<long long>((seconds % 3600) / 60));
    return b;
}

} // namespace amber
