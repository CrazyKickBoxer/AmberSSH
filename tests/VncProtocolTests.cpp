// VncProtocolTests.cpp — the RFB codecs and rectangle decoders against
// hand-assembled byte streams.
//
// Provenance: every fixture here is built in the test from RFC 6143's
// message layouts (§7.3–§7.8), field by field, with the expected pixels
// stated beside it. The ZRLE fixtures are deflated in the test with zlib so
// the compressed bytes are reproducible from the tile stream shown; nothing
// is a captured blob whose contents would have to be taken on trust. The
// reference implementation (vncfree) ships no byte fixtures, and Hextile is
// not in it at all, so §7.7.4 is the only source for those.
#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vnc/RfbDecoders.h"
#include "vnc/RfbProtocol.h"

using namespace amber::vnc;

namespace
{

using Bytes = std::vector<uint8_t>;

uint32_t P(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

// one pixel in kRequestedFormat on the wire: B, G, R, X
void Wire(Bytes& v, uint8_t r, uint8_t g, uint8_t b, uint8_t x = 0)
{
    v.push_back(b); v.push_back(g); v.push_back(r); v.push_back(x);
}

// ZRLE's CPIXEL: B, G, R
void CPix(Bytes& v, uint8_t r, uint8_t g, uint8_t b)
{
    v.push_back(b); v.push_back(g); v.push_back(r);
}

void U16(Bytes& v, uint16_t x) { v.push_back(static_cast<uint8_t>(x >> 8)); v.push_back(static_cast<uint8_t>(x)); }
void U32(Bytes& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));  v.push_back(static_cast<uint8_t>(x));
}

// Deflates `in` on a stream that persists across calls, the way a ZRLE
// server's does, with a sync flush so the decoder sees every byte.
struct Deflater
{
    z_stream z{};
    Deflater() { deflateInit(&z, Z_DEFAULT_COMPRESSION); }
    ~Deflater() { deflateEnd(&z); }
    Bytes Do(const Bytes& in)
    {
        Bytes out(in.size() + 64);
        z.next_in = const_cast<Bytef*>(in.data());
        z.avail_in = static_cast<uInt>(in.size());
        z.next_out = out.data();
        z.avail_out = static_cast<uInt>(out.size());
        REQUIRE(deflate(&z, Z_SYNC_FLUSH) == Z_OK);
        out.resize(out.size() - z.avail_out);
        return out;
    }
};

Bytes ZrleRect(const Bytes& compressed)
{
    Bytes v;
    U32(v, static_cast<uint32_t>(compressed.size()));
    v.insert(v.end(), compressed.begin(), compressed.end());
    return v;
}

} // namespace

// ---- pixel format & version ----------------------------------------------------
TEST_CASE("the requested pixel format encodes as RFC 6143 §7.4 lays it out", "[vnc][protocol]")
{
    uint8_t pf[16];
    EncodePixelFormat(kRequestedFormat, pf);
    const uint8_t expect[16] = { 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0 };
    CHECK(std::memcmp(pf, expect, 16) == 0);
    CHECK(DecodePixelFormat(pf) == kRequestedFormat);
}

TEST_CASE("version strings parse and the reply never exceeds the offer", "[vnc][protocol]")
{
    Version v;
    CHECK(ParseVersion(reinterpret_cast<const uint8_t*>("RFB 003.008\n"), v));
    CHECK(v.major == 3);
    CHECK(v.minor == 8);
    CHECK_FALSE(ParseVersion(reinterpret_cast<const uint8_t*>("HTTP/1.1 200"), v));
    CHECK_FALSE(ParseVersion(reinterpret_cast<const uint8_t*>("RFB 003.00x\n"), v));

    CHECK(ChooseMinor({ 3, 3 }) == 3);
    CHECK(ChooseMinor({ 3, 5 }) == 3);     // 3.4-3.6 are answered as 3.3
    CHECK(ChooseMinor({ 3, 7 }) == 7);
    CHECK(ChooseMinor({ 3, 8 }) == 8);
    CHECK(ChooseMinor({ 3, 889 }) == 8);   // UltraVNC-style: still 3.8
    CHECK(ChooseMinor({ 4, 0 }) == -1);
    CHECK(ChooseMinor({ 3, 2 }) == -1);
    const auto r = VersionReply(7);
    CHECK(std::string(r.begin(), r.end()) == "RFB 003.007\n");
}

