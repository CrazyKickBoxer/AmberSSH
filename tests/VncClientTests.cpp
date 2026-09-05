// VncClientTests.cpp — the RFB client state machine driven through the
// in-process fake server: every handshake dialect, every failure the server
// can express, updates in each encoding, the pseudo-encodings, and the
// stream fed one byte at a time.
//
// The three version dialects are the tests that matter most. A client that
// waits for a SecurityResult a 3.7 server never sends, or answers a 3.3
// server's single security type, or asks a 3.7 server why it refused, does
// not get a wrong answer — it hangs. So each of those is a test that feeds
// exactly what that server version sends and asserts on exactly what the
// client sent back, byte for byte.
#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "FakeRfbServer.h"
#include "vnc/RfbClient.h"
#include "vnc/RfbDes.h"

using namespace amber::vnc;
using fakerfb::Bytes;
using fakerfb::Server;

namespace
{

Bytes Take(RfbClient& c) { return c.TakeOutput(); }

bool Feed(RfbClient& c, const Bytes& b) { return c.Feed(b.data(), b.size()); }

uint32_t P(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}
void Wire(Bytes& v, uint8_t r, uint8_t g, uint8_t b) { v.push_back(b); v.push_back(g); v.push_back(r); v.push_back(0); }
void CPix(Bytes& v, uint8_t r, uint8_t g, uint8_t b) { v.push_back(b); v.push_back(g); v.push_back(r); }

// A client brought to Ready against a `minor` server with security None.
// Returns the bytes the client sent after ServerInit (pixel format,
// encodings, first request) for the caller to inspect.
Bytes Connect(RfbClient& c, Server& s)
{
    REQUIRE(Feed(c, s.Greeting()));
    Take(c);
    if (s.minor == 3)
    {
        REQUIRE(Feed(c, s.SecurityOffer({ SecNone })));
    }
    else
    {
        REQUIRE(Feed(c, s.SecurityOffer({ SecNone })));
        Take(c);   // the choice
        if (s.minor >= 8)
            REQUIRE(Feed(c, s.SecurityResult(true)));
    }
    Take(c);   // ClientInit
    REQUIRE(Feed(c, s.ServerInit()));
    REQUIRE(c.State() == ClientState::Ready);
    return Take(c);
}

Bytes Deflate(z_stream& z, const Bytes& in)
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

} // namespace

// ---- the three dialects, security None ---------------------------------------------
TEST_CASE("RFB 3.8 with None: choice, SecurityResult, ClientInit", "[vnc][client]")
{
    Server s;
    s.minor = 8;
    RfbClient c({});
    REQUIRE(Feed(c, s.Greeting()));
    CHECK(Take(c) == Bytes{ 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '8', '\n' });
    REQUIRE(Feed(c, s.SecurityOffer({ SecVncAuth, SecNone })));
    CHECK(Take(c) == Bytes{ SecNone });          // no password: None is what can succeed
    CHECK(c.State() == ClientState::SecurityResult);
    REQUIRE(Feed(c, s.SecurityResult(true)));
    CHECK(Take(c) == Bytes{ 1 });                // ClientInit, shared
    CHECK(c.State() == ClientState::ServerInit);
    REQUIRE(Feed(c, s.ServerInit()));
    CHECK(c.State() == ClientState::Ready);
    CHECK(c.Init().width == 64);
    CHECK(c.Init().name == "fake desktop");

    size_t off = 0;
    const Bytes out = Take(c);
    const auto msgs = fakerfb::ParseClientMessages(out, off);
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0].type == CSetPixelFormat);
    CHECK(msgs[0].format == kRequestedFormat);
    CHECK(msgs[1].type == CSetEncodings);
    CHECK(msgs[1].encodings.front() == EncZRLE);
    CHECK(msgs[1].encodings.back() == EncPseudoContinuousUpdates);
    CHECK(msgs[2].type == CFramebufferUpdateRequest);
    CHECK_FALSE(msgs[2].incremental);            // the first picture is a full one
    CHECK(msgs[2].w == 64);
    CHECK(msgs[2].h == 48);
    CHECK(off == out.size());
    CHECK(c.OutstandingRequests() == 1);
}

