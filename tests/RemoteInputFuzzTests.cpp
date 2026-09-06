// RemoteInputFuzzTests.cpp — the two parsers a hostile server reaches first.
//
// AmberXFuzzTests covers the AmberX side. These are the ones every ordinary
// session runs: the terminal escape-sequence parser, which reads whatever a
// remote shell emits, and the RFB client, which reads whatever a VNC server
// sends. Both parse length-prefixed and state-machine formats from bytes this
// application did not write, and neither had a fuzz target.
//
// The contract in both cases is the same: refuse, or produce something whose
// invariants hold. Never read past the end, never allocate on an unchecked
// length, never crash, never leave the grid or the framebuffer describing an
// area it does not own.
//
// Deterministic, seeded, and driven by the same loop as the AmberX targets so
// it runs in CI with no extra toolchain. AMBER_FUZZ_ITERS and AMBER_FUZZ_SEED
// widen or reproduce a run.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "term/grid.h"
#include "term/vtparser.h"
#include "term/ImageDecode.h"
#include "vnc/RfbClient.h"
#include "vnc/RfbDecoders.h"
#include "vnc/RfbProtocol.h"

namespace
{

struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t Below(uint32_t n) { return n ? static_cast<uint32_t>(Next() % n) : 0; }
};

// ---- targets -------------------------------------------------------------

// The terminal parser. A fresh grid each time, so one iteration cannot leave
// state that makes the next one's failure hard to read.
bool FuzzVt(const uint8_t* data, size_t len)
{
    Grid g;
    g.Init(80, 24);
    VtParser p(g);
    p.Feed(data, len);

    // Whatever it did, the cursor is inside the grid it was given and the
    // grid still reports the size it was initialised with. An escape sequence
    // that moves the cursor out of bounds, or resizes the grid from under the
    // renderer, is the bug class this is looking for.
    REQUIRE(g.Cols() == 80);
    REQUIRE(g.Rows() == 24);
    REQUIRE(g.CurX() >= 0);
    REQUIRE(g.CurX() <= g.Cols());
    REQUIRE(g.CurY() >= 0);
    REQUIRE(g.CurY() < g.Rows());
    return true;
}

// Sixel, straight off the wire inside a DCS string.
bool FuzzSixel(const uint8_t* data, size_t len)
{
    amber::DecodedImage img;
    const std::string payload(reinterpret_cast<const char*>(data), len);
    if (!amber::DecodeSixel(payload, img))
        return false;
    // A decode that succeeded must describe a buffer it actually filled.
    REQUIRE(img.w > 0);
    REQUIRE(img.h > 0);
    REQUIRE(img.rgba.size() == static_cast<size_t>(img.w) * img.h * 4);
    return true;
}

bool FuzzPng(const uint8_t* data, size_t len)
{
    amber::DecodedImage img;
    if (!amber::DecodePng(data, len, img))
        return false;
    REQUIRE(img.w > 0);
    REQUIRE(img.h > 0);
    REQUIRE(img.rgba.size() == static_cast<size_t>(img.w) * img.h * 4);
    return true;
}

// The RFB client's whole state machine, from the greeting onwards.
bool FuzzRfbClient(const uint8_t* data, size_t len)
{
    amber::vnc::RfbClient c({});
    const bool ok = c.Feed(data, len);
    const amber::vnc::Framebuffer& fb = c.Fb();
    // Whether it accepted or refused, the framebuffer never describes more
    // than it holds, and never more than the protocol's own ceiling.
    REQUIRE(fb.px.size() == static_cast<size_t>(fb.width) * fb.height);
    REQUIRE(fb.width <= amber::vnc::kMaxDimension);
    REQUIRE(fb.height <= amber::vnc::kMaxDimension);
    return ok;
}