// ---- reader --------------------------------------------------------------------
TEST_CASE("a short read consumes nothing and marks the reader short", "[vnc][protocol]")
{
    const uint8_t data[3] = { 1, 2, 3 };
    Reader r(data, 3);
    uint32_t v;
    CHECK_FALSE(r.U32(v));
    CHECK(r.Short());
    CHECK(r.Consumed() == 0);
    uint16_t s;
    CHECK(r.U16(s));
    CHECK(s == 0x0102);
    CHECK(r.Remaining() == 1);
}

// ---- client messages -------------------------------------------------------------
TEST_CASE("client messages are laid out as §7.5 specifies", "[vnc][protocol]")
{
    CHECK(MsgFramebufferUpdateRequest(true, 0, 0, 800, 600) == Bytes{ 3, 1, 0, 0, 0, 0, 0x03, 0x20, 0x02, 0x58 });
    CHECK(MsgKeyEvent(true, 0x41) == Bytes{ 4, 1, 0, 0, 0, 0, 0, 0x41 });
    CHECK(MsgKeyEvent(false, 0x01000000 + 0x20AC) == Bytes{ 4, 0, 0, 0, 0x01, 0x00, 0x20, 0xAC });
    CHECK(MsgPointerEvent(kButtonLeft | kButtonWheelUp, 300, 200) == Bytes{ 5, 9, 0x01, 0x2C, 0x00, 0xC8 });
    CHECK(MsgSetEncodings({ EncZRLE, EncPseudoCursor }) ==
          Bytes{ 2, 0, 0, 2, 0, 0, 0, 16, 0xFF, 0xFF, 0xFF, 0x11 });
    CHECK(MsgClientCutText("hi") == Bytes{ 6, 0, 0, 0, 0, 0, 0, 2, 'h', 'i' });
    CHECK(MsgEnableContinuousUpdates(true, 0, 0, 640, 480) == Bytes{ 150, 1, 0, 0, 0, 0, 0x02, 0x80, 0x01, 0xE0 });
    CHECK(MsgClientInit(true) == Bytes{ 1 });
    const Bytes spf = MsgSetPixelFormat(kRequestedFormat);
    REQUIRE(spf.size() == 20);
    CHECK(spf[0] == 0);
    CHECK(spf[4] == 32);
    CHECK(spf[14] == 16);
}

// ---- server messages ------------------------------------------------------------
TEST_CASE("ServerInit is validated before its name is read", "[vnc][protocol]")
{
    Bytes v;
    U16(v, 640); U16(v, 480);
    uint8_t pf[16];
    EncodePixelFormat(kRequestedFormat, pf);
    v.insert(v.end(), pf, pf + 16);
    U32(v, 4);
    v.insert(v.end(), { 'd', 'e', 's', 'k' });

    Reader r(v.data(), v.size());
    ServerInit init;
    CHECK(ParseServerInit(r, init) == Parse::Ok);
    CHECK(init.width == 640);
    CHECK(init.height == 480);
    CHECK(init.name == "desk");
    CHECK(r.Remaining() == 0);

    // partial: nothing consumed
    Reader p(v.data(), v.size() - 2);
    CHECK(ParseServerInit(p, init) == Parse::NeedMore);
    CHECK(p.Consumed() == 0);

    // zero width
    Bytes z = v;
    z[0] = z[1] = 0;
    Reader rz(z.data(), z.size());
    CHECK(ParseServerInit(rz, init) == Parse::Bad);

    // a name length no desktop has
    Bytes big = v;
    big[20] = 0x7F;
    Reader rb(big.data(), big.size());
    CHECK(ParseServerInit(rb, init) == Parse::Bad);
}