TEST_CASE("RFB 3.7 with None: no SecurityResult — ServerInit follows the choice", "[vnc][client]")
{
    Server s;
    s.minor = 7;
    RfbClient c({});
    REQUIRE(Feed(c, s.Greeting()));
    CHECK(Take(c).back() == '\n');
    REQUIRE(Feed(c, s.SecurityOffer({ SecNone })));
    CHECK(Take(c) == Bytes{ SecNone, 1 });       // choice, then ClientInit at once
    CHECK(c.State() == ClientState::ServerInit); // a client waiting for a result here would deadlock
    REQUIRE(Feed(c, s.ServerInit()));
    CHECK(c.State() == ClientState::Ready);
}

TEST_CASE("RFB 3.3 with None: the server states the type, the client sends no choice", "[vnc][client]")
{
    Server s;
    s.minor = 3;
    RfbClient c({});
    REQUIRE(Feed(c, s.Greeting()));
    CHECK(Take(c) == Bytes{ 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '3', '\n' });
    REQUIRE(Feed(c, s.SecurityOffer({ SecNone })));
    CHECK(Take(c) == Bytes{ 1 });                // ClientInit only: a stray choice byte would land in it
    REQUIRE(Feed(c, s.ServerInit()));
    CHECK(c.State() == ClientState::Ready);
    CHECK(c.NegotiatedMinor() == 3);
}

TEST_CASE("a 3.5 server is answered as 3.3", "[vnc][client]")
{
    Server s;
    s.minor = 5;
    RfbClient c({});
    REQUIRE(Feed(c, s.Greeting()));
    CHECK(Take(c)[10] == '3');
    CHECK(c.NegotiatedMinor() == 3);
}

// ---- VNC Authentication ---------------------------------------------------------------
TEST_CASE("RFB 3.3 with VNC Authentication: challenge, response, result", "[vnc][client]")
{
    Server s;
    s.minor = 3;
    ClientOptions o;
    o.password = "secret";
    RfbClient c(o);
    REQUIRE(Feed(c, s.Greeting()));
    Take(c);
    REQUIRE(Feed(c, s.SecurityOffer({ SecVncAuth })));
    CHECK(Take(c).empty());                      // 3.3: nothing to choose
    CHECK(c.State() == ClientState::VncAuthChallenge);
    REQUIRE(Feed(c, s.Challenge()));
    uint8_t expect[16];
    VncAuthResponse("secret", s.challenge, expect);
    CHECK(Take(c) == Bytes(expect, expect + 16));
    CHECK(c.State() == ClientState::SecurityResult);   // 3.3 does send one after VNC auth
    REQUIRE(Feed(c, s.SecurityResult(true)));
    CHECK(Take(c) == Bytes{ 1 });
    REQUIRE(Feed(c, s.ServerInit()));
    CHECK(c.State() == ClientState::Ready);
}

TEST_CASE("RFB 3.8 with a wrong password reports the server's reason and does not retry", "[vnc][client]")
{
    Server s;
    s.minor = 8;
    ClientOptions o;
    o.password = "wrong";
    RfbClient c(o);
    REQUIRE(Feed(c, s.Greeting()));
    REQUIRE(Feed(c, s.SecurityOffer({ SecVncAuth, SecNone })));
    CHECK(Take(c).back() == SecVncAuth);         // a password means VNC auth is what was meant
    REQUIRE(Feed(c, s.Challenge()));
    Take(c);
    // the result arrives in two pieces: the code, then the reason
    Bytes res = s.SecurityResult(false, "Authentication failure");
    const Bytes head(res.begin(), res.begin() + 6);
    const Bytes tail(res.begin() + 6, res.end());
    REQUIRE(Feed(c, head));
    CHECK(c.State() == ClientState::SecurityResult);
    CHECK_FALSE(Feed(c, tail));
    CHECK(c.State() == ClientState::Failed);
    CHECK(c.Failure() == FailureKind::Authentication);
    CHECK(c.Error() == "Authentication failure");
}

TEST_CASE("RFB 3.7 with a wrong password fails without asking for a reason", "[vnc][client]")
{
    Server s;
    s.minor = 7;
    ClientOptions o;
    o.password = "wrong";
    RfbClient c(o);
    REQUIRE(Feed(c, s.Greeting()));
    REQUIRE(Feed(c, s.SecurityOffer({ SecVncAuth })));
    REQUIRE(Feed(c, s.Challenge()));
    CHECK_FALSE(Feed(c, s.SecurityResult(false)));   // four bytes and nothing more
    CHECK(c.Failure() == FailureKind::Authentication);
}

