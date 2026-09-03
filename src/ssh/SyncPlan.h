// SyncPlan.h — directory comparison, the synchronisation planner, resume
// safety and the transfer retry rules.
//
// All of it is pure: no sockets, no libssh2, no Windows file handles, no UI.
// That is deliberate. The dangerous decisions in Stage 6 are not the byte
// pushing — they are "are these two files the same", "is it safe to append to
// this partial file", "which of these steps deletes something" and "is this
// failure worth retrying". Every one of those is a function of metadata, so
// every one can be tested exhaustively without a server.
//
// The comparison deliberately does not know which side is local: it is given
// two flat lists of relative paths and returns a classification. That is what
// makes a two-way compare the same code as a one-way one.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// A file or directory reduced to what a comparison needs. `mode` is 0 when
// the side cannot report Unix permissions, which is the normal case for a
// Windows local file — the planner then knows not to claim it preserved them.
struct SyncEntry
{
    std::string path;        // relative to the sync root, '/'-separated
    bool dir = false;
    bool link = false;
    uint64_t size = 0;
    int64_t mtime = 0;       // Unix seconds; 0 = unknown
    uint32_t mode = 0;       // Unix mode bits, 0 = not available
};

// ------------------------------------------------------------- comparison
enum class CompareState
{
    Identical,     // same type, same size, mtimes within tolerance
    LocalOnly,
    RemoteOnly,
    LocalNewer,
    RemoteNewer,
    SizeDiffers,   // mtimes agree but the sizes do not — something is wrong
    TypeDiffers,   // a file on one side, a directory on the other
    Uncertain,     // a symlink, or a timestamp that cannot be trusted
};

const char* CompareStateName(CompareState s);

struct ComparePair
{
    std::string path;
    CompareState state = CompareState::Identical;
    bool haveLocal = false, haveRemote = false;
    SyncEntry local, remote;
    // True when both sides are directories: the planner creates those first
    // and never "transfers" them.
    bool BothDirs() const { return haveLocal && haveRemote && local.dir && remote.dir; }
};

struct CompareOptions
{
    // Timestamps are the crux of any sync tool. SFTP reports whole seconds,
    // FAT and exFAT store two-second granularity, and a server with the wrong
    // timezone rules can be an hour out twice a year. Two mtimes inside this
    // window are treated as the same time.
    int64_t mtimeToleranceSec = 2;
    // With this off, matching size and mtime is not enough to call two files
    // identical — the caller must hash them. Off is slower and certain.
    bool trustSizeAndTime = true;
    // Relative paths matching any of these are left out of the comparison
    // entirely, so they can be neither transferred nor deleted.
    std::vector<std::string> excludeGlobs;
    // A symlink is reported Uncertain rather than followed: following one
    // during a sync is how a tool ends up copying a filesystem into a
    // subdirectory of itself.
    bool followLinks = false;
};

// Glob matching over a relative path. `*` and `?` match within one segment,
// `**` matches across segments, a leading `/` anchors at the root, and a
// trailing `/` matches directories only.
bool MatchGlob(const std::string& pattern, const std::string& path, bool isDir);
bool Excluded(const CompareOptions& o, const std::string& relPath, bool isDir);

// Classifies every path present on either side. Non-destructive by
// construction: it returns a description and touches nothing.
std::vector<ComparePair> Compare(const std::vector<SyncEntry>& local,
                                 const std::vector<SyncEntry>& remote,
                                 const CompareOptions& o);

// --------------------------------------------------------------- planner
enum class SyncDirection { LocalToRemote, RemoteToLocal, TwoWay };

enum class SyncAction
{
    None,
    Upload,          // the file does not exist remotely
    Download,        // the file does not exist locally
    ReplaceRemote,
    ReplaceLocal,
    MkdirRemote,
    MkdirLocal,
    DeleteRemote,    // only ever with deleteExtraneous
    DeleteLocal,     // only ever with deleteExtraneous
    Conflict,        // two-way, both sides changed: the user decides
    Skip,            // reported so the plan accounts for everything it saw
};

const char* SyncActionName(SyncAction a);
// True for anything that removes or overwrites data. The plan counts these
// separately and the UI must show them differently.
bool Destructive(SyncAction a);
// True for an action that only removes data.
bool Deletes(SyncAction a);

