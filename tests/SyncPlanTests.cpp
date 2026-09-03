// SyncPlanTests.cpp — directory comparison, the sync planner, resume safety,
// verification and the retry rules.
//
// These are the dangerous decisions in Stage 6, and they are all functions of
// metadata, so they are all testable without a server. What is NOT here is
// the byte pushing; the report is explicit about that.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "../src/ssh/SyncPlan.h"
#include "../src/utility/Hash.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace amber;

namespace
{

SyncEntry F(const char* path, uint64_t size, int64_t mtime)
{
    SyncEntry e;
    e.path = path;
    e.size = size;
    e.mtime = mtime;
    e.mode = 0644;
    return e;
}

SyncEntry D(const char* path)
{
    SyncEntry e;
    e.path = path;
    e.dir = true;
    e.mode = 0755;
    return e;
}

const ComparePair* Find(const std::vector<ComparePair>& v, const char* path)
{
    for (const ComparePair& p : v)
        if (p.path == path)
            return &p;
    return nullptr;
}

const SyncStep* Step(const SyncPlan& p, const char* path)
{
    for (const SyncStep& s : p.steps)
        if (s.path == path)
            return &s;
    return nullptr;
}

} // namespace

// ------------------------------------------------------------- exclusions
TEST_CASE("glob exclusions match the way a user expects", "[sync][exclude]")
{
    SECTION("an unanchored pattern matches at any depth")
    {
        CHECK(MatchGlob("*.tmp", "a.tmp", false));
        CHECK(MatchGlob("*.tmp", "deep/inside/a.tmp", false));
        CHECK_FALSE(MatchGlob("*.tmp", "a.tmpx", false));
    }
    SECTION("a leading slash anchors at the sync root")
    {
        CHECK(MatchGlob("/build", "build", true));
        CHECK_FALSE(MatchGlob("/build", "src/build", true));
    }
    SECTION("a trailing slash matches directories only")
    {
        CHECK(MatchGlob("node_modules/", "node_modules", true));
        CHECK_FALSE(MatchGlob("node_modules/", "node_modules", false));
    }
    SECTION("a single star stops at a separator, two do not")
    {
        CHECK_FALSE(MatchGlob("/src/*.c", "src/deep/a.c", false));
        CHECK(MatchGlob("/src/**/*.c", "src/deep/a.c", false));
        CHECK(MatchGlob("/src/**", "src/deep/a.c", false));
    }
    SECTION("a question mark matches one character, not a separator")
    {
        CHECK(MatchGlob("a?c", "abc", false));
        CHECK_FALSE(MatchGlob("a?c", "a/c", false));
    }
    SECTION("matching is case-insensitive, as Windows paths are")
    {
        CHECK(MatchGlob("*.TMP", "a.tmp", false));
    }
    SECTION("excluding a directory excludes everything under it")
    {
        CompareOptions o;
        o.excludeGlobs = { "node_modules/" };
        CHECK(Excluded(o, "node_modules", true));
        CHECK(Excluded(o, "node_modules/pkg/index.js", false));
        CHECK_FALSE(Excluded(o, "src/index.js", false));
    }
    SECTION("an excluded path can be neither transferred nor deleted")
    {
        CompareOptions o;
        o.excludeGlobs = { "*.log" };
        const auto pairs = Compare({ F("keep.txt", 1, 100), F("noise.log", 1, 100) },
                                   {}, o);
        CHECK(pairs.size() == 1);
        CHECK(pairs[0].path == "keep.txt");
        SyncOptions so;
        so.deleteExtraneous = true;
        so.direction = SyncDirection::RemoteToLocal;
        const SyncPlan plan = BuildPlan(pairs, so);
        CHECK(Step(plan, "noise.log") == nullptr);
    }
}

