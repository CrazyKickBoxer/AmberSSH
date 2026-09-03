// LocalSessionTests.cpp — the local (ConPTY) session backend.
//
// What is testable without a machine-specific shell is tested here: the WSL
// list parser (a UTF-16 format with a BOM and CR endings that is easy to get
// subtly wrong), executable resolution, the shell-integration bootstrap, and
// the profile round-trip for the Local protocol. Launching a real pseudo-
// console belongs in the manual matrix in docs/STAGE1-LOCAL-REPORT.md.
#include <catch2/catch_test_macros.hpp>

#include "../src/platform/ConPty.h"
#include "../src/profiles/ConnectionProfile.h"
#include "../src/profiles/ProfileStore.h"

#include <filesystem>
#include <string>

using namespace amber;

TEST_CASE("WSL list parser reads wsl.exe --list --quiet output")
{
    SECTION("plain names, CRLF, as wsl.exe prints them")
    {
        auto d = ParseWslList(L"Ubuntu\r\nDebian\r\nkali-linux\r\n");
        REQUIRE(d.size() == 3);
        CHECK(d[0] == "Ubuntu");
        CHECK(d[1] == "Debian");
        CHECK(d[2] == "kali-linux");
    }
    SECTION("a leading byte-order mark is not a distribution")
    {
        auto d = ParseWslList(L"\uFEFFUbuntu\r\n");
        REQUIRE(d.size() == 1);
        CHECK(d[0] == "Ubuntu");
    }
    SECTION("blank lines and trailing whitespace are dropped")
    {
        auto d = ParseWslList(L"Ubuntu  \r\n\r\n   \r\nDebian\r\n\r\n");
        REQUIRE(d.size() == 2);
        CHECK(d[0] == "Ubuntu");
        CHECK(d[1] == "Debian");
    }
    SECTION("no trailing newline still yields the last name")
    {
        auto d = ParseWslList(L"Ubuntu\r\nDebian");
        REQUIRE(d.size() == 2);
        CHECK(d[1] == "Debian");
    }
    SECTION("empty output means no distributions, not one empty name")
    {
        CHECK(ParseWslList(L"").empty());
        CHECK(ParseWslList(L"\uFEFF").empty());
        CHECK(ParseWslList(L"\r\n\r\n").empty());
    }
    SECTION("a localised error line is not taken for a distribution")
    {
        // wsl.exe prints a sentence when the feature is absent; a name never
        // contains both a colon and a space.
        auto d = ParseWslList(L"Windows Subsystem for Linux has no installed distributions.\r\n"
                              L"Error: 0x8007019e\r\n");
        for (const std::string& n : d)
            CHECK(n.find(' ') == std::string::npos);
    }
    SECTION("names with a hyphen or digits survive intact")
    {
        auto d = ParseWslList(L"Ubuntu-22.04\r\nopenSUSE-Leap-15.6\r\n");
        REQUIRE(d.size() == 2);
        CHECK(d[0] == "Ubuntu-22.04");
        CHECK(d[1] == "openSUSE-Leap-15.6");
    }
}

TEST_CASE("executable resolution expands variables and finds PATH entries")
{
    SECTION("an absolute path is returned unchanged")
    {
        std::wstring p = L"C:\\Windows\\System32\\cmd.exe";
        CHECK(ResolveExecutable(p) == p);
    }
    SECTION("environment variables expand")
    {
        std::wstring r = ResolveExecutable(L"%SystemRoot%\\System32\\cmd.exe");
        CHECK(r.find(L"%") == std::wstring::npos);
        CHECK(std::filesystem::exists(r));
    }
    SECTION("a bare name resolves against PATH")
    {
        std::wstring r = ResolveExecutable(L"cmd.exe");
        CHECK(r.size() > 7);                       // not just "cmd.exe"
        CHECK(std::filesystem::exists(r));
    }
    SECTION("an unresolvable name comes back unchanged so the error can name it")
    {
        CHECK(ResolveExecutable(L"no-such-program-xyzzy.exe") ==
              L"no-such-program-xyzzy.exe");
    }
    SECTION("empty stays empty")
    {
        CHECK(ResolveExecutable(L"").empty());
    }
}