struct SyncOptions
{
    SyncDirection direction = SyncDirection::LocalToRemote;
    // Deleting files the source does not have turns a copy into a mirror.
    // Off unless the user says otherwise, every time.
    bool deleteExtraneous = false;
    bool preserveTimes = true;
    bool preserveMode = true;
    // Two-way only. Ask is the default because an automatic merge policy for
    // "both sides changed" cannot be right in general — there is no rule that
    // does not sometimes throw away the edit the user cared about.
    enum class ConflictRule { Ask, PreferNewer, PreferLocal, PreferRemote };
    ConflictRule conflict = ConflictRule::Ask;
};

struct SyncStep
{
    SyncAction action = SyncAction::None;
    std::string path;
    bool dir = false;
    uint64_t bytes = 0;       // what will actually move
    std::string why;          // the comparison state that produced this step
};

struct SyncPlan
{
    std::vector<SyncStep> steps;
    size_t conflicts = 0;
    size_t destructive = 0;
    size_t deletions = 0;
    uint64_t bytes = 0;
    // Directories are created before anything inside them and deleted after
    // everything inside them, so the plan is safe to execute in order. Held
    // as an invariant the tests check rather than as a hope.
    bool Ordered() const;
};

SyncPlan BuildPlan(const std::vector<ComparePair>& pairs, const SyncOptions& o);

// ---------------------------------------------------------------- resume
// Whether a partially transferred file can be appended to.
enum class ResumeDecision
{
    Fresh,     // nothing there: start at zero
    Resume,    // append from ResumeCheck::partialSize
    Restart,   // something is there but cannot be trusted: truncate and redo
};

const char* ResumeDecisionName(ResumeDecision d);

// What is known about a partial transfer. `haveRecord` is the crux: a
// resume is only safe when the source's size and mtime were RECORDED when
// the partial was written and still match now. Without that record, N bytes
// on disk might be the first N bytes of a different version of the file, and
// appending would produce a file that is corrupt in a way no size check can
// see. That is why "no record" means Restart and not Resume.
struct ResumeCheck
{
    uint64_t partialSize = 0;
    uint64_t sourceSize = 0;
    int64_t sourceMtime = 0;
    bool haveRecord = false;
    uint64_t recordedSize = 0;
    int64_t recordedMtime = 0;
    // Servers that cannot seek, or a source whose size is unknown, make
    // resume impossible whatever the metadata says.
    bool serverSupportsResume = true;
    int64_t mtimeToleranceSec = 2;
};

// Returns the decision and, in `why`, the reason — which the queue shows, so
// a restart never looks like an unexplained loss of progress.
ResumeDecision DecideResume(const ResumeCheck& c, std::string& why);
// The offset to start from, given a decision. Always 0 unless resuming.
uint64_t ResumeOffset(const ResumeCheck& c, ResumeDecision d);

// ----------------------------------------------------------------- queue
enum class XferState
{
    Queued,
    Running,
    Verifying,     // transferred, hashing before it is called done
    Done,
    Failed,
    Cancelled,
    Paused,
};

const char* XferStateName(XferState s);

enum class VerifyState
{
    NotRequested,
    Pending,
    Passed,
    Failed,
    Unavailable,   // neither end could produce a hash — reported, not assumed
};

const char* VerifyStateName(VerifyState v);
// A transfer is only successful when any REQUIRED verification passed.
// Unavailable is not success when verification was required.
bool TransferSucceeded(XferState s, VerifyState v, bool verifyRequired);

// ----------------------------------------------------------------- retry
struct RetryPolicy
{
    int maxAttempts = 3;         // 1 = no retry
    bool retryOnVerifyFailure = true;
};

// Whether a failure is worth another attempt. Split from the state machine so
// the classification can be tested against the strings the SFTP layer really
// produces.
//
// A permission error, a missing path, a full disk or a name that is too long
// will fail identically on every attempt; retrying them just delays the
// report. A dropped connection or a timeout is exactly what retry is for.
bool RetryableError(const std::string& error);
bool ShouldRetry(const RetryPolicy& p, int attemptsMade, const std::string& error);
// 1s, 3s, 8s — short, because a queue that stalls for a minute per item looks
// broken.
int RetryDelaySeconds(int attemptsMade);

// --------------------------------------------------------- rate and ETA
// Bytes per second over the whole transfer, and the seconds left at that
// rate. Both return 0 when there is not enough information to say — never a
// guess, because an ETA that swings wildly is worse than no ETA.
uint64_t TransferRate(uint64_t done, double elapsedSec);
int64_t TransferEta(uint64_t done, uint64_t total, double elapsedSec);
std::string FormatRate(uint64_t bytesPerSec);
std::string FormatEta(int64_t seconds);

} // namespace amber