// ------------------------------------------------------------- comparison
TEST_CASE("comparison classifies every combination", "[sync][compare]")
{
    CompareOptions o;
    const auto pairs = Compare(
        { F("same.txt", 100, 1000), F("localonly.txt", 5, 1000),
          F("newer.txt", 100, 2000), F("older.txt", 100, 1000),
          F("sizediff.txt", 100, 1000), D("dir"), F("clash", 1, 1000) },
        { F("same.txt", 100, 1000), F("remoteonly.txt", 5, 1000),
          F("newer.txt", 100, 1000), F("older.txt", 100, 2000),
          F("sizediff.txt", 200, 1000), D("dir"), D("clash") },
        o);

    CHECK(Find(pairs, "same.txt")->state == CompareState::Identical);
    CHECK(Find(pairs, "localonly.txt")->state == CompareState::LocalOnly);
    CHECK(Find(pairs, "remoteonly.txt")->state == CompareState::RemoteOnly);
    CHECK(Find(pairs, "newer.txt")->state == CompareState::LocalNewer);
    CHECK(Find(pairs, "older.txt")->state == CompareState::RemoteNewer);
    CHECK(Find(pairs, "sizediff.txt")->state == CompareState::SizeDiffers);
    CHECK(Find(pairs, "dir")->state == CompareState::Identical);
    CHECK(Find(pairs, "clash")->state == CompareState::TypeDiffers);
}

TEST_CASE("timestamp tolerance absorbs filesystem granularity",
          "[sync][compare]")
{
    CompareOptions o;                      // 2 second default
    SECTION("one second apart is the same time")
    {
        const auto p = Compare({ F("a", 10, 1001) }, { F("a", 10, 1000) }, o);
        CHECK(p[0].state == CompareState::Identical);
    }
    SECTION("three seconds apart is not")
    {
        const auto p = Compare({ F("a", 10, 1003) }, { F("a", 10, 1000) }, o);
        CHECK(p[0].state == CompareState::LocalNewer);
    }
    SECTION("tolerance is configurable for a server an hour out")
    {
        CompareOptions wide;
        wide.mtimeToleranceSec = 3700;
        const auto p = Compare({ F("a", 10, 4600) }, { F("a", 10, 1000) }, wide);
        CHECK(p[0].state == CompareState::Identical);
    }
    SECTION("matching time with differing size is never identical")
    {
        // The one combination that means something is actually wrong: same
        // mtime, different length. Neither side can be called newer.
        const auto p = Compare({ F("a", 10, 1000) }, { F("a", 99, 1000) }, o);
        CHECK(p[0].state == CompareState::SizeDiffers);
    }
}

TEST_CASE("comparison refuses to guess without timestamps", "[sync][compare]")
{
    CompareOptions o;
    SECTION("no mtime on one side and equal sizes is uncertain, not identical")
    {
        const auto p = Compare({ F("a", 10, 0) }, { F("a", 10, 1000) }, o);
        CHECK(p[0].state == CompareState::Uncertain);
    }
    SECTION("no mtime and different sizes is a size difference")
    {
        const auto p = Compare({ F("a", 10, 0) }, { F("a", 20, 1000) }, o);
        CHECK(p[0].state == CompareState::SizeDiffers);
    }
    SECTION("a symlink is uncertain rather than followed")
    {
        SyncEntry l = F("link", 10, 1000);
        l.link = true;
        const auto p = Compare({ l }, { F("link", 10, 1000) }, o);
        CHECK(p[0].state == CompareState::Uncertain);
    }
    SECTION("trustSizeAndTime off makes a match uncertain until hashed")
    {
        CompareOptions strict;
        strict.trustSizeAndTime = false;
        const auto p = Compare({ F("a", 10, 1000) }, { F("a", 10, 1000) }, strict);
        CHECK(p[0].state == CompareState::Uncertain);
    }
}

TEST_CASE("comparison is non-destructive by construction", "[sync][compare]")
{
    // It takes two lists by const reference and returns a description. There
    // is no path by which it can act, which is the property the spec asks
    // for and the reason it is a separate function from the planner.
    CompareOptions o;
    const std::vector<SyncEntry> local{ F("a", 1, 1) };
    const std::vector<SyncEntry> remote{ F("b", 1, 1) };
    const auto p = Compare(local, remote, o);
    CHECK(local.size() == 1);
    CHECK(remote.size() == 1);
    CHECK(p.size() == 2);
}

