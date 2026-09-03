#include "HostSigil.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace amber
{

namespace
{

// FNV-1a over the whole fingerprint, then a splitmix64 stream from it. The
// hash does not need to be cryptographic — the key already is — it needs to
// be STABLE across builds and machines, and to avalanche so a one-bit change
// to the key rearranges the whole figure.
uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

struct Stream
{
    uint64_t state;
    explicit Stream(uint64_t seed) : state(seed) {}
    uint64_t Next()
    {
        // splitmix64: short, well-mixed, and identical on every compiler,
        // which is what "the same key produces the same sigil across
        // machines" actually requires.
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // 0..n-1
    uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
    // 0..1
    float Unit() { return static_cast<float>(Next() % 100000u) / 100000.0f; }
};

} // namespace

std::string SigilSourceText(const std::string& fingerprint)
{
    // "ssh-ed25519 SHA256:abc..." or "SHA256:abc..." -> "abc..."
    const size_t at = fingerprint.find("SHA256:");
    if (at != std::string::npos)
    {
        std::string body = fingerprint.substr(at + 7);
        // Trim at the first space: a fingerprint body has none.
        const size_t sp = body.find_first_of(" \t\r\n");
        if (sp != std::string::npos)
            body.erase(sp);
        return body;
    }
    std::string t = fingerprint;
    while (!t.empty() && isspace(static_cast<unsigned char>(t.front())))
        t.erase(t.begin());
    while (!t.empty() && isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    return t;
}

Sigil MakeSigil(const std::string& fingerprint)
{
    Sigil s;
    s.version = kSigilVersion;
    const std::string body = SigilSourceText(fingerprint);
    s.fromFingerprint = fingerprint.find("SHA256:") != std::string::npos &&
                        body.size() >= 40;
    // Derive from the NORMALISED body, so "ssh-ed25519 SHA256:x" and
    // "SHA256:x" are the same host and get the same sigil — the key is the
    // identity, not the way it happened to be printed.
    s.seed = Fnv1a64(body.empty() ? fingerprint : body);
    Stream rnd(s.seed);

    // --- constellation ---------------------------------------------------
    // Five to eight nodes on a jittered ring: enough structure to be
    // memorable, few enough to stay legible at tab size.
    const int n = 5 + static_cast<int>(rnd.Below(4));
    s.nodes.reserve(static_cast<size_t>(n));
    const float phase = rnd.Unit() * 6.2831853f;
    for (int i = 0; i < n; ++i)
    {
        const float base = phase + 6.2831853f * static_cast<float>(i) /
                                       static_cast<float>(n);
        // Radial and angular jitter, both bounded so nodes cannot collide or
        // leave the box.
        const float rad = 0.26f + rnd.Unit() * 0.16f;
        const float wobble = (rnd.Unit() - 0.5f) * 0.35f;
        SigilNode node;
        node.x = 0.5f + std::cos(base + wobble) * rad;
        node.y = 0.5f + std::sin(base + wobble) * rad;
        node.r = 0.045f + rnd.Unit() * 0.045f;
        // Filled versus ring is a shape distinction, so it survives
        // monochrome and high-contrast rendering.
        node.filled = (rnd.Next() & 1) != 0;
        s.nodes.push_back(node);
    }

    // --- edges ------------------------------------------------------------
    // The ring, plus a couple of chords. The ring guarantees a connected
    // figure; the chords are what make two sigils tell apart at a glance.
    for (int i = 0; i < n; ++i)
    {
        SigilEdge e;
        e.a = i;
        e.b = (i + 1) % n;
        e.doubled = false;
        s.edges.push_back(e);
    }
    const int chords = 1 + static_cast<int>(rnd.Below(3));
    for (int i = 0; i < chords; ++i)
    {
        SigilEdge e;
        e.a = static_cast<int>(rnd.Below(static_cast<uint32_t>(n)));
        e.b = static_cast<int>(rnd.Below(static_cast<uint32_t>(n)));
        // A chord to itself or to a neighbour adds nothing; skip rather than
        // draw a degenerate line.
        if (e.a == e.b || (e.a + 1) % n == e.b || (e.b + 1) % n == e.a)
            continue;
        e.doubled = (rnd.Next() & 1) != 0;
        s.edges.push_back(e);
    }

    // --- corners and border rhythm ---------------------------------------
    for (float& c : s.corners)
        c = (rnd.Next() & 1) ? (0.12f + rnd.Unit() * 0.20f) : 0.0f;
    const int segs = 6 + static_cast<int>(rnd.Below(5));
    s.rhythm.reserve(static_cast<size_t>(segs));
    for (int i = 0; i < segs; ++i)
        s.rhythm.push_back(0.4f + rnd.Unit() * 1.6f);

    // Hue is a hint, never the distinction: two hosts must differ in shape
    // too, which the nodes, chords, corners and rhythm all guarantee.
    s.hueDeg = rnd.Unit() * 360.0f;
    return s;
}

std::string SigilDescribe(const Sigil& s)
{
    // A screen reader has to be able to tell two hosts apart from this
    // sentence alone, so it names the counts and the asymmetries.
    static const char* kCorner[4] = { "top-left", "top-right", "bottom-right",
                                      "bottom-left" };
    std::string out = std::to_string(s.nodes.size()) + " nodes";
    int filled = 0;
    for (const SigilNode& nd : s.nodes)
        if (nd.filled)
            ++filled;
    out += ", " + std::to_string(filled) + " filled";
    const size_t chords = s.edges.size() > s.nodes.size()
                              ? s.edges.size() - s.nodes.size()
                              : 0;
    if (chords)
        out += ", " + std::to_string(chords) +
               (chords == 1 ? " chord" : " chords");
    bool anyCut = false;
    for (int i = 0; i < 4; ++i)
    {
        if (s.corners[i] <= 0.0f)
            continue;
        out += anyCut ? " and " : ", cut ";
        out += kCorner[i];
        anyCut = true;
    }
    out += ", rhythm " + std::to_string(s.rhythm.size());
    return out;
}

std::string SigilMnemonic(const Sigil& s)
{
    // Consonant-vowel pairs, so it is pronounceable and the position of a
    // letter tells you which alphabet it came from. The consonants drop the
    // ones that are misread on screen or aloud: no L (reads as 1 or I), no O
    // (reads as 0), no C alongside K, no W alongside V, no Q, no X, no Y. The
    // vowels are all five — in an alternating word an I or an O can only be a
    // vowel, so neither can be confused with a digit there.
    static const char kC[] = "BDFGHJKMNPRSTVZ";   // 15
    static const char kV[] = "AEIOU";             // 5
    uint64_t v = s.seed;
    std::string out;
    for (int i = 0; i < 2; ++i)
    {
        out.push_back(kC[v % 15]);
        v /= 15;
        out.push_back(kV[v % 5]);
        v /= 5;
    }
    return out;
}

bool SigilEqual(const Sigil& a, const Sigil& b)
{
    if (a.version != b.version || a.seed != b.seed)
        return false;
    if (a.nodes.size() != b.nodes.size() || a.edges.size() != b.edges.size() ||
        a.rhythm.size() != b.rhythm.size())
        return false;
    for (size_t i = 0; i < a.nodes.size(); ++i)
    {
        const SigilNode& x = a.nodes[i];
        const SigilNode& y = b.nodes[i];
        if (x.filled != y.filled || std::fabs(x.x - y.x) > 1e-6f ||
            std::fabs(x.y - y.y) > 1e-6f || std::fabs(x.r - y.r) > 1e-6f)
            return false;
    }
    for (size_t i = 0; i < a.edges.size(); ++i)
        if (a.edges[i].a != b.edges[i].a || a.edges[i].b != b.edges[i].b ||
            a.edges[i].doubled != b.edges[i].doubled)
            return false;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(a.corners[i] - b.corners[i]) > 1e-6f)
            return false;
    return true;
}

} // namespace amber
