// HostKeyDecision.h — what to do about the key a server just presented.
//
// This is the one control standing between the user and a man-in-the-middle,
// and until now it existed only as a run of branches in the middle of a
// nineteen-hundred-line connect function, where nothing could reach it
// without a server. The branches are the same; they are here so the whole
// matrix can be tested.
//
// Pure: no libssh2, no sockets, no Windows. It is given what the lookup found
// and what the profile says, and returns which of four things should happen.
#pragma once

#include <string>
#include <vector>

namespace amber
{

// What known_hosts said about this host and key. Mirrors libssh2's
// LIBSSH2_KNOWNHOST_CHECK_*, named rather than numbered so a caller cannot
// pass the wrong constant and get a plausible-looking answer.
enum class KnownHostResult
{
    Match,       // this exact key is stored for this host
    Mismatch,    // a different key is stored for this host
    NotFound,    // nothing is stored for this host
    Failure,     // the lookup itself could not run
};

enum class HostKeyAction
{
    Accept,      // proceed, say nothing
    Prompt,      // unknown host: show the fingerprint and wait for an answer
    RefuseMismatch,   // stored key differs — the alarm case
    RefuseNotListed,  // manual list is in force and this key is not in it
};

struct HostKeyVerdict
{
    HostKeyAction action = HostKeyAction::Prompt;
    // True when an accepted key should be written to known_hosts. False for a
    // manual-list match, which is configuration rather than a trust decision
    // to remember, and for anything refused.
    bool persist = false;
};

// The decision.
//
// `manualKeys` is the profile's "SSH > Host keys" list. When it is non-empty
// it is the WHOLE trust store for this connection: a listed fingerprint is
// accepted without a prompt and anything else is refused, and known_hosts is
// not consulted. That is deliberate — a manual list exists precisely so a
// host can be pinned independently of the file.
//
// Entries are compared case-insensitively, ignoring surrounding whitespace,
// with an optional "SHA256:" prefix on either side. An empty entry is skipped
// rather than treated as a wildcard.
//
// An empty `fingerprint` never matches anything, so a server whose key could
// not be hashed cannot be accepted by a manual list.
HostKeyVerdict DecideHostKey(KnownHostResult lookup,
                             const std::string& fingerprint,
                             const std::vector<std::string>& manualKeys);

// True when `entry` names `fingerprint`, under the comparison above. Exposed
// because it is the fiddly half and deserves its own tests.
bool HostKeyFingerprintMatches(const std::string& entry, const std::string& fingerprint);

} // namespace amber