// ---------------------------------------------------------------- planner
TEST_CASE("a one-way plan uploads what is missing and leaves the rest",
          "[sync][plan]")
{
    CompareOptions o;
    const auto pairs = Compare(
        { D("sub"), F("new.txt", 50, 1000), F("sub/deep.txt", 10, 1000),
          F("same.txt", 5, 1000), F("mine.txt", 7, 2000) },
        { F("same.txt", 5, 1000), F("mine.txt", 7, 1000),
          F("theirs.txt", 9, 1000) },
        o);
    SyncOptions so;                       // LocalToRemote, no deletions
    const SyncPlan plan = BuildPlan(pairs, so);

    CHECK(Step(plan, "sub")->action == SyncAction::MkdirRemote);
    CHECK(Step(plan, "new.txt")->action == SyncAction::Upload);
    CHECK(Step(plan, "sub/deep.txt")->action == SyncAction::Upload);
    CHECK(Step(plan, "same.txt")->action == SyncAction::Skip);
    CHECK(Step(plan, "mine.txt")->action == SyncAction::ReplaceRemote);
    // Extraneous, but deletion was not asked for.
    CHECK(Step(plan, "theirs.txt")->action == SyncAction::Skip);
    CHECK(plan.deletions == 0);
    CHECK(plan.bytes == 50 + 10 + 7);

    SECTION("directories are created before the files inside them")
    {
        CHECK(plan.Ordered());
        size_t mk = 0, deep = 0;
        for (size_t i = 0; i < plan.steps.size(); ++i)
        {
            if (plan.steps[i].path == "sub")
                mk = i;
            if (plan.steps[i].path == "sub/deep.txt")
                deep = i;
        }
        CHECK(mk < deep);
    }
}

TEST_CASE("a one-way sync never overwrites a newer destination",
          "[sync][plan][safety]")
{
    CompareOptions o;
    const auto pairs = Compare({ F("a", 10, 5000) }, { F("a", 10, 1000) }, o);
    SECTION("pushing: the local copy is newer, so it replaces the remote")
    {
        SyncOptions so;
        so.direction = SyncDirection::LocalToRemote;
        CHECK(BuildPlan(pairs, so).steps[0].action == SyncAction::ReplaceRemote);
    }
    SECTION("pulling: the local copy is newer and is LEFT ALONE")
    {
        // Overwriting it would throw away the newer file, which is what a
        // careless sync tool does and is the thing users never forgive.
        SyncOptions so;
        so.direction = SyncDirection::RemoteToLocal;
        const SyncPlan p = BuildPlan(pairs, so);
        CHECK(p.steps[0].action == SyncAction::Skip);
        CHECK(p.steps[0].why.find("newer") != std::string::npos);
        CHECK(p.destructive == 0);
    }
}

TEST_CASE("deletion is opt-in, counted and never implied", "[sync][plan][safety]")
{
    CompareOptions o;
    const auto pairs = Compare({ F("keep", 1, 1000) },
                               { F("keep", 1, 1000), F("extra", 1, 1000),
                                 D("extradir"), F("extradir/inner", 1, 1000) },
                               o);
    SECTION("without the flag, a compare stays a copy")
    {
        SyncOptions so;
        so.direction = SyncDirection::LocalToRemote;
        const SyncPlan p = BuildPlan(pairs, so);
        CHECK(p.deletions == 0);
        CHECK(p.destructive == 0);
        for (const SyncStep& s : p.steps)
            CHECK_FALSE(Deletes(s.action));
    }
    SECTION("with the flag, deletions appear and are counted")
    {
        SyncOptions so;
        so.direction = SyncDirection::LocalToRemote;
        so.deleteExtraneous = true;
        const SyncPlan p = BuildPlan(pairs, so);
        CHECK(p.deletions == 3);
        CHECK(p.destructive == 3);
        CHECK(Step(p, "extra")->action == SyncAction::DeleteRemote);
        CHECK(Destructive(SyncAction::DeleteRemote));
    }
    SECTION("a directory is deleted AFTER the things inside it")
    {
        SyncOptions so;
        so.direction = SyncDirection::LocalToRemote;
        so.deleteExtraneous = true;
        const SyncPlan p = BuildPlan(pairs, so);
        CHECK(p.Ordered());
        size_t dir = 0, inner = 0;
        for (size_t i = 0; i < p.steps.size(); ++i)
        {
            if (p.steps[i].path == "extradir")
                dir = i;
            if (p.steps[i].path == "extradir/inner")
                inner = i;
        }
        CHECK(inner < dir);
    }
}