TEST_CASE("a refusal carries its reason in every dialect", "[vnc][client]")
{
    for (int minor : { 3, 7, 8 })
    {
        Server s;
        s.minor = minor;
        RfbClient c({});
        REQUIRE(Feed(c, s.Greeting()));
        CHECK_FALSE(Feed(c, s.Refusal("Too many connections")));
        CHECK(c.State() == ClientState::Failed);
        CHECK(c.Failure() == FailureKind::Refused);
        CHECK(c.Error() == "Too many connections");
    }
}

TEST_CASE("no security type in common is an authentication failure, not a retry", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    REQUIRE(Feed(c, s.Greeting()));
    CHECK_FALSE(Feed(c, s.SecurityOffer({ 16, 30 })));   // Tight, ARD
    CHECK(c.Failure() == FailureKind::Authentication);
}

TEST_CASE("a policy that forbids None refuses a None-only server", "[vnc][client]")
{
    Server s;
    ClientOptions o;
    o.allowNone = false;
    RfbClient c(o);
    REQUIRE(Feed(c, s.Greeting()));
    CHECK_FALSE(Feed(c, s.SecurityOffer({ SecNone })));
    CHECK(c.Failure() == FailureKind::Authentication);
}

TEST_CASE("not an RFB server, or an unsupported version, fails as protocol", "[vnc][client]")
{
    {
        RfbClient c({});
        const std::string h = "HTTP/1.1 400\r\n";
        CHECK_FALSE(c.Feed(reinterpret_cast<const uint8_t*>(h.data()), h.size()));
        CHECK(c.Failure() == FailureKind::Protocol);
    }
    {
        RfbClient c({});
        const std::string v = "RFB 004.000\n";
        CHECK_FALSE(c.Feed(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
        CHECK(c.Failure() == FailureKind::Protocol);
        CHECK(c.Error().find("4.0") != std::string::npos);
    }
}

// ---- updates ----------------------------------------------------------------------------
TEST_CASE("an update in every encoding lands in the framebuffer, and the next is requested first", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);

    // header alone: the client must already ask for the next frame
    REQUIRE(Feed(c, Server::UpdateHeader(4)));
    {
        size_t off = 0;
        const Bytes out = Take(c);
        const auto msgs = fakerfb::ParseClientMessages(out, off);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].type == CFramebufferUpdateRequest);
        CHECK(msgs[0].incremental);
    }
    CHECK(c.OutstandingRequests() == 1);

    // 1: Raw 2x2 at (0,0)
    Bytes r1 = Server::RectHeader(0, 0, 2, 2, EncRaw);
    Wire(r1, 1, 2, 3); Wire(r1, 4, 5, 6); Wire(r1, 7, 8, 9); Wire(r1, 10, 11, 12);
    REQUIRE(Feed(c, r1));
    // 2: CopyRect of that to (10,10)
    Bytes r2 = Server::RectHeader(10, 10, 2, 2, EncCopyRect);
    fakerfb::U16(r2, 0); fakerfb::U16(r2, 0);
    REQUIRE(Feed(c, r2));
    // 3: Hextile 16x16 at (20,0): background only
    Bytes r3 = Server::RectHeader(20, 0, 16, 16, EncHextile);
    r3.push_back(2);
    Wire(r3, 50, 60, 70);
    REQUIRE(Feed(c, r3));
    // 4: ZRLE 64x8 at (0,40): one solid tile
    z_stream z{};
    REQUIRE(deflateInit(&z, Z_DEFAULT_COMPRESSION) == Z_OK);
    Bytes tiles = { 1 };
    CPix(tiles, 90, 91, 92);
    const Bytes body = Deflate(z, tiles);
    Bytes r4 = Server::RectHeader(0, 40, 64, 8, EncZRLE);
    fakerfb::U32(r4, static_cast<uint32_t>(body.size()));
    r4.insert(r4.end(), body.begin(), body.end());
    REQUIRE(Feed(c, r4));
    deflateEnd(&z);

    const Framebuffer& fb = c.Fb();
    CHECK(fb.Row(0)[0] == P(1, 2, 3));
    CHECK(fb.Row(1)[1] == P(10, 11, 12));
    CHECK(fb.Row(10)[10] == P(1, 2, 3));
    CHECK(fb.Row(11)[11] == P(10, 11, 12));
    CHECK(fb.Row(5)[25] == P(50, 60, 70));
    CHECK(fb.Row(47)[63] == P(90, 91, 92));
    CHECK(c.TakeUpdatesCompleted() == 1);
    CHECK(c.Dirty().rects.size() == 5);          // the initial full one plus four
    CHECK(c.State() == ClientState::Ready);
}

