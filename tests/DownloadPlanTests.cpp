// DownloadPlanTests.cpp — a hostile SFTP server, without an SFTP server.
//
// The traversal gate is tested component by component in RemoteNameTests.
// This drives the thing that uses it: given a directory tree a malicious
// server could serve, does the planner ever produce a path outside the folder
// the user chose? Every case here is a tree a real server can return.
#include <catch2/catch_test_macros.hpp>

#include <map>

#include "../src/ssh/DownloadPlan.h"
#include "../src/ssh/RemoteName.h"

using amber::DownloadLimits;
using amber::DownloadPlan;
using amber::PlanDownload;
using amber::RemoteEntry;

namespace
{
const std::wstring kRoot = L"C:\\Downloads";

// A remote tree held in memory: path -> entries. Anything not present lists
// as empty, the way an unreadable directory does.
struct FakeServer
{
    std::map<std::string, std::vector<RemoteEntry>> tree;
    int listCalls = 0;

    amber::RemoteLister Lister()
    {
        return [this](const std::string& dir, std::vector<RemoteEntry>& out) {
            ++listCalls;
            auto it = tree.find(dir);
            if (it == tree.end())
                return false;
            out = it->second;
            return true;
        };
    }
};

RemoteEntry File(const char* n, uint64_t sz = 1) { RemoteEntry e; e.name = n; e.size = sz; return e; }
RemoteEntry Dir(const char* n)  { RemoteEntry e; e.name = n; e.dir = true; return e; }
RemoteEntry LinkDir(const char* n) { RemoteEntry e; e.name = n; e.dir = true; e.link = true; return e; }

// The invariant the whole file exists to check. Reported by remote path,
// which is ASCII in these fixtures and does not need narrowing.
void EveryPathInsideRoot(const DownloadPlan& p)
{
    for (const auto& f : p.files)
    {
        INFO("planned from " << f.remote);
        REQUIRE(amber::PathWithin(kRoot, f.local));
    }
    for (const auto& d : p.dirs)
        REQUIRE(amber::PathWithin(kRoot, d));
}
}

TEST_CASE("an ordinary tree plans exactly", "[downloadplan]")
{
    FakeServer s;
    s.tree["/home/josh/project"] = { File("a.txt", 10), Dir("src"), File("b.log", 20) };
    s.tree["/home/josh/project/src"] = { File("main.cpp", 30) };

    const DownloadPlan p = PlanDownload("/home/josh", kRoot, Dir("project"), s.Lister());

    REQUIRE(p.refused.empty());
    REQUIRE_FALSE(p.truncated);
    REQUIRE(p.files.size() == 3);
    REQUIRE(p.dirs.size() == 2);
    EveryPathInsideRoot(p);
    // Order is the server's listing order, with a subdirectory's contents
    // inline at the point the directory appeared. Asserted because the
    // transfer queue is filled in this order and a user watching it should
    // see the tree in the shape the server described it.
    REQUIRE(p.files[0].remote == "/home/josh/project/a.txt");
    REQUIRE(p.files[0].local  == L"C:\\Downloads\\project\\a.txt");
    REQUIRE(p.files[1].remote == "/home/josh/project/src/main.cpp");
    REQUIRE(p.files[1].local  == L"C:\\Downloads\\project\\src\\main.cpp");
    REQUIRE(p.files[2].remote == "/home/josh/project/b.log");
    // Parents before children, so creating them in order always works.
    REQUIRE(p.dirs[0] == L"C:\\Downloads\\project");
    REQUIRE(p.dirs[1] == L"C:\\Downloads\\project\\src");
}

TEST_CASE("a traversing name never becomes a path", "[downloadplan]")
{
    FakeServer s;
    s.tree["/evil"] = {
        File("..\\..\\..\\evil.exe"),
        File("../../../evil2.exe"),
        File(".."),
        File("sub/nested"),
        File("ok.txt"),
    };
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("evil"), s.Lister());

    REQUIRE(p.files.size() == 1);
    REQUIRE(p.files[0].local == L"C:\\Downloads\\evil\\ok.txt");
    REQUIRE(p.refused.size() == 4);
    EveryPathInsideRoot(p);
}

TEST_CASE("the Windows-specific names never become paths either", "[downloadplan]")
{
    FakeServer s;
    s.tree["/evil"] = {
        File("C:evil"), File("notes.txt:hidden"), File("CON"), File("COM1.log"),
        File("trailing."), File("trailing "), File("star*"), File("bidi\xE2\x80\xAE" "gnp.exe"),
        File("fine.txt"),
    };
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("evil"), s.Lister());

    REQUIRE(p.files.size() == 1);
    REQUIRE(p.files[0].local == L"C:\\Downloads\\evil\\fine.txt");
    REQUIRE(p.refused.size() == 8);
    EveryPathInsideRoot(p);
}