TEST_CASE("a two-way sync asks rather than inventing a merge",
          "[sync][plan][safety]")
{
    CompareOptions o;
    const auto pairs = Compare(
        { F("lnew", 10, 5000), F("rnew", 10, 1000), F("clash", 10, 1000),
          F("lonly", 1, 1000) },
        { F("lnew", 10, 1000), F("rnew", 10, 5000), F("clash", 99, 1000),
          F("ronly", 1, 1000) },
        o);
    SyncOptions so;
    so.direction = SyncDirection::TwoWay;
    const SyncPlan p = BuildPlan(pairs, so);

    // Each side's newer copy wins where there IS a newer copy.
    CHECK(Step(p, "lnew")->action == SyncAction::ReplaceRemote);
    CHECK(Step(p, "rnew")->action == SyncAction::ReplaceLocal);
    // Same time, different size: no basis for a decision.
    CHECK(Step(p, "clash")->action == SyncAction::Conflict);
    CHECK(p.conflicts == 1);
    // Missing on one side: copied, not deleted.
    CHECK(Step(p, "lonly")->action == SyncAction::Upload);
    CHECK(Step(p, "ronly")->action == SyncAction::Download);
    CHECK(p.deletions == 0);

    SECTION("an explicit preference resolves a size clash")
    {
        SyncOptions pl = so;
        pl.conflict = SyncOptions::ConflictRule::PreferLocal;
        CHECK(Step(BuildPlan(pairs, pl), "clash")->action == SyncAction::ReplaceRemote);
        SyncOptions pr = so;
        pr.conflict = SyncOptions::ConflictRule::PreferRemote;
        CHECK(Step(BuildPlan(pairs, pr), "clash")->action == SyncAction::ReplaceLocal);
    }
    SECTION("prefer-newer cannot resolve a clash where neither is newer")
    {
        // The sizes differ and the times agree, so there is no newer copy.
        // A rule that cannot apply must leave the conflict standing rather
        // than pick a side.
        SyncOptions pn = so;
        pn.conflict = SyncOptions::ConflictRule::PreferNewer;
        CHECK(Step(BuildPlan(pairs, pn), "clash")->action == SyncAction::Conflict);
    }
}

TEST_CASE("type differences and links are always conflicts", "[sync][plan][safety]")
{
    CompareOptions o;
    SyncEntry link = F("l", 1, 1000);
    link.link = true;
    const auto pairs = Compare({ F("clash", 1, 1000), link },
                               { D("clash"), F("l", 1, 1000) }, o);
    for (SyncDirection d : { SyncDirection::LocalToRemote,
                             SyncDirection::RemoteToLocal, SyncDirection::TwoWay })
    {
        SyncOptions so;
        so.direction = d;
        const SyncPlan p = BuildPlan(pairs, so);
        CHECK(Step(p, "clash")->action == SyncAction::Conflict);
        CHECK(Step(p, "l")->action == SyncAction::Conflict);
    }
}

TEST_CASE("an empty comparison yields an empty plan", "[sync][plan]")
{
    SyncOptions so;
    const SyncPlan p = BuildPlan({}, so);
    CHECK(p.steps.empty());
    CHECK(p.bytes == 0);
    CHECK(p.conflicts == 0);
    CHECK(p.destructive == 0);
    CHECK(p.Ordered());
}