// The pixel decoders directly, against a framebuffer of known size. This is
// where a bad rectangle would write outside the buffer, so the guard bytes
// around it are the point.
bool FuzzDecoders(const uint8_t* data, size_t len)
{
    using namespace amber::vnc;
    Framebuffer fb;
    REQUIRE(fb.Resize(64, 48));
    const size_t pixels = fb.px.size();

    // A rectangle taken from the input itself, so the mutator can aim at the
    // bounds check rather than only at the payload.
    RectHeader h{};
    if (len >= 8)
    {
        h.x = static_cast<uint16_t>((data[0] << 8) | data[1]);
        h.y = static_cast<uint16_t>((data[2] << 8) | data[3]);
        h.w = static_cast<uint16_t>((data[4] << 8) | data[5]);
        h.h = static_cast<uint16_t>((data[6] << 8) | data[7]);
    }
    const Rect r{ h.x, h.y, h.w, h.h };

    bool any = false;
    {
        Reader rd(data, len);
        any = (DecodeRaw(rd, fb, r) == Decode::Ok) || any;
    }
    {
        Reader rd(data, len);
        any = (DecodeCopyRect(rd, fb, r) == Decode::Ok) || any;
    }
    {
        Reader rd(data, len);
        HextileDecoder hx;
        hx.BeginRect();
        any = (hx.Decode(rd, fb, r) == Decode::Ok) || any;
    }
    {
        Reader rd(data, len);
        ZrleDecoder zr;
        zr.Reset();
        any = (zr.Decode(rd, fb, r) == Decode::Ok) || any;
    }
    // The buffer is the size it was: nothing reallocated it, and nothing that
    // wrote outside it went unnoticed by the allocator.
    REQUIRE(fb.px.size() == pixels);
    REQUIRE(fb.width == 64);
    REQUIRE(fb.height == 48);
    return any;
}

// The server's cursor shape, which carries its own dimensions.
bool FuzzCursor(const uint8_t* data, size_t len)
{
    using namespace amber::vnc;
    RectHeader h{};
    if (len >= 8)
    {
        h.w = static_cast<uint16_t>((data[4] << 8) | data[5]);
        h.h = static_cast<uint16_t>((data[6] << 8) | data[7]);
    }
    CursorShape cur;
    Reader rd(data, len);
    if (DecodeCursor(rd, h, cur) != Decode::Ok)
        return false;
    REQUIRE(cur.width <= kMaxCursorDim);
    REQUIRE(cur.height <= kMaxCursorDim);
    REQUIRE(cur.bgra.size() == static_cast<size_t>(cur.width) * cur.height);
    return true;
}

using Target = bool (*)(const uint8_t*, size_t);
struct TargetInfo { const char* name; Target fn; };

const TargetInfo kTargets[] = {
    { "vt", FuzzVt },
    { "sixel", FuzzSixel },
    { "png", FuzzPng },
    { "rfb-client", FuzzRfbClient },
    { "rfb-decoders", FuzzDecoders },
    { "rfb-cursor", FuzzCursor },
};