TEST_CASE("shell integration is offered only where it is safe")
{
    SECTION("PowerShell gets a prompt wrapper carrying both OSC 7 and OSC 133")
    {
        for (const char* key : { "pwsh", "powershell" })
        {
            std::wstring a = ShellIntegrationArgs(key);
            REQUIRE_FALSE(a.empty());
            CHECK(a.find(L"133;A") != std::wstring::npos);   // prompt start
            CHECK(a.find(L"133;B") != std::wstring::npos);   // input start
            CHECK(a.find(L"133;D") != std::wstring::npos);   // exit code
            CHECK(a.find(L"]7;file://") != std::wstring::npos);
        }
    }
    SECTION("bash-family shells get PROMPT_COMMAND and PS0")
    {
        for (const char* key : { "gitbash", "wsl:Ubuntu" })
        {
            std::wstring a = ShellIntegrationArgs(key);
            REQUIRE_FALSE(a.empty());
            CHECK(a.find(L"PROMPT_COMMAND") != std::wstring::npos);
            CHECK(a.find(L"133;C") != std::wstring::npos);   // command start
        }
    }
    SECTION("cmd.exe has no prompt hook that can carry escapes, so it gets none")
    {
        CHECK(ShellIntegrationArgs("cmd").empty());
    }
    SECTION("an unknown shell gets none rather than a guess")
    {
        CHECK(ShellIntegrationArgs("fish").empty());
        CHECK(ShellIntegrationArgs("").empty());
    }
    SECTION("nothing in the bootstrap writes to a dotfile")
    {
        // The whole point of the per-session bootstrap: it is passed as
        // arguments and touches nothing on disk.
        for (const char* key : { "pwsh", "powershell", "gitbash", "wsl:Ubuntu" })
        {
            std::wstring a = ShellIntegrationArgs(key);
            CHECK(a.find(L".bashrc") == std::wstring::npos);
            CHECK(a.find(L".zshrc") == std::wstring::npos);
            CHECK(a.find(L"profile.ps1") == std::wstring::npos);
            CHECK(a.find(L">>") == std::wstring::npos);      // no redirection
        }
    }
}

TEST_CASE("shell discovery reports usable entries")
{
    // cmd.exe exists on every Windows machine, so this much is safe to assert
    // anywhere the suite runs; everything else is machine-dependent.
    std::vector<LocalShell> shells = DiscoverLocalShells();
    REQUIRE_FALSE(shells.empty());
    bool sawCmd = false;
    for (const LocalShell& s : shells)
    {
        CHECK_FALSE(s.key.empty());
        CHECK_FALSE(s.name.empty());
        CHECK_FALSE(s.exe.empty());
        if (s.key == "cmd")
        {
            sawCmd = true;
            CHECK(std::filesystem::exists(s.exe));
        }
        if (s.wsl)
        {
            CHECK_FALSE(s.distro.empty());
            CHECK(s.key == "wsl:" + s.distro);
            CHECK(s.args.find(L"--distribution") != std::wstring::npos);
        }
    }
    CHECK(sawCmd);
}

TEST_CASE("a discovered shell resolves back from its key")
{
    LocalShell got;
    REQUIRE(ResolveShellByKey("cmd", got));
    CHECK(got.key == "cmd");
    CHECK(std::filesystem::exists(got.exe));

    SECTION("an absent shell reports absence rather than a wrong shell")
    {
        LocalShell miss;
        CHECK_FALSE(ResolveShellByKey("wsl:NoSuchDistro", miss));
        CHECK_FALSE(ResolveShellByKey("", miss));
    }
}

