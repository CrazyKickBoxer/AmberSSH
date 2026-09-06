// HostKeyDecisionTests.cpp — the one control between the user and a
// man-in-the-middle. It had no test before this file; the whole matrix is
// here, including the cases where getting it wrong is silent.
#include <catch2/catch_test_macros.hpp>

#include "../src/ssh/HostKeyDecision.h"

using amber::DecideHostKey;
using amber::HostKeyAction;
using amber::HostKeyFingerprintMatches;
using amber::KnownHostResult;

namespace
{
const std::string kFp = "SHA256:BXbT2VqXlKcQ0mPfDdJ8v9nRsYtWuEiOaZcHgLkMnPq";
const std::string kOther = "SHA256:ZZbT2VqXlKcQ0mPfDdJ8v9nRsYtWuEiOaZcHgLkMnPq";
const std::vector<std::string> kNoManual;
}

TEST_CASE("known_hosts says match: proceed", "[hostkey]")
{
    const auto v = DecideHostKey(KnownHostResult::Match, kFp, kNoManual);
    REQUIRE(v.action == HostKeyAction::Accept);
    REQUIRE_FALSE(v.persist);   // already stored, nothing to write
}

TEST_CASE("known_hosts says mismatch: refuse, never prompt", "[hostkey]")
{
    const auto v = DecideHostKey(KnownHostResult::Mismatch, kFp, kNoManual);
    REQUIRE(v.action == HostKeyAction::RefuseMismatch);
    REQUIRE_FALSE(v.persist);
    // The alarm case must not be reachable as a prompt: a click-through here
    // is exactly the attack succeeding.
    REQUIRE(v.action != HostKeyAction::Prompt);
    REQUIRE(v.action != HostKeyAction::Accept);
}

TEST_CASE("unknown host asks, and remembers if accepted", "[hostkey]")
{
    const auto v = DecideHostKey(KnownHostResult::NotFound, kFp, kNoManual);
    REQUIRE(v.action == HostKeyAction::Prompt);
    REQUIRE(v.persist);
}

TEST_CASE("a lookup that could not run asks rather than proceeding", "[hostkey]")
{
    // Fail-closed: a broken or unreadable known_hosts must not become an
    // implicit "fine". It is the same as never having seen this host.
    const auto v = DecideHostKey(KnownHostResult::Failure, kFp, kNoManual);
    REQUIRE(v.action == HostKeyAction::Prompt);
    REQUIRE(v.action != HostKeyAction::Accept);
}

TEST_CASE("a manual list is the whole trust store", "[hostkey]")
{
    const std::vector<std::string> listed{ kFp };

    // Listed: accepted with no prompt, whatever known_hosts thinks — including
    // when known_hosts holds a different key, which is the point of pinning.
    for (KnownHostResult r : { KnownHostResult::Match, KnownHostResult::Mismatch,
                               KnownHostResult::NotFound, KnownHostResult::Failure })
    {
        const auto v = DecideHostKey(r, kFp, listed);
        REQUIRE(v.action == HostKeyAction::Accept);
        REQUIRE_FALSE(v.persist);   // configuration, not a decision to store
    }

    // Not listed: refused, whatever known_hosts thinks — including when
    // known_hosts says match. The list is exclusive in both directions.
    for (KnownHostResult r : { KnownHostResult::Match, KnownHostResult::Mismatch,
                               KnownHostResult::NotFound, KnownHostResult::Failure })
        REQUIRE(DecideHostKey(r, kOther, listed).action == HostKeyAction::RefuseNotListed);
}

TEST_CASE("an unhashable key cannot be accepted by a manual list", "[hostkey]")
{
    // A server whose key produced no fingerprint must not slip through a list
    // containing an empty or whitespace-only entry.
    const std::vector<std::string> sloppy{ "", "   ", "\t\r\n" };
    REQUIRE(DecideHostKey(KnownHostResult::NotFound, "", sloppy).action ==
            HostKeyAction::RefuseNotListed);
    REQUIRE(DecideHostKey(KnownHostResult::NotFound, kFp, sloppy).action ==
            HostKeyAction::RefuseNotListed);
    // And an empty fingerprint is not matched by a real entry either.
    REQUIRE(DecideHostKey(KnownHostResult::NotFound, "", { kFp }).action ==
            HostKeyAction::RefuseNotListed);
}

TEST_CASE("fingerprint comparison is forgiving about form, not about value", "[hostkey]")
{
    const std::string bare = kFp.substr(7);

    REQUIRE(HostKeyFingerprintMatches(kFp, kFp));
    REQUIRE(HostKeyFingerprintMatches(bare, kFp));            // entry without prefix
    REQUIRE(HostKeyFingerprintMatches(kFp, bare));            // fingerprint without prefix
    REQUIRE(HostKeyFingerprintMatches("  " + kFp + "\r\n", kFp));   // surrounding space
    REQUIRE(HostKeyFingerprintMatches("sha256:" + bare, kFp));      // lower-case prefix

    REQUIRE_FALSE(HostKeyFingerprintMatches(kOther, kFp));
    REQUIRE_FALSE(HostKeyFingerprintMatches("", kFp));
    REQUIRE_FALSE(HostKeyFingerprintMatches(kFp, ""));
    // A prefix of the right fingerprint is not the right fingerprint.
    REQUIRE_FALSE(HostKeyFingerprintMatches(bare.substr(0, 20), kFp));
    // Nor is the right fingerprint with something appended.
    REQUIRE_FALSE(HostKeyFingerprintMatches(kFp + "x", kFp));
}

TEST_CASE("one good entry among bad ones is enough", "[hostkey]")
{
    const std::vector<std::string> mixed{ "", "  ", kOther, "  " + kFp + " ", "junk" };
    REQUIRE(DecideHostKey(KnownHostResult::NotFound, kFp, mixed).action ==
            HostKeyAction::Accept);
}
