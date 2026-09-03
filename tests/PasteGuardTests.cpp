// PasteGuardTests.cpp — when a paste has to be confirmed. This is a safety
// rule, so the boundaries are pinned rather than left to the UI.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "utility/PasteGuard.h"

using amber::kPasteConfirmChars;
using amber::PasteLineCount;
using amber::PasteNeedsConfirm;

TEST_CASE("a single line pastes without interruption", "[paste][safety]")
{
    REQUIRE_FALSE(PasteNeedsConfirm("ls -la", true));
    REQUIRE_FALSE(PasteNeedsConfirm("", true));
    REQUIRE_FALSE(PasteNeedsConfirm(std::string(500, 'x'), true));
}

TEST_CASE("anything that would submit a command is confirmed",
          "[paste][safety]")
{
    // One carriage return is one Enter: this already runs the first command.
    REQUIRE(PasteNeedsConfirm("rm -rf /var/tmp\r", true));
    REQUIRE(PasteNeedsConfirm("a\rb", true));
    REQUIRE(PasteNeedsConfirm("\r", true));
}

TEST_CASE("a very long single line is confirmed too", "[paste][safety]")
{
    REQUIRE_FALSE(PasteNeedsConfirm(std::string(kPasteConfirmChars, 'x'), true));
    REQUIRE(PasteNeedsConfirm(std::string(kPasteConfirmChars + 1, 'x'), true));
}

TEST_CASE("the guard can be switched off", "[paste][safety]")
{
    REQUIRE_FALSE(PasteNeedsConfirm("rm -rf /\rreboot\r", false));
    REQUIRE_FALSE(PasteNeedsConfirm(std::string(9999, 'x'), false));
}

TEST_CASE("the reported line count matches what would be submitted",
          "[paste]")
{
    REQUIRE(PasteLineCount("one") == 1);
    REQUIRE(PasteLineCount("one\rtwo") == 2);
    // A trailing return means the last line is submitted as well, so it is a
    // line for the purposes of the warning.
    REQUIRE(PasteLineCount("one\rtwo\r") == 3);
    REQUIRE(PasteLineCount("") == 1);
}
