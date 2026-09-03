// HostSigil.h — a stable geometric identity derived from an SSH host key.
//
// The same host key always produces the same sigil; a one-bit change produces
// a visibly different one. Its job is to stop human mistakes that a
// fingerprint string does not: running a production command in development,
// confusing two similarly named hosts, trusting a familiar name after the key
// changed, losing context when a window is detached.
//
// What it is NOT: a security control. The sigil supplements the fingerprint,
// the changed-key warning and the explicit confirmation — it never replaces
// any of them, and recognising a shape is not verification. That distinction
// is why every function here is about DRAWING and none of them return a
// verdict.
//
// Pure and deterministic: no Windows, no GDI, no renderer. It turns a
// fingerprint string into geometry, and the callers draw it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// The algorithm is versioned and the version is stored with any cached
// appearance, so a future change to the geometry cannot silently make a
// user's familiar sigil mean a different key.
constexpr int kSigilVersion = 1;

struct SigilNode
{
    float x = 0.0f, y = 0.0f;   // 0..1 within the sigil box
    float r = 0.0f;             // radius, 0..1 relative to the box
    bool filled = false;        // filled or ring — readable without colour
};

struct SigilEdge
{
    int a = 0, b = 0;           // indices into nodes
    bool doubled = false;       // drawn as two strokes
};

struct Sigil
{
    int version = kSigilVersion;
    // The 64-bit digest the geometry came from, so a caller can compare two
    // sigils without re-deriving them.
    uint64_t seed = 0;
    std::vector<SigilNode> nodes;
    std::vector<SigilEdge> edges;
    // Corner cuts, clockwise from top-left: how much of each corner is
    // chamfered, 0..1. A shape cue that survives monochrome and low contrast.
    float corners[4] = { 0, 0, 0, 0 };
    // Border rhythm: dash lengths around the frame, again shape not colour.
    std::vector<float> rhythm;
    // A hue hint in degrees. Deliberately ONE of several cues and never the
    // only one — the sigil must be distinguishable in monochrome.
    float hueDeg = 0.0f;
    // Whether the source key was parsed as a real fingerprint. A sigil from
    // an unparseable string is still drawn (so the UI never has a hole) but
    // is marked, and the UI says "unverified" rather than implying identity.
    bool fromFingerprint = false;
};

// Builds a sigil from an SSH fingerprint as AmberSSH stores it — typically
// "SHA256:<base64>" or a "keytype SHA256:<base64>" pair. Any string is
// accepted; a string that does not look like a fingerprint still yields a
// stable shape, with `fromFingerprint` false.
Sigil MakeSigil(const std::string& fingerprint);

// A short verbal descriptor for screen readers and for the text-only view,
// e.g. "five nodes, cut top-left, double link". Deterministic, and derived
// from the same digest, so two hosts that look different also read
// differently.
std::string SigilDescribe(const Sigil& s);

// A four-character mnemonic from the same digest, for places too small for a
// drawing (a tab, a toast, a log line). Uses a consonant-vowel alphabet so
// the result is pronounceable and hard to confuse: "TAKO", "BIRU".
std::string SigilMnemonic(const Sigil& s);

// True when two sigils would be drawn identically. Used by the tests to
// assert that a one-bit key change is visible.
bool SigilEqual(const Sigil& a, const Sigil& b);

// The normalised fingerprint a sigil was derived from — the base64 body of a
// SHA256 fingerprint when one was found, else the trimmed input. Exposed so
// the UI can show the exact text beside the shape, never the shape alone.
std::string SigilSourceText(const std::string& fingerprint);

} // namespace amber