TEST_CASE("rectangle bounds use 32-bit arithmetic", "[vnc][protocol]")
{
    RectHeader h;
    h.x = 65535; h.w = 2; h.y = 0; h.h = 1;
    CHECK_FALSE(RectWithin(h, 800, 600));   // 16-bit wrap would pass this
    h.x = 799; h.w = 1; h.y = 599; h.h = 1;
    CHECK(RectWithin(h, 800, 600));
    h.w = 0;
    CHECK_FALSE(RectWithin(h, 800, 600));
}

TEST_CASE("ServerCutText refuses an unbounded length", "[vnc][protocol]")
{
    Bytes v = { 3, 0, 0, 0 };
    U32(v, 5);
    v.insert(v.end(), { 'h', 'e', 'l', 'l', 'o' });
    Reader r(v.data(), v.size());
    std::string t;
    CHECK(ParseServerCutText(r, t) == Parse::Ok);
    CHECK(t == "hello");

    Bytes huge = { 3, 0, 0, 0 };
    U32(huge, 0x80000000u);
    Reader rh(huge.data(), huge.size());
    CHECK(ParseServerCutText(rh, t) == Parse::Bad);
}

TEST_CASE("cut text is Latin-1 with LF endings, and says so about the rest", "[vnc][protocol]")
{
    CHECK(Latin1FromUtf8("caf\xC3\xA9") == "caf\xE9");         // é
    CHECK(Latin1FromUtf8("\xE2\x82\xAC") == "?");              // € has no Latin-1
    CHECK(Latin1FromUtf8("a\r\nb\rc") == "a\nb\nc");
    CHECK(Utf8FromLatin1("caf\xE9") == "caf\xC3\xA9");
    CHECK(Latin1FromUtf8("\xFF") == "?");                      // a stray continuation byte
}

// ---- Raw -------------------------------------------------------------------------
TEST_CASE("Raw writes BGRX bytes as opaque 0xFFRRGGBB", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(4, 4));
    Bytes v;
    Wire(v, 10, 20, 30, 0);    // the X byte is zero on the wire...
    Wire(v, 40, 50, 60, 0x7F);
    Wire(v, 70, 80, 90);
    Wire(v, 1, 2, 3);
    Reader r(v.data(), v.size());
    CHECK(DecodeRaw(r, fb, { 1, 1, 2, 2 }) == Decode::Ok);
    CHECK(fb.Row(1)[1] == P(10, 20, 30));   // ...and alpha is opaque regardless
    CHECK(fb.Row(1)[2] == P(40, 50, 60));
    CHECK(fb.Row(2)[1] == P(70, 80, 90));
    CHECK(fb.Row(2)[2] == P(1, 2, 3));
    CHECK(fb.Row(0)[0] == 0xFF000000u);     // untouched
    CHECK(r.Remaining() == 0);

    Reader partial(v.data(), v.size() - 1);
    CHECK(DecodeRaw(partial, fb, { 1, 1, 2, 2 }) == Decode::NeedMore);
    CHECK(partial.Consumed() == 0);

    Reader oob(v.data(), v.size());
    CHECK(DecodeRaw(oob, fb, { 3, 3, 2, 2 }) == Decode::Bad);
}

// ---- CopyRect --------------------------------------------------------------------
TEST_CASE("CopyRect handles overlap in both directions", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(4, 4));
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            fb.Row(y)[x] = P(static_cast<uint8_t>(x), static_cast<uint8_t>(y), 0);

    // right by one, overlapping: [0 1 2 3] -> [0 0 1 2]
    Bytes v;
    U16(v, 0); U16(v, 0);
    Reader r(v.data(), v.size());
    CHECK(DecodeCopyRect(r, fb, { 1, 0, 3, 1 }) == Decode::Ok);
    CHECK(fb.Row(0)[1] == P(0, 0, 0));
    CHECK(fb.Row(0)[2] == P(1, 0, 0));
    CHECK(fb.Row(0)[3] == P(2, 0, 0));

    // down by one, overlapping, on column 0: rows 1..3 <- rows 0..2
    Bytes d;
    U16(d, 0); U16(d, 0);
    Reader rd(d.data(), d.size());
    CHECK(DecodeCopyRect(rd, fb, { 0, 1, 1, 3 }) == Decode::Ok);
    CHECK(fb.Row(1)[0] == P(0, 0, 0));
    CHECK(fb.Row(2)[0] == P(0, 1, 0));
    CHECK(fb.Row(3)[0] == P(0, 2, 0));

    // up by one on column 3: rows 0..2 <- rows 1..3 (source below destination)
    Bytes u;
    U16(u, 3); U16(u, 1);
    Reader ru(u.data(), u.size());
    CHECK(DecodeCopyRect(ru, fb, { 3, 0, 1, 3 }) == Decode::Ok);
    CHECK(fb.Row(0)[3] == P(3, 1, 0));
    CHECK(fb.Row(2)[3] == P(3, 3, 0));

    // a source off the edge
    Bytes bad;
    U16(bad, 3); U16(bad, 0);
    Reader rb(bad.data(), bad.size());
    CHECK(DecodeCopyRect(rb, fb, { 0, 0, 2, 1 }) == Decode::Bad);
}

