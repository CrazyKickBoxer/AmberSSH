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