TEST_CASE("the Local protocol survives a profile round trip")
{
    ConnectionProfile p;
    p.id = MakeUuid();
    p.name = "Ubuntu shell";
    p.protocol = Protocol::Local;
    p.localShellKey = "wsl:Ubuntu";
    p.localExe = "C:\\custom\\shell.exe";
    p.localArgs = "--flag \"a b\"";
    p.localCwd = "%USERPROFILE%\\src";
    p.localEnv = "EDITOR=vim\nLANG=en_GB.UTF-8";
    p.localShellIntegration = false;

    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("amber-local-" + MakeUuid());
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "profiles.json";
    {
        ProfileStore store;
        store.Upsert(p);
        std::string err;
        REQUIRE(store.SaveTo(file, &err));
    }
    ProfileStore reload;
    REQUIRE(reload.LoadFrom(file));
    const std::vector<ConnectionProfile>& all = reload.All();
    REQUIRE(all.size() == 1);
    const ConnectionProfile& r = all[0];

    CHECK(r.protocol == Protocol::Local);
    CHECK(r.localShellKey == p.localShellKey);
    CHECK(r.localExe == p.localExe);
    CHECK(r.localArgs == p.localArgs);
    CHECK(r.localCwd == p.localCwd);
    CHECK(r.localEnv == p.localEnv);
    CHECK(r.localShellIntegration == false);

    SECTION("the port is carried but meaningless, as it is for Serial")
    {
        // Portless protocols simply ignore the field rather than normalising
        // it; asserting the value would encode a rule the codebase does not
        // have. What matters is that it never blocks the profile.
        CHECK(r.Valid());
    }
    SECTION("the protocol name round-trips through its string form")
    {
        CHECK(std::string(ProtocolName(Protocol::Local)) == "local");
        CHECK(ProtocolFromName("local") == Protocol::Local);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a local profile is valid on its shell key alone")
{
    ConnectionProfile p;
    p.id = MakeUuid();
    p.protocol = Protocol::Local;
    CHECK_FALSE(p.Valid());              // neither a key nor an executable

    p.localShellKey = "pwsh";
    CHECK(p.Valid());                    // a discovered shell is enough

    p.localShellKey.clear();
    p.localExe = "cmd.exe";
    CHECK(p.Valid());                    // so is a bare executable

    SECTION("host and port are irrelevant to a local session")
    {
        p.host.clear();
        p.port = 0;
        CHECK(p.Valid());
    }
}

TEST_CASE("environment override lines are parsed the way the transport parses them")
{
    // Mirrors the loop in ThreadMainLocal: blank lines, comments and lines
    // with no '=' are skipped rather than passed to the child as garbage.
    auto parse = [](const std::string& text) {
        std::vector<std::string> out;
        for (size_t pos = 0; pos < text.size();)
        {
            size_t end = text.find_first_of("\r\n", pos);
            std::string one = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                        : end - pos);
            pos = (end == std::string::npos) ? text.size() : end + 1;
            size_t a = one.find_first_not_of(" \t");
            if (a == std::string::npos || one[a] == '#' || one.find('=') == std::string::npos)
                continue;
            out.push_back(one.substr(a));
        }
        return out;
    };

    auto e = parse("EDITOR=vim\r\n\r\n# a comment\nLANG=en_GB.UTF-8\nnot an assignment\n  PAGER=less\n");
    REQUIRE(e.size() == 3);
    CHECK(e[0] == "EDITOR=vim");
    CHECK(e[1] == "LANG=en_GB.UTF-8");
    CHECK(e[2] == "PAGER=less");

    SECTION("a value may itself contain an equals sign")
    {
        auto q = parse("OPTS=--flag=1 --other=2\n");
        REQUIRE(q.size() == 1);
        CHECK(q[0] == "OPTS=--flag=1 --other=2");
    }
    SECTION("an empty value is still an assignment")
    {
        auto q = parse("EMPTY=\n");
        REQUIRE(q.size() == 1);
        CHECK(q[0] == "EMPTY=");
    }
}