// ---- Hextile --------------------------------------------------------------------
TEST_CASE("Hextile background, foreground and subrects (§7.7.4)", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(16, 16));
    Bytes v;
    v.push_back(2 | 4 | 8);      // BackgroundSpecified | ForegroundSpecified | AnySubrects
    Wire(v, 200, 0, 0);          // background red
    Wire(v, 0, 0, 200);          // foreground blue
    v.push_back(1);              // one subrect
    v.push_back(0x23);           // x=2, y=3
    v.push_back(0x34);           // w-1=3, h-1=4 -> 4x5
    HextileDecoder hx;
    hx.BeginRect();
    Reader r(v.data(), v.size());
    CHECK(hx.Decode(r, fb, { 0, 0, 16, 16 }) == Decode::Ok);
    CHECK(fb.Row(0)[0] == P(200, 0, 0));
    CHECK(fb.Row(3)[2] == P(0, 0, 200));
    CHECK(fb.Row(7)[5] == P(0, 0, 200));
    CHECK(fb.Row(8)[5] == P(200, 0, 0));
    CHECK(fb.Row(3)[6] == P(200, 0, 0));
    CHECK(r.Remaining() == 0);
}

TEST_CASE("Hextile coloured subrects, raw tiles, and a tile that arrives in pieces", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(20, 16));   // two tiles across: 16 and 4 wide
    Bytes v;
    // tile 0: coloured subrects on a green background
    v.push_back(2 | 8 | 16);
    Wire(v, 0, 200, 0);
    v.push_back(2);
    Wire(v, 9, 9, 9);  v.push_back(0x00); v.push_back(0x11);   // 2x2 at 0,0
    Wire(v, 7, 7, 7);  v.push_back(0xFF); v.push_back(0x00);   // 1x1 at 15,15
    // tile 1 (4 wide): raw, 4x16 pixels
    v.push_back(1);
    for (int i = 0; i < 4 * 16; ++i)
        Wire(v, static_cast<uint8_t>(i), 1, 2);

    HextileDecoder hx;
    hx.BeginRect();
    // feed everything but the last byte: tile 0 completes, tile 1 waits
    Reader r1(v.data(), v.size() - 1);
    CHECK(hx.Decode(r1, fb, { 0, 0, 20, 16 }) == Decode::NeedMore);
    CHECK(hx.TilesDone() == 1);
    const size_t consumed = r1.Consumed();
    CHECK(consumed > 0);
    CHECK(fb.Row(1)[1] == P(9, 9, 9));
    CHECK(fb.Row(15)[15] == P(7, 7, 7));
    CHECK(fb.Row(5)[5] == P(0, 200, 0));
    // then the rest, from where the first call stopped
    Reader r2(v.data() + consumed, v.size() - consumed);
    CHECK(hx.Decode(r2, fb, { 0, 0, 20, 16 }) == Decode::Ok);
    CHECK(hx.TilesDone() == 2);
    CHECK(fb.Row(0)[16] == P(0, 1, 2));
    CHECK(fb.Row(15)[19] == P(63, 1, 2));
}