TEST_CASE("a symlinked directory is not descended", "[downloadplan]")
{
    FakeServer s;
    s.tree["/x"] = { LinkDir("to-root"), Dir("real"), File("f") };
    s.tree["/x/to-root"] = { File("etc-passwd-ish") };
    s.tree["/x/real"] = { File("g") };

    const DownloadPlan p = PlanDownload("/", kRoot, Dir("x"), s.Lister());

    // The symlink is refused and its contents never listed.
    for (const auto& f : p.files)
        REQUIRE(f.remote.find("to-root") == std::string::npos);
    REQUIRE(p.files.size() == 2);
    REQUIRE(p.refused.size() == 1);
    REQUIRE(p.refused[0].reason == std::string("it is a symbolic link to a directory"));
    EveryPathInsideRoot(p);
}

TEST_CASE("a symlink loop cannot hang the walk", "[downloadplan]")
{
    // The classic denial of service: a directory that contains itself. Even
    // if the link flag were wrong, the depth limit ends it.
    FakeServer s;
    s.tree["/loop"] = { Dir("again"), File("f") };
    s.tree["/loop/again"] = { Dir("again"), File("f") };
    for (int i = 0; i < 200; ++i)
    {
        std::string path = "/loop";
        for (int k = 0; k <= i; ++k) path += "/again";
        s.tree[path] = { Dir("again"), File("f") };
    }

    DownloadLimits lim;
    lim.maxDepth = 8;
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("loop"), s.Lister(), lim);

    REQUIRE(p.truncated);
    REQUIRE(p.files.size() <= 9);
    EveryPathInsideRoot(p);
}

TEST_CASE("a very wide tree stops at the file limit", "[downloadplan]")
{
    FakeServer s;
    std::vector<RemoteEntry> many;
    for (int i = 0; i < 5000; ++i)
        many.push_back(File(("f" + std::to_string(i)).c_str()));
    s.tree["/wide"] = many;

    DownloadLimits lim;
    lim.maxFiles = 100;
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("wide"), s.Lister(), lim);

    REQUIRE(p.truncated);
    REQUIRE(p.files.size() == 100);
    EveryPathInsideRoot(p);
}

TEST_CASE("a hostile name on the top entry is refused before anything is listed",
          "[downloadplan]")
{
    FakeServer s;
    s.tree["/..\\..\\evil"] = { File("payload.exe") };
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("..\\..\\evil"), s.Lister());

    REQUIRE(p.files.empty());
    REQUIRE(p.dirs.empty());
    REQUIRE(p.refused.size() == 1);
    REQUIRE(s.listCalls == 0);   // never even asked the server
}

TEST_CASE("a single file plans without listing anything", "[downloadplan]")
{
    FakeServer s;
    const DownloadPlan p = PlanDownload("/home", kRoot, File("notes.txt", 42), s.Lister());
    REQUIRE(p.files.size() == 1);
    REQUIRE(p.files[0].local == L"C:\\Downloads\\notes.txt");
    REQUIRE(p.files[0].size == 42);
    REQUIRE(s.listCalls == 0);
    EveryPathInsideRoot(p);
}

TEST_CASE("an unlistable directory is empty, not fatal", "[downloadplan]")
{
    FakeServer s;
    s.tree["/a"] = { Dir("denied"), File("readable") };
    // "/a/denied" is absent from the tree, so the lister returns false.
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("a"), s.Lister());
    REQUIRE(p.files.size() == 1);
    REQUIRE(p.files[0].remote == "/a/readable");
    EveryPathInsideRoot(p);
}

TEST_CASE("non-ASCII names survive intact", "[downloadplan]")
{
    FakeServer s;
    s.tree["/i18n"] = {
        File("\xC3\xA9t\xC3\xA9.txt"),                       // été.txt
        File("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82.log"),  // привет.log
        File("\xF0\x9F\x93\x81.dat"),                        // an emoji, outside the BMP
    };
    const DownloadPlan p = PlanDownload("/", kRoot, Dir("i18n"), s.Lister());
    REQUIRE(p.refused.empty());
    REQUIRE(p.files.size() == 3);
    EveryPathInsideRoot(p);
    // The surrogate pair round-tripped rather than being dropped.
    REQUIRE(p.files[2].local.find(L"\xD83D\xDCC1") != std::wstring::npos);
}
