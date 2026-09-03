// WorkspaceTests.cpp — the workspace store: serialisation round-trips
// (including split tabs and ordering), replace-by-name, and removal.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>

#include "sessions/Workspaces.h"

using amber::Workspace;
using amber::WorkspaceStore;
using amber::WorkspaceTab;

namespace
{

Workspace Make(const char* name)
{
    Workspace w;
    w.name = name;
    WorkspaceTab a;
    a.profileId = "prof-web-01";
    WorkspaceTab b;
    b.profileId = "prof-db";
    b.splitProfileId = "prof-cache";
    b.splitVertical = false;
    w.tabs.push_back(a);
    w.tabs.push_back(b);
    return w;
}

} // namespace

TEST_CASE("a workspace survives a serialisation round trip",
          "[workspace][contract]")
{
    Workspace in = Make("Morning");
    Workspace out;
    REQUIRE(WorkspaceStore::FromLine(WorkspaceStore::ToLine(in), out));
    REQUIRE(out.name == "Morning");
    REQUIRE(out.tabs.size() == 2);
    // Tab ORDER is the point of a workspace, so it has to survive.
    REQUIRE(out.tabs[0].profileId == "prof-web-01");
    REQUIRE(out.tabs[0].splitProfileId.empty());
    REQUIRE(out.tabs[1].profileId == "prof-db");
    REQUIRE(out.tabs[1].splitProfileId == "prof-cache");
    REQUIRE(out.tabs[1].splitVertical == false);
}

TEST_CASE("a name with quotes or backslashes round trips", "[workspace]")
{
    Workspace in = Make("she said \"prod\\staging\"");
    Workspace out;
    REQUIRE(WorkspaceStore::FromLine(WorkspaceStore::ToLine(in), out));
    REQUIRE(out.name == in.name);
}

TEST_CASE("malformed workspace lines are rejected", "[workspace][contract]")
{
    Workspace out;
    REQUIRE_FALSE(WorkspaceStore::FromLine("", out));
    REQUIRE_FALSE(WorkspaceStore::FromLine("garbage", out));
    REQUIRE_FALSE(WorkspaceStore::FromLine("{\"n\":\"x\",\"t\":[]}", out));  // no tabs
    REQUIRE_FALSE(WorkspaceStore::FromLine("{\"t\":[{\"p\":\"a\"}]}", out)); // no name
}

TEST_CASE("saving by the same name replaces rather than duplicates",
          "[workspace]")
{
    WorkspaceStore s;
    s.UseFile("");                  // memory only: never touch real state
    s.Put(Make("Morning"));
    s.Put(Make("Evening"));
    REQUIRE(s.All().size() == 2);

    Workspace again = Make("Morning");
    again.tabs.resize(1);
    s.Put(again);
    REQUIRE(s.All().size() == 2);
    REQUIRE(s.Find("Morning") != nullptr);
    REQUIRE(s.Find("Morning")->tabs.size() == 1);
}

TEST_CASE("an unnamed or empty workspace is not stored", "[workspace]")
{
    WorkspaceStore s;
    s.UseFile("");
    Workspace nameless = Make("");
    s.Put(nameless);
    Workspace empty;
    empty.name = "Empty";
    s.Put(empty);
    REQUIRE(s.All().empty());
}

TEST_CASE("removal works and missing names are reported", "[workspace]")
{
    WorkspaceStore s;
    s.UseFile("");
    s.Put(Make("Morning"));
    REQUIRE(s.Remove("Morning"));
    REQUIRE_FALSE(s.Remove("Morning"));
    REQUIRE(s.All().empty());
    REQUIRE(s.Find("Morning") == nullptr);
}

TEST_CASE("workspaces survive a trip through the file", "[workspace][io]")
{
    std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "amber_workspace_test.jsonl";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    {
        WorkspaceStore s;
        s.UseFile(tmp.string());
        s.Put(Make("Morning"));
        s.Put(Make("Evening"));
    }
    {
        WorkspaceStore s;
        s.UseFile(tmp.string());
        REQUIRE(s.All().size() == 2);
        REQUIRE(s.Find("Evening") != nullptr);
        REQUIRE(s.Find("Evening")->tabs[1].splitProfileId == "prof-cache");
        s.Load();                    // idempotent, must not double
        REQUIRE(s.All().size() == 2);
    }
    std::filesystem::remove(tmp, ec);
}

// ---------------------------------------------------- schema 2: pane trees
TEST_CASE("a pane tree survives a workspace round trip", "[workspace][panes]")
{
    Workspace w;
    w.name = "ops";
    WorkspaceTab t;
    t.profileId = "p-root";
    t.paneProfileIds = { "p-web1", "p-web2", "p-db" };
    t.layout = "V0.400(0,H0.500(1,V0.500(2,3)))";
    t.focusPane = 2;
    t.readOnlyPanes = { 3 };
    t.splitProfileId = "p-web1";     // the schema-1 view of the same tab
    t.splitVertical = true;
    w.tabs.push_back(t);

    const std::string line = WorkspaceStore::ToLine(w);
    Workspace back;
    REQUIRE(WorkspaceStore::FromLine(line, back));
    REQUIRE(back.tabs.size() == 1);
    const WorkspaceTab& g = back.tabs[0];
    CHECK(back.version == Workspace::kSchemaVersion);
    CHECK(g.profileId == "p-root");
    CHECK(g.layout == t.layout);
    CHECK(g.focusPane == 2);
    REQUIRE(g.paneProfileIds.size() == 3);
    CHECK(g.paneProfileIds[0] == "p-web1");
    CHECK(g.paneProfileIds[2] == "p-db");
    REQUIRE(g.readOnlyPanes.size() == 1);
    CHECK(g.readOnlyPanes[0] == 3);

    SECTION("the schema-1 fields are still written, for an older build")
    {
        CHECK(line.find("\"s\":\"p-web1\"") != std::string::npos);
        CHECK(line.find("\"v\":1") != std::string::npos);
    }
}