TEST_CASE("Hextile refuses a subrect outside its tile", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(8, 8));   // one 8x8 tile
    Bytes v;
    v.push_back(8);
    v.push_back(1);
    v.push_back(0x70);   // x=7
    v.push_back(0x10);   // w=2 -> 7+2 > 8
    HextileDecoder hx;
    hx.BeginRect();
    Reader r(v.data(), v.size());
    CHECK(hx.Decode(r, fb, { 0, 0, 8, 8 }) == Decode::Bad);
}

// ---- ZRLE ------------------------------------------------------------------------
TEST_CASE("ZRLE subencodings decode and the zlib stream spans rectangles", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(70, 6));   // two tiles across: 64 and 6 wide
    Deflater def;
    ZrleDecoder z;
    z.Reset();

    // rectangle 1: tile 0 solid, tile 1 raw
    Bytes tiles;
    tiles.push_back(1);
    CPix(tiles, 10, 20, 30);
    tiles.push_back(0);
    for (int i = 0; i < 6 * 6; ++i)
        CPix(tiles, static_cast<uint8_t>(i), 0, 0);
    const Bytes rect1 = ZrleRect(def.Do(tiles));
    Reader r1(rect1.data(), rect1.size());
    CHECK(z.Decode(r1, fb, { 0, 0, 70, 6 }) == Decode::Ok);
    CHECK(fb.Row(0)[0] == P(10, 20, 30));
    CHECK(fb.Row(5)[63] == P(10, 20, 30));
    CHECK(fb.Row(0)[64] == P(0, 0, 0));
    CHECK(fb.Row(5)[69] == P(35, 0, 0));

    // rectangle 2 on the SAME stream: packed palette (2 colours, 1 bit) over
    // a 8x2 rect, then plain RLE with a run over 255, then palette RLE
    Bytes t2;
    t2.push_back(2);                    // packed, palette of 2
    CPix(t2, 1, 1, 1);
    CPix(t2, 2, 2, 2);
    t2.push_back(0b10101010);           // row 0: 1 0 1 0 1 0 1 0
    t2.push_back(0b11110000);           // row 1: 1 1 1 1 0 0 0 0
    const Bytes rect2 = ZrleRect(def.Do(t2));
    Reader r2(rect2.data(), rect2.size());
    CHECK(z.Decode(r2, fb, { 0, 0, 8, 2 }) == Decode::Ok);
    CHECK(fb.Row(0)[0] == P(2, 2, 2));
    CHECK(fb.Row(0)[1] == P(1, 1, 1));
    CHECK(fb.Row(1)[3] == P(2, 2, 2));
    CHECK(fb.Row(1)[4] == P(1, 1, 1));

    // rectangle 3: plain RLE filling a 64x6 tile with one run of 384
    // (length bytes 255 + 128 -> 383 + 1) then palette RLE on the 6-wide tile
    Bytes t3;
    t3.push_back(128);
    CPix(t3, 5, 6, 7);
    t3.push_back(255);
    t3.push_back(128);
    t3.push_back(130);                  // palette RLE, 2 entries
    CPix(t3, 100, 0, 0);
    CPix(t3, 0, 100, 0);
    t3.push_back(0x80 | 0);             // index 0, run follows
    t3.push_back(17);                   // run 18
    t3.push_back(0x80 | 1);
    t3.push_back(17);                   // run 18: 36 pixels = 6x6
    const Bytes rect3 = ZrleRect(def.Do(t3));
    Reader r3(rect3.data(), rect3.size());
    CHECK(z.Decode(r3, fb, { 0, 0, 70, 6 }) == Decode::Ok);
    CHECK(fb.Row(0)[0] == P(5, 6, 7));
    CHECK(fb.Row(5)[63] == P(5, 6, 7));
    CHECK(fb.Row(0)[64] == P(100, 0, 0));
    CHECK(fb.Row(2)[69] == P(100, 0, 0));
    CHECK(fb.Row(3)[64] == P(0, 100, 0));
    CHECK(fb.Row(5)[69] == P(0, 100, 0));
}