TEST_CASE("the whole session works one byte at a time", "[vnc][client]")
{
    // Build the complete server stream, then feed it byte by byte and
    // compare the client's total output with the all-at-once run.
    Server s;
    s.minor = 8;
    Bytes stream;
    auto add = [&](const Bytes& b) { stream.insert(stream.end(), b.begin(), b.end()); };
    add(s.Greeting());
    add(s.SecurityOffer({ SecNone }));
    add(s.SecurityResult(true));
    add(s.ServerInit());
    add(Server::UpdateHeader(2));
    // the Raw rectangle sits outside the Hextile tile that follows, so both
    // survive; the tile's background would otherwise overwrite it in order
    Bytes r1 = Server::RectHeader(21, 1, 2, 1, EncRaw);
    Wire(r1, 1, 1, 1); Wire(r1, 2, 2, 2);
    add(r1);
    Bytes r2 = Server::RectHeader(0, 0, 16, 16, EncHextile);
    r2.push_back(2 | 8);
    Wire(r2, 3, 3, 3);
    r2.push_back(1);
    r2.push_back(0x55); r2.push_back(0x22);   // 3x3 at 5,5 in the foreground (unspecified: black)
    add(r2);
    add(Server::CutText("hi"));
    add(Server::Bell());

    RfbClient whole({});
    REQUIRE(whole.Feed(stream.data(), stream.size()));
    const Bytes outWhole = Take(whole);

    RfbClient bits({});
    for (uint8_t b : stream)
        REQUIRE(bits.Feed(&b, 1));
    const Bytes outBits = Take(bits);

    CHECK(outWhole == outBits);
    CHECK(bits.State() == ClientState::Ready);
    CHECK(bits.Fb().Row(1)[22] == P(2, 2, 2));
    CHECK(bits.Fb().Row(1)[21] == P(1, 1, 1));
    CHECK(bits.Fb().Row(5)[5] == 0xFF000000u);     // the subrect, in the unspecified (black) foreground
    CHECK(bits.Fb().Row(1)[1] == P(3, 3, 3));      // the tile's background
    CHECK(bits.TakeCutTexts() == std::vector<std::string>{ "hi" });
    CHECK(bits.TakeBells() == 1);
    CHECK(bits.TakeUpdatesCompleted() == 1);
}

TEST_CASE("DesktopSize resizes before the bounds check and asks for a full picture", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    Take(c);
    REQUIRE(Feed(c, Server::RectHeader(0, 0, 100, 80, EncPseudoDesktopSize)));   // outside the 64x48 buffer
    CHECK(c.Fb().width == 100);
    CHECK(c.Fb().height == 80);
    CHECK(c.Init().width == 100);
    CHECK(c.TakeResized());
    CHECK_FALSE(c.TakeResized());
    const Rect b = c.Dirty().Bounds();
    CHECK(b.w == 100);
    CHECK(b.h == 80);
    size_t off = 0;
    const Bytes out = Take(c);
    const auto msgs = fakerfb::ParseClientMessages(out, off);
    REQUIRE_FALSE(msgs.empty());
    bool full = false;
    for (const auto& m : msgs)
        if (m.type == CFramebufferUpdateRequest && !m.incremental && m.w == 100 && m.h == 80)
            full = true;
    CHECK(full);
}

TEST_CASE("a rectangle outside the framebuffer ends the stream", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    Bytes r = Server::RectHeader(60, 40, 10, 10, EncRaw);
    r.insert(r.end(), 400, 0);
    CHECK_FALSE(Feed(c, r));
    CHECK(c.Failure() == FailureKind::Protocol);
}

TEST_CASE("an unknown message type ends the stream", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    CHECK_FALSE(Feed(c, { 99, 0, 0, 0 }));
    CHECK(c.State() == ClientState::Failed);
}