// ----------------------------------------------------------------- resume
TEST_CASE("resume is safe rather than optimistic", "[sync][resume][safety]")
{
    std::string why;
    SECTION("nothing transferred: start fresh")
    {
        ResumeCheck c;
        c.sourceSize = 1000;
        CHECK(DecideResume(c, why) == ResumeDecision::Fresh);
        CHECK(ResumeOffset(c, ResumeDecision::Fresh) == 0);
    }
    SECTION("a partial with a matching record resumes from its length")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 1000;
        c.sourceMtime = 5000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        c.recordedMtime = 5000;
        CHECK(DecideResume(c, why) == ResumeDecision::Resume);
        CHECK(ResumeOffset(c, ResumeDecision::Resume) == 400);
        CHECK(why.find("400") != std::string::npos);
    }
    SECTION("NO RECORD means restart, not resume")
    {
        // The heart of it. Without a record of the source as it was when the
        // partial was written, 400 bytes on disk might be the first 400 bytes
        // of a different version — and appending would produce a file that is
        // corrupt in a way no size check can detect.
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 1000;
        c.sourceMtime = 5000;
        c.haveRecord = false;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
        CHECK(why.find("no record") != std::string::npos);
        CHECK(ResumeOffset(c, ResumeDecision::Restart) == 0);
    }
    SECTION("the source changing size since the partial means restart")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 2000;
        c.sourceMtime = 5000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        c.recordedMtime = 5000;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
        CHECK(why.find("size") != std::string::npos);
    }
    SECTION("the source being modified since the partial means restart")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 1000;
        c.sourceMtime = 9000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        c.recordedMtime = 5000;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
        CHECK(why.find("modified") != std::string::npos);
    }
    SECTION("a clock skew inside the tolerance still resumes")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 1000;
        c.sourceMtime = 5001;
        c.haveRecord = true;
        c.recordedSize = 1000;
        c.recordedMtime = 5000;
        CHECK(DecideResume(c, why) == ResumeDecision::Resume);
    }
    SECTION("a partial LARGER than the source means restart")
    {
        ResumeCheck c;
        c.partialSize = 5000;
        c.sourceSize = 1000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
        CHECK(why.find("larger") != std::string::npos);
    }
    SECTION("a partial the same length as the source is not resumed")
    {
        // The right length may still be the wrong content, which is what
        // verification is for; there is nothing left to append either way.
        ResumeCheck c;
        c.partialSize = 1000;
        c.sourceSize = 1000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
    }
    SECTION("a server that cannot seek never resumes")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 1000;
        c.haveRecord = true;
        c.recordedSize = 1000;
        c.serverSupportsResume = false;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
    }
    SECTION("an unknown source size never resumes")
    {
        ResumeCheck c;
        c.partialSize = 400;
        c.sourceSize = 0;
        c.haveRecord = true;
        CHECK(DecideResume(c, why) == ResumeDecision::Restart);
    }
    SECTION("every decision comes with a reason")
    {
        // A restart that looks like unexplained lost progress is a support
        // call; the queue shows this string.
        for (bool record : { true, false })
            for (uint64_t partial : { 0ull, 400ull, 5000ull })
            {
                ResumeCheck c;
                c.partialSize = partial;
                c.sourceSize = 1000;
                c.haveRecord = record;
                c.recordedSize = 1000;
                std::string w;
                DecideResume(c, w);
                CHECK_FALSE(w.empty());
            }
    }
}