TEST_CASE("ZRLE refuses what the specification leaves unused", "[vnc][decode]")
{
    Framebuffer fb;
    REQUIRE(fb.Resize(4, 4));
    {
        Deflater def;
        ZrleDecoder z;
        Bytes t = { 17 };   // 17..127 unused
        const Bytes rect = ZrleRect(def.Do(t));
        Reader r(rect.data(), rect.size());
        CHECK(z.Decode(r, fb, { 0, 0, 4, 4 }) == Decode::Bad);
    }
    {
        Deflater def;
        ZrleDecoder z;
        Bytes t = { 130 };   // palette RLE with 2 entries, then index 5
        CPix(t, 1, 1, 1);
        CPix(t, 2, 2, 2);
        t.push_back(5);
        const Bytes rect = ZrleRect(def.Do(t));
        Reader r(rect.data(), rect.size());
        CHECK(z.Decode(r, fb, { 0, 0, 4, 4 }) == Decode::Bad);
    }
    {
        Deflater def;
        ZrleDecoder z;
        Bytes t = { 128 };   // a run longer than the tile
        CPix(t, 1, 1, 1);
        t.push_back(255);
        t.push_back(255);
        t.push_back(0);
        const Bytes rect = ZrleRect(def.Do(t));
        Reader r(rect.data(), rect.size());
        CHECK(z.Decode(r, fb, { 0, 0, 4, 4 }) == Decode::Bad);
    }
    {
        ZrleDecoder z;
        Bytes rect;
        U32(rect, 0x7FFFFFFFu);   // a length no rectangle has
        Reader r(rect.data(), rect.size());
        CHECK(z.Decode(r, fb, { 0, 0, 4, 4 }) == Decode::Bad);
    }
    {
        ZrleDecoder z;
        Bytes rect;
        U32(rect, 10);            // body not here yet
        rect.push_back(1);
        Reader r(rect.data(), rect.size());
        CHECK(z.Decode(r, fb, { 0, 0, 4, 4 }) == Decode::NeedMore);
        CHECK(r.Consumed() == 0);
    }
}

// ---- Cursor ----------------------------------------------------------------------
TEST_CASE("Cursor pixels take their alpha from the bitmask", "[vnc][decode]")
{
    Bytes v;
    Wire(v, 1, 2, 3); Wire(v, 4, 5, 6);
    Wire(v, 7, 8, 9); Wire(v, 10, 11, 12);
    v.push_back(0b10000000);   // row 0: pixel 0 opaque, pixel 1 clear
    v.push_back(0b01000000);   // row 1: pixel 0 clear, pixel 1 opaque
    RectHeader h;
    h.x = 1; h.y = 0; h.w = 2; h.h = 2; h.encoding = EncPseudoCursor;
    CursorShape cs;
    Reader r(v.data(), v.size());
    CHECK(DecodeCursor(r, h, cs) == Decode::Ok);
    CHECK(cs.width == 2);
    CHECK(cs.hotX == 1);
    CHECK(cs.bgra[0] == P(1, 2, 3));
    CHECK(cs.bgra[1] == 0x00040506u);
    CHECK(cs.bgra[2] == 0x00070809u);
    CHECK(cs.bgra[3] == P(10, 11, 12));

    RectHeader hidden;
    hidden.w = 0; hidden.h = 0;
    Reader r0(v.data(), 0);
    CHECK(DecodeCursor(r0, hidden, cs) == Decode::Ok);
    CHECK(cs.width == 0);

    RectHeader huge;
    huge.w = 1000; huge.h = 1;
    CHECK(DecodeCursor(r0, huge, cs) == Decode::Bad);
}

// ---- dirty region ------------------------------------------------------------------
TEST_CASE("a storm of dirty rectangles collapses to its bounding box", "[vnc][decode]")
{
    DirtyRegion d;
    for (uint16_t i = 0; i < 100; ++i)
        d.Add({ static_cast<uint16_t>(i * 2), static_cast<uint16_t>(i), 1, 1 });
    CHECK(d.rects.size() <= DirtyRegion::kMaxRects);
    const Rect b = d.Bounds();
    CHECK(b.x == 0);
    CHECK(b.y == 0);
    CHECK(b.x + b.w == 199);
    CHECK(b.y + b.h == 100);
}