TEST_CASE("a workspace written before pane trees still loads",
          "[workspace][panes][compat]")
{
    // Exactly what the previous schema produced: no version, no layout.
    const std::string old =
        R"({"n":"legacy","t":[{"p":"a","s":"b","v":0},{"p":"c","s":"","v":1}]})";
    Workspace w;
    REQUIRE(WorkspaceStore::FromLine(old, w));
    CHECK(w.version == 1);
    REQUIRE(w.tabs.size() == 2);
    CHECK(w.tabs[0].profileId == "a");
    CHECK(w.tabs[0].splitProfileId == "b");
    CHECK_FALSE(w.tabs[0].splitVertical);
    CHECK(w.tabs[0].layout.empty());          // nothing to mis-restore
    CHECK(w.tabs[0].paneProfileIds.empty());
    CHECK(w.tabs[1].profileId == "c");
    CHECK(w.tabs[1].splitProfileId.empty());
}

TEST_CASE("several tabs with pane trees do not bleed into each other",
          "[workspace][panes]")
{
    Workspace w;
    w.name = "multi";
    WorkspaceTab a;
    a.profileId = "a0";
    a.paneProfileIds = { "a1" };
    a.layout = "V0.500(0,1)";
    a.focusPane = 1;
    WorkspaceTab b;
    b.profileId = "b0";               // no panes at all
    WorkspaceTab c;
    c.profileId = "c0";
    c.paneProfileIds = { "c1", "c2" };
    c.layout = "H0.300(0,V0.500(1,2))";
    c.readOnlyPanes = { 1, 2 };
    w.tabs = { a, b, c };

    Workspace back;
    REQUIRE(WorkspaceStore::FromLine(WorkspaceStore::ToLine(w), back));
    REQUIRE(back.tabs.size() == 3);
    CHECK(back.tabs[0].paneProfileIds.size() == 1);
    CHECK(back.tabs[0].focusPane == 1);
    // The middle tab has no layout, and must not inherit its neighbours'.
    CHECK(back.tabs[1].layout.empty());
    CHECK(back.tabs[1].paneProfileIds.empty());
    CHECK(back.tabs[1].focusPane == 0);
    CHECK(back.tabs[1].readOnlyPanes.empty());
    CHECK(back.tabs[2].layout == c.layout);
    REQUIRE(back.tabs[2].paneProfileIds.size() == 2);
    CHECK(back.tabs[2].paneProfileIds[1] == "c2");
    CHECK(back.tabs[2].readOnlyPanes.size() == 2);
}

TEST_CASE("a workspace never carries a broadcast set", "[workspace][panes][security]")
{
    // Restoring a workspace that starts typing into four production hosts is
    // not a feature. There is deliberately no field for it, so the schema
    // cannot express one even if a later change wanted to.
    Workspace w;
    w.name = "safe";
    WorkspaceTab t;
    t.profileId = "p";
    t.paneProfileIds = { "q", "r" };
    t.layout = "V0.500(0,V0.500(1,2))";
    w.tabs.push_back(t);
    const std::string line = WorkspaceStore::ToLine(w);
    CHECK(line.find("broadcast") == std::string::npos);
    CHECK(line.find("\"bc\"") == std::string::npos);
}

TEST_CASE("a workspace claiming a newer schema is read as the oldest one",
          "[workspace][compat]")
{
    const std::string future =
        R"({"n":"fromfuture","t":[{"p":"a","s":"","v":1}],"ver":99})";
    Workspace w;
    REQUIRE(WorkspaceStore::FromLine(future, w));
    CHECK(w.version == 1);            // take only what this build understands
    CHECK(w.tabs.size() == 1);
}

TEST_CASE("a workspace cannot demand an unbounded number of panes",
          "[workspace][panes][security]")
{
    std::string ids;
    for (int i = 0; i < 500; ++i)
        ids += std::string(i ? "," : "") + "\"p" + std::to_string(i) + "\"";
    // A custom delimiter: the layout string contains ")\"", which would end
    // an ordinary raw string in the middle of the JSON.
    const std::string line =
        R"J({"n":"huge","t":[{"p":"root","s":"","v":1,"l":"V0.500(0,1)","f":0,"q":[)J" +
        ids + R"J(],"o":[]}],"ver":2})J";
    Workspace w;
    REQUIRE(WorkspaceStore::FromLine(line, w));
    REQUIRE(w.tabs.size() == 1);
    CHECK(w.tabs[0].paneProfileIds.size() <= 65);
}