TEST_CASE("cursor shapes, cut text, bells and colour maps are handled", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    Take(c);
    Bytes cur = Server::RectHeader(1, 1, 2, 1, EncPseudoCursor);
    Wire(cur, 9, 9, 9); Wire(cur, 8, 8, 8);
    cur.push_back(0b10000000);
    REQUIRE(Feed(c, cur));
    CHECK(c.TakeCursorChanged());
    CHECK(c.Cursor().width == 2);
    CHECK(c.Cursor().hotX == 1);
    CHECK(c.Cursor().bgra[0] == P(9, 9, 9));
    CHECK(c.Cursor().bgra[1] == 0x00080808u);

    REQUIRE(Feed(c, Server::CutText("caf\xE9")));
    CHECK(c.TakeCutTexts() == std::vector<std::string>{ "caf\xC3\xA9" });
    REQUIRE(Feed(c, Server::Bell()));
    REQUIRE(Feed(c, Server::Bell()));
    CHECK(c.TakeBells() == 2);
    REQUIRE(Feed(c, Server::ColourMap(3)));   // consumed, ignored
    CHECK(c.State() == ClientState::Ready);
}

TEST_CASE("an unprompted EndOfContinuousUpdates turns continuous updates on", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    CHECK_FALSE(c.ContinuousUpdates());
    REQUIRE(Feed(c, Server::EndOfContinuousUpdates()));
    CHECK(c.ContinuousUpdates());
    size_t off = 0;
    const Bytes out = Take(c);
    const auto msgs = fakerfb::ParseClientMessages(out, off);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].type == CEnableContinuousUpdates);
    CHECK(msgs[0].enable);
    CHECK(msgs[0].w == 64);
    // from here updates arrive unasked: no request follows an update header
    REQUIRE(Feed(c, Server::UpdateHeader(0)));
    CHECK(Take(c).empty());
    CHECK(c.TakeUpdatesCompleted() == 1);
}

TEST_CASE("input and clipboard go out in the protocol's own layouts", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    c.SendKey(true, 'a');
    c.SendKey(false, 'a');
    c.SendPointer(kButtonLeft, 5, 6);
    c.SendCutText("na\xC3\xAFve\r\n");   // ï -> Latin-1, CRLF -> LF
    size_t off = 0;
    const Bytes out = Take(c);
    const auto msgs = fakerfb::ParseClientMessages(out, off);
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].type == CKeyEvent);
    CHECK(msgs[0].down);
    CHECK(msgs[0].keysym == 'a');
    CHECK_FALSE(msgs[1].down);
    CHECK(msgs[2].type == CPointerEvent);
    CHECK(msgs[2].buttons == kButtonLeft);
    CHECK(msgs[2].x == 5);
    CHECK(msgs[3].type == CClientCutText);
    CHECK(msgs[3].text == "na\xEFve\n");
}

TEST_CASE("nothing is sent before the connection is ready", "[vnc][client]")
{
    RfbClient c({});
    c.SendKey(true, 'a');
    c.SendPointer(0, 0, 0);
    c.RequestUpdate(true);
    CHECK(Take(c).empty());
}

// ---- ExtendedDesktopSize ----------------------------------------------------------
namespace
{
// An ExtendedDesktopSize rectangle: header (x = reason, y = status, w x h)
// and one screen covering it.
Bytes ExtendedSizeRect(uint16_t reason, uint16_t status, uint16_t w, uint16_t h, uint32_t screenId)
{
    Bytes v = Server::RectHeader(reason, status, w, h, EncPseudoExtendedDesktopSize);
    v.push_back(1);
    v.insert(v.end(), { 0, 0, 0 });
    fakerfb::U32(v, screenId);
    fakerfb::U16(v, 0);
    fakerfb::U16(v, 0);
    fakerfb::U16(v, w);
    fakerfb::U16(v, h);
    fakerfb::U32(v, 0);
    return v;
}
} // namespace

TEST_CASE("SetDesktopSize is laid out as the extension specifies", "[vnc][protocol]")
{
    Screen s;
    s.id = 0x11223344;
    s.flags = 7;
    const Bytes m = MsgSetDesktopSize(1600, 900, s);
    const Bytes want = { 251, 0, 0x06, 0x40, 0x03, 0x84, 1, 0,
                         0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0x06, 0x40, 0x03, 0x84, 0, 0, 0, 7 };
    REQUIRE(m == want);
}

