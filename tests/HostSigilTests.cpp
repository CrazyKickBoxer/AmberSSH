// HostSigilTests — the identity figure derived from a host key.
//
// The whole point of a sigil is that a user can recognise it. That makes two
// properties load-bearing and both are tested here: the SAME key always gives
// the same figure, and a DIFFERENT key gives a visibly different one. A sigil
// that drifted between builds would be worse than no sigil, because people
// would learn to ignore the change that matters.
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "security/HostSigil.h"

using namespace amber;

namespace
{

// A realistic base64 fingerprint body.
std::string Fp(const std::string& body)
{
    return "SHA256:" + body;
}

const char* kBodyA = "uYtM0LrpZ8kX3vQ9dGfJhKlNpRsTwZaBcDeFgHiJkLm";
const char* kBodyB = "uYtM0LrpZ8kX3vQ9dGfJhKlNpRsTwZaBcDeFgHiJkLn";   // one char

// Everything about the drawing except the colour. Used to prove hue is never
// the only difference between two hosts.
std::string ShapeSignature(const Sigil& s)
{
    std::string k;
    for (const SigilNode& n : s.nodes)
    {
        k += std::to_string(static_cast<int>(n.x * 10000.0f)) + ",";
        k += std::to_string(static_cast<int>(n.y * 10000.0f)) + ",";
        k += std::to_string(static_cast<int>(n.r * 10000.0f)) + ",";
        k += n.filled ? "F;" : "o;";
    }
    for (const SigilEdge& e : s.edges)
        k += std::to_string(e.a) + "-" + std::to_string(e.b) +
             (e.doubled ? "=" : "|");
    for (float c : s.corners)
        k += std::to_string(static_cast<int>(c * 10000.0f)) + ".";
    for (float t : s.rhythm)
        k += std::to_string(static_cast<int>(t * 100.0f)) + "_";
    return k;
}

} // namespace

TEST_CASE("A sigil is a pure function of the fingerprint", "[sigil]")
{
    const Sigil a = MakeSigil(Fp(kBodyA));
    const Sigil b = MakeSigil(Fp(kBodyA));
    CHECK(SigilEqual(a, b));
    CHECK(a.seed == b.seed);
    CHECK(a.version == kSigilVersion);
}

TEST_CASE("One changed character changes the whole figure", "[sigil]")
{
    const Sigil a = MakeSigil(Fp(kBodyA));
    const Sigil b = MakeSigil(Fp(kBodyB));
    CHECK_FALSE(SigilEqual(a, b));
    CHECK(a.seed != b.seed);
    // Not merely a different colour: the drawing itself differs.
    CHECK(ShapeSignature(a) != ShapeSignature(b));
}

TEST_CASE("The same key printed differently is the same host", "[sigil]")
{
    // ssh-keygen and the connection banner print a fingerprint differently.
    // They are the same identity, so they must produce the same sigil.
    const Sigil bare = MakeSigil(Fp(kBodyA));
    const Sigil typed = MakeSigil("ssh-ed25519 " + Fp(kBodyA));
    const Sigil trailing = MakeSigil(Fp(kBodyA) + " root@host");
    CHECK(SigilEqual(bare, typed));
    CHECK(SigilEqual(bare, trailing));
}

TEST_CASE("SigilSourceText normalises to the fingerprint body", "[sigil]")
{
    CHECK(SigilSourceText("SHA256:abc") == "abc");
    CHECK(SigilSourceText("ssh-rsa SHA256:abc") == "abc");
    CHECK(SigilSourceText("SHA256:abc user@host") == "abc");
    // Something that is not a fingerprint is used as-is, trimmed.
    CHECK(SigilSourceText("  plain  ") == "plain");
}

TEST_CASE("A sigil stays inside its box and stays legible", "[sigil]")
{
    // Sampled across many hosts, because a bound that only holds for one seed
    // is not a bound.
    for (int i = 0; i < 400; ++i)
    {
        const Sigil s = MakeSigil(Fp(kBodyA + std::to_string(i)));
        REQUIRE(s.nodes.size() >= 5);
        REQUIRE(s.nodes.size() <= 8);
        for (const SigilNode& n : s.nodes)
        {
            CHECK(n.x > 0.0f);
            CHECK(n.x < 1.0f);
            CHECK(n.y > 0.0f);
            CHECK(n.y < 1.0f);
            CHECK(n.r > 0.0f);
            CHECK(n.r < 0.1f);
        }
        // The ring is always present, so the figure is always connected.
        REQUIRE(s.edges.size() >= s.nodes.size());
        for (const SigilEdge& e : s.edges)
        {
            CHECK(e.a >= 0);
            CHECK(e.b >= 0);
            CHECK(e.a < static_cast<int>(s.nodes.size()));
            CHECK(e.b < static_cast<int>(s.nodes.size()));
            // A degenerate self-edge would draw as a dot on a node and read as
            // a filled node instead of a line.
            CHECK(e.a != e.b);
        }
        CHECK(s.rhythm.size() >= 6);
        CHECK(s.rhythm.size() <= 10);
        CHECK(s.hueDeg >= 0.0f);
        CHECK(s.hueDeg < 360.0f);
    }
}

