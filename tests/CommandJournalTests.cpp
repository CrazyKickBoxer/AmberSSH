// CommandJournalTests.cpp — the command journal's model: serialisation
// round-trips, the "do not record" rules, retention, and search ranking.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>

#include "sessions/CommandJournal.h"

using amber::CommandJournal;
using amber::JournalEntry;

namespace
{

JournalEntry Make(const char* cmd, const char* host = "web01",
                  const char* cwd = "/srv/app", int code = 0, double dur = 1.0)
{
    JournalEntry e;
    e.command = cmd;
    e.host = host;
    e.cwd = cwd;
    e.exitCode = code;
    e.durationSec = dur;
    e.startedAt = 1767225600;   // a fixed instant, so tests do not drift
    return e;
}

} // namespace

TEST_CASE("a journal entry survives a serialisation round trip",
          "[journal][contract]")
{
    JournalEntry in = Make("grep -R \"needle\" .", "db-primary", "/var/log", 2, 12.5);
    std::string line = CommandJournal::ToLine(in);
    JournalEntry out;
    REQUIRE(CommandJournal::FromLine(line, out));
    REQUIRE(out.command == in.command);      // embedded quotes survive
    REQUIRE(out.host == in.host);
    REQUIRE(out.cwd == in.cwd);
    REQUIRE(out.exitCode == 2);
    REQUIRE(out.startedAt == in.startedAt);
    REQUIRE(out.durationSec > 12.0);
    REQUIRE(out.durationSec < 13.0);
}

TEST_CASE("control characters and backslashes are escaped, not lost",
          "[journal][contract]")
{
    JournalEntry in = Make("printf 'a\\tb\\n' > C:\\tmp\\x");
    JournalEntry out;
    REQUIRE(CommandJournal::FromLine(CommandJournal::ToLine(in), out));
    REQUIRE(out.command == in.command);

    JournalEntry raw = Make("echo one\ttwo");
    std::string line = CommandJournal::ToLine(raw);
    // A raw tab would break the one-entry-per-line format.
    REQUIRE(line.find('\t') == std::string::npos);
    REQUIRE(CommandJournal::FromLine(line, out));
    REQUIRE(out.command == raw.command);
}

TEST_CASE("a malformed line is rejected rather than half read",
          "[journal][contract]")
{
    JournalEntry out;
    REQUIRE_FALSE(CommandJournal::FromLine("", out));
    REQUIRE_FALSE(CommandJournal::FromLine("not json at all", out));
    REQUIRE_FALSE(CommandJournal::FromLine("{\"h\":\"web\"}", out));  // no command
    REQUIRE_FALSE(CommandJournal::FromLine("{\"c\":\"\"}", out));     // empty command
}

TEST_CASE("search ranks the command above the host and directory",
          "[journal][search]")
{
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make("systemctl restart nginx", "web01", "/etc"));
    j.Add(Make("ls -la", "nginx-box", "/home"));          // 'nginx' only in host
    j.Add(Make("tail -f error.log", "web01", "/var/log/nginx"));

    std::vector<size_t> hits = j.Search("nginx");
    REQUIRE(hits.size() == 3);
    // The entry whose COMMAND contains the term wins over the ones where it
    // only appears in the host name or the path.
    REQUIRE(j.Entries()[hits[0]].command == "systemctl restart nginx");
}

TEST_CASE("search matches across command, host and directory",
          "[journal][search]")
{
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make("df -h", "web01", "/srv"));
    j.Add(Make("df -h", "db-primary", "/srv"));

    std::vector<size_t> hits = j.Search("df db");
    REQUIRE_FALSE(hits.empty());
    REQUIRE(j.Entries()[hits[0]].host == "db-primary");

    // An empty query returns everything, newest first.
    hits = j.Search("");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 0);

    // A term present nowhere matches nothing.
    REQUIRE(j.Search("zzzznotpresent").empty());
}

TEST_CASE("a leading space means do not record", "[journal][privacy]")
{
    // This is the shell's own HISTCONTROL=ignorespace convention, and it is
    // the escape hatch for a command with a secret in its arguments.
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make(" mysql -u root -phunter2"));
    REQUIRE(j.Entries().empty());

    j.Add(Make("mysql -u root"));
    REQUIRE(j.Entries().size() == 1);
}

TEST_CASE("blank commands and trailing whitespace are cleaned up",
          "[journal]")
{
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make(""));
    j.Add(Make("\t"));
    REQUIRE(j.Entries().empty());

    j.Add(Make("uptime   "));
    REQUIRE(j.Entries().size() == 1);
    REQUIRE(j.Entries()[0].command == "uptime");
}

TEST_CASE("an immediately repeated command collapses to one entry",
          "[journal]")
{
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make("make", "web01"));
    j.Add(Make("make", "web01"));
    j.Add(Make("make", "web01"));
    REQUIRE(j.Entries().size() == 1);

    // The same command on a DIFFERENT host is a separate fact.
    j.Add(Make("make", "db-primary"));
    REQUIRE(j.Entries().size() == 2);

    // And a repeat that is not immediate is kept.
    j.Add(Make("ls", "db-primary"));
    j.Add(Make("make", "db-primary"));
    REQUIRE(j.Entries().size() == 4);
}

TEST_CASE("the newest command is first and removal works by index",
          "[journal]")
{
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make("first", "h"));
    j.Add(Make("second", "h"));
    REQUIRE(j.Entries()[0].command == "second");

    j.Remove(0);
    REQUIRE(j.Entries().size() == 1);
    REQUIRE(j.Entries()[0].command == "first");

    j.Remove(99);                       // out of range is a no-op, not a crash
    REQUIRE(j.Entries().size() == 1);

    j.Clear();
    REQUIRE(j.Entries().empty());
}

TEST_CASE("loading is idempotent and never doubles the history",
          "[journal][regression]")
{
    // The overlay calls Load() when it opens, and commands are added while the
    // app runs. If Load() appended instead of replacing, every entry would
    // appear twice — and because Add() rewrites the whole file, the duplicates
    // would be written back to disk.
    CommandJournal j;
    j.UseFile("");   // memory only: never touch the user's real journal
    j.Add(Make("uptime", "web01"));
    const size_t after_add = j.Entries().size();
    j.Load();
    j.Load();
    REQUIRE(j.Entries().size() == after_add);
}

TEST_CASE("retention is bounded", "[journal]")
{
    REQUIRE(CommandJournal::kMaxEntries >= 1000);
    REQUIRE(CommandJournal::kMaxEntries <= 100000);
}

TEST_CASE("the journal survives a trip through the file", "[journal][io]")
{
    // Exercises the real read/write path against a scratch file, including
    // the ordering flip: oldest-first on disk, newest-first in memory.
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "amber_journal_test.jsonl";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    {
        CommandJournal j;
        j.UseFile(tmp.string());
        j.Add(Make("first", "web01"));
        j.Add(Make("second", "web01", "/tmp", 3, 9.0));
    }
    {
        CommandJournal j;
        j.UseFile(tmp.string());
        REQUIRE(j.Entries().size() == 2);
        REQUIRE(j.Entries()[0].command == "second");   // newest first
        REQUIRE(j.Entries()[0].exitCode == 3);
        REQUIRE(j.Entries()[1].command == "first");
        // Reopening must not double the contents.
        j.Load();
        REQUIRE(j.Entries().size() == 2);
    }
    std::filesystem::remove(tmp, ec);
}