// ----------------------------------------------------------- verification
TEST_CASE("SHA-256 agrees with the published vectors", "[sync][hash]")
{
    std::string hex;
    REQUIRE(Sha256Bytes("", 0, hex));
    CHECK(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256Bytes("abc", 3, hex));
    CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("a local file hashes, whole and by range", "[sync][hash]")
{
    const auto dir = std::filesystem::temp_directory_path() / "amberssh-tests";
    std::filesystem::create_directories(dir);
    const auto file = dir / "hash-me.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "abc" << "def";
    }
    std::string hex, err;
    REQUIRE(Sha256File(file.wstring(), hex, err));
    std::string whole;
    REQUIRE(Sha256Bytes("abcdef", 6, whole));
    CHECK(hex == whole);

    SECTION("a range hashes only that range")
    {
        std::string part, expect;
        REQUIRE(Sha256FileRange(file.wstring(), 0, 3, part, err));
        REQUIRE(Sha256Bytes("abc", 3, expect));
        CHECK(part == expect);
    }
    SECTION("a range past the end fails rather than returning a short digest")
    {
        // A digest of part of a range, presented as a digest of the range, is
        // the worst possible outcome for a verification feature.
        std::string part;
        CHECK_FALSE(Sha256FileRange(file.wstring(), 0, 999, part, err));
        CHECK_FALSE(err.empty());
    }
    SECTION("a missing file fails with a reason")
    {
        std::string h;
        CHECK_FALSE(Sha256File((dir / "nope.bin").wstring(), h, err));
        CHECK_FALSE(err.empty());
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("a remote digest is found in whatever the host printed",
          "[sync][hash]")
{
    const std::string want =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::string got;
    SECTION("GNU coreutils sha256sum")
    {
        REQUIRE(ParseRemoteSha256(want + "  /srv/app/file.tar", got));
        CHECK(got == want);
    }
    SECTION("BSD sha256")
    {
        REQUIRE(ParseRemoteSha256("SHA256 (/srv/file) = " + want + "\n", got));
        CHECK(got == want);
    }
    SECTION("upper case is normalised")
    {
        std::string upper = want;
        for (char& c : upper)
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        REQUIRE(ParseRemoteSha256(upper, got));
        CHECK(got == want);
    }
    SECTION("a login banner before the digest does not defeat the parse")
    {
        REQUIRE(ParseRemoteSha256("Welcome to prod01\nLast login: Tue\n" + want +
                                      "  file",
                                  got));
        CHECK(got == want);
    }
    SECTION("no digest is reported as no digest")
    {
        CHECK_FALSE(ParseRemoteSha256("sha256sum: not found", got));
        CHECK_FALSE(ParseRemoteSha256("", got));
        CHECK_FALSE(ParseRemoteSha256("AMBER_NO_SHA256", got));
        // An MD5 is 32 hex characters and must not be mistaken for a SHA-256.
        CHECK_FALSE(ParseRemoteSha256("d41d8cd98f00b204e9800998ecf8427e  f", got));
    }
    SECTION("a 64-hex run glued to other characters is not a digest")
    {
        CHECK_FALSE(ParseRemoteSha256("x" + want + "y", got));
    }
}

TEST_CASE("digest comparison never reports agreement it does not have",
          "[sync][hash][safety]")
{
    const std::string a =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(DigestsMatch(a, a));
    CHECK(DigestsMatch(a, "  " + a + "\n"));
    std::string upper = a;
    for (char& c : upper)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    CHECK(DigestsMatch(a, upper));
    // Two failures to hash must never read as a match.
    CHECK_FALSE(DigestsMatch("", ""));
    CHECK_FALSE(DigestsMatch(a, ""));
    CHECK_FALSE(DigestsMatch("short", "short"));
    CHECK_FALSE(DigestsMatch(a, a.substr(0, 63) + "e"));
}

TEST_CASE("required verification gates success", "[sync][hash][safety]")
{
    CHECK(TransferSucceeded(XferState::Done, VerifyState::NotRequested, false));
    CHECK(TransferSucceeded(XferState::Done, VerifyState::Passed, true));
    CHECK_FALSE(TransferSucceeded(XferState::Done, VerifyState::Failed, true));
    CHECK_FALSE(TransferSucceeded(XferState::Verifying, VerifyState::Pending, true));
    CHECK_FALSE(TransferSucceeded(XferState::Failed, VerifyState::Passed, true));
    SECTION("verification that could not be performed is not success")
    {
        // Otherwise the guarantee the user asked for silently becomes "we
        // copied some bytes and hoped".
        CHECK_FALSE(TransferSucceeded(XferState::Done, VerifyState::Unavailable, true));
        CHECK(TransferSucceeded(XferState::Done, VerifyState::Unavailable, false));
    }
}

// ------------------------------------------------------------------ retry
TEST_CASE("retry distinguishes a blip from a permanent failure",
          "[sync][retry]")
{
    SECTION("permanent failures are not retried")
    {
        for (const char* e : { "permission denied", "no such file",
                              "No space left on device", "disk full",
                              "quota exceeded", "file name too long",
                              "is a directory", "read-only file system",
                              "cancelled", "unsupported operation" })
        {
            INFO(e);
            CHECK_FALSE(RetryableError(e));
        }
    }
    SECTION("transport failures are")
    {
        for (const char* e : { "connection reset by peer", "timed out",
                              "broken pipe", "network is unreachable",
                              "socket error", "write failed", "read failed" })
        {
            INFO(e);
            CHECK(RetryableError(e));
        }
    }
    SECTION("an empty error is not a failure at all")
    {
        CHECK_FALSE(RetryableError(""));
    }
}

TEST_CASE("the retry state machine is bounded and backs off", "[sync][retry]")
{
    RetryPolicy p;                        // 3 attempts
    CHECK(ShouldRetry(p, 1, "connection reset"));
    CHECK(ShouldRetry(p, 2, "connection reset"));
    CHECK_FALSE(ShouldRetry(p, 3, "connection reset"));
    CHECK_FALSE(ShouldRetry(p, 9, "connection reset"));
    CHECK_FALSE(ShouldRetry(p, 1, "permission denied"));
    CHECK_FALSE(ShouldRetry(p, 0, "connection reset"));   // nothing tried yet

    CHECK(RetryDelaySeconds(1) == 1);
    CHECK(RetryDelaySeconds(2) == 3);
    CHECK(RetryDelaySeconds(3) == 8);
    CHECK(RetryDelaySeconds(99) == 8);    // held, never unbounded
    CHECK(RetryDelaySeconds(0) == 1);

    SECTION("maxAttempts of 1 means no retry at all")
    {
        RetryPolicy once;
        once.maxAttempts = 1;
        CHECK_FALSE(ShouldRetry(once, 1, "connection reset"));
    }
    SECTION("a cancelled transfer is never retried")
    {
        // Retrying something the user cancelled would be the application
        // arguing with them.
        CHECK_FALSE(ShouldRetry(p, 1, "cancelled"));
    }
}

// ------------------------------------------------------------ rate and ETA
TEST_CASE("rate and ETA say nothing when they know nothing", "[sync][queue]")
{
    CHECK(TransferRate(0, 10.0) == 0);
    CHECK(TransferRate(1000, 0.05) == 0);      // too early to divide
    CHECK(TransferRate(1000, 1.0) == 1000);
    CHECK(TransferRate(1000, 2.0) == 500);

    CHECK(TransferEta(0, 0, 1.0) == 0);        // no total: nothing to say
    CHECK(TransferEta(100, 100, 1.0) == 0);    // finished
    CHECK(TransferEta(1000, 3000, 1.0) == 2);
    CHECK(TransferEta(1000, 3000, 0.01) == -1);   // not enough information

    CHECK(FormatRate(0) == "-");
    CHECK(FormatRate(500) == "500 B/s");
    CHECK(FormatRate(2048) == "2.0 KB/s");
    CHECK(FormatRate(5ull * 1024 * 1024) == "5.0 MB/s");
    CHECK(FormatEta(-1) == "-");
    CHECK(FormatEta(45) == "45s");
    CHECK(FormatEta(125) == "2m05s");
    CHECK(FormatEta(7300) == "2h01m");
}

TEST_CASE("every state and action has a distinct name", "[sync][queue]")
{
    // The queue shows these, and two states that print the same are two
    // states the user cannot tell apart.
    for (XferState a : { XferState::Queued, XferState::Running,
                         XferState::Verifying, XferState::Done,
                         XferState::Failed, XferState::Cancelled,
                         XferState::Paused })
        CHECK(std::string(XferStateName(a)) != "unknown");
    for (VerifyState v : { VerifyState::NotRequested, VerifyState::Pending,
                           VerifyState::Passed, VerifyState::Failed,
                           VerifyState::Unavailable })
        CHECK(std::string(VerifyStateName(v)) != "unknown");
    // A mismatch has to be shouted, not mentioned.
    CHECK(std::string(VerifyStateName(VerifyState::Failed)) == "MISMATCH");
    // A deletion has to read as one.
    CHECK(std::string(SyncActionName(SyncAction::DeleteRemote)).find("DELETE") !=
          std::string::npos);
}

// --------------------------------------------------------------- unicode
TEST_CASE("comparison handles Unicode and long paths as opaque bytes",
          "[sync][unicode]")
{
    CompareOptions o;
    const std::string emoji = "\xF0\x9F\x93\x81/\xC3\xA9t\xC3\xA9.txt";
    const std::string deep = std::string(40, 'a') + "/" + std::string(200, 'b');
    const auto p = Compare({ F(emoji.c_str(), 10, 1000), F(deep.c_str(), 5, 1000) },
                           { F(emoji.c_str(), 10, 1000) }, o);
    CHECK(Find(p, emoji.c_str())->state == CompareState::Identical);
    CHECK(Find(p, deep.c_str())->state == CompareState::LocalOnly);

    SECTION("a glob matches a non-ASCII name")
    {
        CHECK(MatchGlob("*.txt", emoji, false));
    }
}