TEST_CASE("Colour is never the only thing telling two hosts apart", "[sigil]")
{
    // A user with a monochrome display, a colour-vision difference, or a
    // screenshot in greyscale must still be able to distinguish hosts. So for
    // every pair of hosts whose hues are close enough to confuse, the shapes
    // must differ.
    std::vector<Sigil> all;
    for (int i = 0; i < 120; ++i)
        all.push_back(MakeSigil(Fp("host-" + std::to_string(i) + kBodyA)));

    int closeHuePairs = 0;
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
        {
            float d = all[i].hueDeg - all[j].hueDeg;
            if (d < 0.0f) d = -d;
            if (d > 180.0f) d = 360.0f - d;
            if (d > 15.0f)
                continue;
            ++closeHuePairs;
            REQUIRE(ShapeSignature(all[i]) != ShapeSignature(all[j]));
        }
    // The test is only meaningful if such pairs actually occurred.
    CHECK(closeHuePairs > 0);
}

TEST_CASE("Distinct hosts get distinct figures", "[sigil]")
{
    std::set<std::string> shapes;
    const int n = 500;
    for (int i = 0; i < n; ++i)
        shapes.insert(ShapeSignature(MakeSigil(Fp(std::to_string(i) + kBodyA))));
    // Geometry is derived from a 64-bit seed, so collisions should not happen
    // at this scale at all.
    CHECK(shapes.size() == static_cast<size_t>(n));
}

TEST_CASE("The mnemonic is pronounceable and stable", "[sigil]")
{
    const std::string m = SigilMnemonic(MakeSigil(Fp(kBodyA)));
    REQUIRE(m.size() == 4);
    const std::string cons = "BDFGHJKMNPRSTVZ";
    const std::string vows = "AEIOU";
    CHECK(cons.find(m[0]) != std::string::npos);
    CHECK(vows.find(m[1]) != std::string::npos);
    CHECK(cons.find(m[2]) != std::string::npos);
    CHECK(vows.find(m[3]) != std::string::npos);
    // No digits, and no consonant that is misread on screen: an L reads as a
    // 1, a C is heard as a K. Position disambiguates the vowels, so I and O
    // are allowed there and only there.
    for (char c : m)
    {
        CHECK(c >= 'A');
        CHECK(c <= 'Z');
    }
    CHECK(m[0] != 'L');
    CHECK(m[2] != 'L');
    CHECK(m[0] != 'C');
    CHECK(m[2] != 'C');
    CHECK(SigilMnemonic(MakeSigil(Fp(kBodyA))) == m);
    CHECK(SigilMnemonic(MakeSigil(Fp(kBodyB))) != m);
}

TEST_CASE("The mnemonic is a shorthand, not an identifier", "[sigil]")
{
    // Four characters cannot carry 256 bits. This test records the honest
    // limit rather than pretending otherwise: mnemonics DO collide, so the
    // UI must never present one as proof of identity — the fingerprint is.
    std::set<std::string> seen;
    int collisions = 0;
    for (int i = 0; i < 2000; ++i)
        if (!seen.insert(SigilMnemonic(MakeSigil(Fp(std::to_string(i))))).second)
            ++collisions;
    CHECK(collisions > 0);
    // Still spread widely, so it is useful as a hint.
    CHECK(seen.size() > 1200);
}

TEST_CASE("The description distinguishes hosts for a screen reader", "[sigil]")
{
    const std::string a = SigilDescribe(MakeSigil(Fp(kBodyA)));
    CHECK_FALSE(a.empty());
    CHECK(a.find("nodes") != std::string::npos);
    CHECK(a.find("filled") != std::string::npos);
    // A sentence is far coarser than the geometry, so it cannot be unique.
    // What it must do is separate most hosts most of the time.
    std::set<std::string> said;
    for (int i = 0; i < 200; ++i)
        said.insert(SigilDescribe(MakeSigil(Fp("h" + std::to_string(i) + kBodyA))));
    CHECK(said.size() > 60);
    // It describes shape, never colour: a description built on hue would be
    // useless to the reader who needs it most.
    CHECK(a.find("hue") == std::string::npos);
    CHECK(a.find("colour") == std::string::npos);
    CHECK(a.find("color") == std::string::npos);
}

TEST_CASE("A sigil is not claimed to come from a real fingerprint", "[sigil]")
{
    // The flag exists so the UI can refuse to present a sigil as an identity
    // check when the text it was given was not actually a key fingerprint.
    CHECK(MakeSigil(Fp(kBodyA)).fromFingerprint);
    CHECK_FALSE(MakeSigil("some-label").fromFingerprint);
    CHECK_FALSE(MakeSigil("SHA256:short").fromFingerprint);
    CHECK_FALSE(MakeSigil("").fromFingerprint);
}

TEST_CASE("An empty fingerprint still produces a drawable figure", "[sigil]")
{
    const Sigil s = MakeSigil("");
    CHECK(s.nodes.size() >= 5);
    CHECK(s.edges.size() >= s.nodes.size());
    CHECK_FALSE(SigilDescribe(s).empty());
}