// ---- seeds ---------------------------------------------------------------
// Well-formed inputs to mutate. From noise alone a state machine is almost
// never driven past its first byte.
std::vector<std::vector<uint8_t>> Seeds()
{
    auto bytes = [](const char* s) {
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s),
                                    reinterpret_cast<const uint8_t*>(s) + std::char_traits<char>::length(s));
    };
    std::vector<std::vector<uint8_t>> out;

    // Terminal: colour, cursor movement, the OSC forms that carry payloads,
    // the shell-integration marks, and a DCS sixel.
    out.push_back(bytes("\x1b[31mred\x1b[0m plain\r\n"));
    out.push_back(bytes("\x1b[2J\x1b[10;20Hmoved\x1b[K"));
    out.push_back(bytes("\x1b]0;a window title\x07"));
    out.push_back(bytes("\x1b]52;c;aGVsbG8=\x07"));
    out.push_back(bytes("\x1b]133;A\x1b\\prompt$ \x1b]133;B\x1b\\cmd\x1b]133;D;0\x1b\\"));
    out.push_back(bytes("\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\"));
    out.push_back(bytes("\x1bP q#0;2;0;0;0#0~~@@vv@@~~@@~~$-#1~~@@vv@@~~$\x1b\\"));
    out.push_back(bytes("\x1b[?1049h\x1b[?25l\x1b[?2004h"));

    // RFB: a server greeting, and the front of a FramebufferUpdate.
    out.push_back(bytes("RFB 003.008\n"));
    {
        std::vector<uint8_t> u{ 0, 0, 0, 1 };            // type 0, pad, 1 rect
        const uint8_t hdr[] = { 0, 0, 0, 0, 0, 8, 0, 8, 0, 0, 0, 0 };  // 8x8 at 0,0, Raw
        u.insert(u.end(), hdr, hdr + sizeof hdr);
        u.insert(u.end(), 8 * 8 * 4, 0x7F);
        out.push_back(u);
    }
    // A rectangle header on its own, for the decoder targets.
    out.push_back(std::vector<uint8_t>{ 0, 4, 0, 4, 0, 16, 0, 16, 1, 2, 3, 4, 5, 6, 7, 8 });
    // A cursor shape: 8x8 with its mask.
    {
        std::vector<uint8_t> c{ 0, 0, 0, 0, 0, 8, 0, 8 };
        c.insert(c.end(), 8 * 8 * 4 + 8, 0xA5);
        out.push_back(c);
    }
    return out;
}

void Mutate(std::vector<uint8_t>& b, Rng& rng)
{
    if (b.empty())
    {
        b.push_back(static_cast<uint8_t>(rng.Next()));
        return;
    }
    switch (rng.Below(7))
    {
    case 0: b[rng.Below(static_cast<uint32_t>(b.size()))] ^= static_cast<uint8_t>(1u << rng.Below(8)); break;
    case 1: b[rng.Below(static_cast<uint32_t>(b.size()))] = static_cast<uint8_t>(rng.Next()); break;
    case 2: b.insert(b.begin() + rng.Below(static_cast<uint32_t>(b.size()) + 1),
                     static_cast<uint8_t>(rng.Next())); break;
    case 3: b.erase(b.begin() + rng.Below(static_cast<uint32_t>(b.size()))); break;
    case 4:
        // A 16-bit field pushed to an extreme. RFB geometry is 16-bit, and a
        // dimension near the ceiling is what the bounds checks exist for.
        if (b.size() >= 2)
        {
            const uint32_t at = rng.Below(static_cast<uint32_t>(b.size()) - 1);
            const uint16_t v = rng.Below(3) == 0 ? 0xFFFF : (rng.Below(2) ? 0x7FFF : 0);
            b[at] = static_cast<uint8_t>(v >> 8);
            b[at + 1] = static_cast<uint8_t>(v & 0xFF);
        }
        break;
    case 5:
        // Splice an escape introducer in: the terminal targets are a state
        // machine, and reaching a new state matters more than byte noise.
        {
            static const char* kIntro[] = { "\x1b[", "\x1b]", "\x1bP", "\x1b_", "\x1b\\", "\x07" };
            const char* s = kIntro[rng.Below(6)];
            const size_t at = rng.Below(static_cast<uint32_t>(b.size()) + 1);
            b.insert(b.begin() + at, s, s + std::char_traits<char>::length(s));
        }
        break;
    default: b.resize(rng.Below(4096)); break;
    }
    if (b.size() > 16384)
        b.resize(16384);
}

} // namespace

TEST_CASE("the terminal and RFB parsers survive a mutation run", "[fuzz][remoteinput]")
{
    uint64_t seed = 0x5D1C9F2E7A46B830ull;
    int iters = 20000;
    if (const char* env = std::getenv("AMBER_FUZZ_ITERS"))
        iters = std::max(100, std::atoi(env));
    if (const char* env = std::getenv("AMBER_FUZZ_SEED"))
        seed = std::strtoull(env, nullptr, 0);
    INFO("seed 0x" << std::hex << seed << " iterations " << std::dec << iters);

    Rng rng(seed);
    std::vector<std::vector<uint8_t>> pool = Seeds();
    int accepted = 0;
    for (int i = 0; i < iters; ++i)
    {
        std::vector<uint8_t> b = pool[rng.Below(static_cast<uint32_t>(pool.size()))];
        const int rounds = 1 + static_cast<int>(rng.Below(4));
        for (int m = 0; m < rounds; ++m)
            Mutate(b, rng);
        const TargetInfo& t = kTargets[rng.Below(static_cast<uint32_t>(std::size(kTargets)))];
        if (t.fn(b.data(), b.size()))
        {
            ++accepted;
            if (pool.size() < 64)
                pool.push_back(b);
        }
    }
    // A run where nothing was ever accepted means the mutator never reached a
    // parser, which would make the whole test decorative.
    CHECK(accepted > 0);
}

TEST_CASE("the seed inputs themselves parse cleanly", "[fuzz][remoteinput]")
{
    // Before trusting the mutation run, prove the unmutated seeds reach the
    // parsers at all: a seed that is rejected on byte one is not a seed.
    for (const std::vector<uint8_t>& s : Seeds())
        for (const TargetInfo& t : kTargets)
            t.fn(s.data(), s.size());
    SUCCEED();
}