TEST_CASE("ExtendedDesktopSize is offered ahead of DesktopSize, and the layout is learned", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    const Bytes out = Connect(c, s);
    size_t off = 0;
    const auto msgs = fakerfb::ParseClientMessages(out, off);
    bool ordered = false;
    for (const auto& m : msgs)
        if (m.type == CSetEncodings)
        {
            const auto ext = std::find(m.encodings.begin(), m.encodings.end(), EncPseudoExtendedDesktopSize);
            const auto plain = std::find(m.encodings.begin(), m.encodings.end(), EncPseudoDesktopSize);
            ordered = ext != m.encodings.end() && plain != m.encodings.end() && ext < plain;
        }
    REQUIRE(ordered);
    REQUIRE_FALSE(c.SupportsSetDesktopSize());

    // the server's initial layout: its own reason, the current size
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    REQUIRE(Feed(c, ExtendedSizeRect(ResizeByServer, ResizeOk, s.width, s.height, 42)));
    REQUIRE(c.SupportsSetDesktopSize());
    REQUIRE(c.Screens().size() == 1);
    REQUIRE(c.Screens()[0].id == 42);
    REQUIRE_FALSE(c.TakeResized());   // same size: not a resize
    REQUIRE(c.TakeResizeStatus() == -1);
}

TEST_CASE("a size asked before the layout is known goes out when it is, with the server's screen id", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    c.RequestDesktopSize(800, 600);
    REQUIRE(Take(c).empty());   // held
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    REQUIRE(Feed(c, ExtendedSizeRect(ResizeByServer, ResizeOk, s.width, s.height, 42)));
    const Bytes out = Take(c);
    Screen want;
    want.id = 42;
    const Bytes msg = MsgSetDesktopSize(800, 600, want);
    REQUIRE(std::search(out.begin(), out.end(), msg.begin(), msg.end()) != out.end());

    SECTION("granted: the framebuffer resizes and a full picture is asked for")
    {
        REQUIRE(Feed(c, Server::UpdateHeader(1)));
        REQUIRE(Feed(c, ExtendedSizeRect(ResizeByThisClient, ResizeOk, 800, 600, 42)));
        REQUIRE(c.Fb().width == 800);
        REQUIRE(c.Fb().height == 600);
        REQUIRE(c.TakeResized());
        REQUIRE(c.TakeResizeStatus() == ResizeOk);
        size_t off = 0;
        const auto msgs = fakerfb::ParseClientMessages(Take(c), off);
        bool full = false;
        for (const auto& m : msgs)
            if (m.type == CFramebufferUpdateRequest && !m.incremental && m.w == 800 && m.h == 600)
                full = true;
        REQUIRE(full);
    }
    SECTION("refused: the size stays, the status is reported once")
    {
        REQUIRE(Feed(c, Server::UpdateHeader(1)));
        REQUIRE(Feed(c, ExtendedSizeRect(ResizeByThisClient, ResizeProhibited, s.width, s.height, 42)));
        REQUIRE(c.Fb().width == s.width);
        REQUIRE_FALSE(c.TakeResized());
        REQUIRE(c.TakeResizeStatus() == ResizeProhibited);
        REQUIRE(c.TakeResizeStatus() == -1);
        REQUIRE(std::string(ResizeStatusName(ResizeProhibited)).find("prohibits") != std::string::npos);
    }
    SECTION("the same size again is not asked for")
    {
        REQUIRE(Feed(c, Server::UpdateHeader(1)));
        REQUIRE(Feed(c, ExtendedSizeRect(ResizeByThisClient, ResizeOk, 800, 600, 42)));
        Take(c);
        c.RequestDesktopSize(800, 600);
        REQUIRE(Take(c).empty());
    }
}

TEST_CASE("a screen layout that does not fit its desktop, or is empty, ends the stream", "[vnc][client]")
{
    Server s;
    RfbClient c({});
    Connect(c, s);
    REQUIRE(Feed(c, Server::UpdateHeader(1)));
    Bytes bad = Server::RectHeader(0, 0, 100, 100, EncPseudoExtendedDesktopSize);
    bad.push_back(1);
    bad.insert(bad.end(), { 0, 0, 0 });
    fakerfb::U32(bad, 1);
    fakerfb::U16(bad, 50);
    fakerfb::U16(bad, 0);
    fakerfb::U16(bad, 100);   // 50 + 100 > 100
    fakerfb::U16(bad, 100);
    fakerfb::U32(bad, 0);
    REQUIRE_FALSE(Feed(c, bad));
    REQUIRE(c.State() == ClientState::Failed);
}
