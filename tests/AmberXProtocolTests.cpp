// AmberXProtocolTests — the AmberXControl framing.
//
// This parser sits between AmberSSH and a process handling X11 bytes that a
// remote host originated. So the properties under test are mostly refusals:
// what it declines to allocate, what it declines to believe, and what it
// declines to guess at.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "amberx/control/Protocol.h"

using namespace amber::amberx;

namespace
{

std::vector<uint8_t> Nonce(uint8_t fill)
{
    return std::vector<uint8_t>(kNonceBytes, fill);
}

std::vector<uint8_t> Cookie(uint8_t fill)
{
    return std::vector<uint8_t>(16, fill);
}

Frame MakeFrame(MsgType t, uint32_t chan, size_t payloadLen)
{
    Frame f;
    f.type = t;
    f.channel = chan;
    f.payload.assign(payloadLen, 0xAB);
    return f;
}

// Writes a header by hand so a test can produce bytes Encode() would refuse.
std::vector<uint8_t> RawHeader(uint32_t magic, uint16_t ver, uint16_t type,
                               uint32_t chan, uint32_t len)
{
    std::vector<uint8_t> v;
    auto u32 = [&](uint32_t n) {
        v.push_back(uint8_t(n)); v.push_back(uint8_t(n >> 8));
        v.push_back(uint8_t(n >> 16)); v.push_back(uint8_t(n >> 24));
    };
    auto u16 = [&](uint16_t n) {
        v.push_back(uint8_t(n)); v.push_back(uint8_t(n >> 8));
    };
    u32(magic); u16(ver); u16(type); u32(chan); u32(len);
    return v;
}

} // namespace

TEST_CASE("A frame round-trips exactly", "[amberx]")
{
    const Frame in = MakeFrame(MsgType::ChannelData, 7, 300);
    std::vector<uint8_t> wire;
    REQUIRE(Encode(in, wire));
    CHECK(wire.size() == kHeaderBytes + 300);

    Frame out;
    size_t consumed = 0;
    REQUIRE(Decode(wire, out, consumed) == Decoded::Ok);
    CHECK(consumed == wire.size());
    CHECK(out.type == MsgType::ChannelData);
    CHECK(out.channel == 7);
    CHECK(out.version == kVersion);
    CHECK(out.payload == in.payload);
}

TEST_CASE("An empty payload is a legal frame", "[amberx]")
{
    std::vector<uint8_t> wire;
    REQUIRE(Encode(MakeFrame(MsgType::Shutdown, kControlChannel, 0), wire));
    CHECK(wire.size() == kHeaderBytes);
    Frame out;
    size_t consumed = 0;
    CHECK(Decode(wire, out, consumed) == Decoded::Ok);
    CHECK(out.payload.empty());
}

TEST_CASE("Every prefix of a frame asks for more, never decides", "[amberx]")
{
    std::vector<uint8_t> wire;
    REQUIRE(Encode(MakeFrame(MsgType::ChannelData, 3, 64), wire));
    Frame out;
    size_t consumed = 0;
    // A parser that guessed on incomplete input would act on a length or a
    // type it had not finished reading.
    for (size_t n = 0; n < wire.size(); ++n)
    {
        const std::vector<uint8_t> part(wire.begin(),
                                        wire.begin() + static_cast<ptrdiff_t>(n));
        INFO(n);
        CHECK(Decode(part, out, consumed) == Decoded::NeedMore);
        CHECK(consumed == 0);
    }
    CHECK(Decode(wire, out, consumed) == Decoded::Ok);
}

TEST_CASE("An oversized length is refused before anything is allocated", "[amberx]")
{
    // The one that matters most. A frame claiming 4 GiB must cost nothing to
    // reject — the header alone is enough to know it is a lie.
    for (uint32_t len : { kMaxPayload + 1, 0x1000000u, 0x7FFFFFFFu, 0xFFFFFFFFu })
    {
        const std::vector<uint8_t> h =
            RawHeader(kMagic, kVersion, uint16_t(MsgType::ChannelData), 1, len);
        Frame out;
        size_t consumed = 0;
        INFO(len);
        CHECK(Decode(h, out, consumed) == Decoded::Bad);
        CHECK(consumed == 0);
    }
    // Exactly at the cap is still legal.
    const std::vector<uint8_t> ok =
        RawHeader(kMagic, kVersion, uint16_t(MsgType::ChannelData), 1, kMaxPayload);
    Frame out;
    size_t consumed = 0;
    CHECK(Decode(ok, out, consumed) == Decoded::NeedMore);   // header only so far
}

TEST_CASE("A wrong magic or version is fatal, not resynchronised", "[amberx]")
{
    Frame out;
    size_t consumed = 0;
    CHECK(Decode(RawHeader(0xDEADBEEF, kVersion, 1, 0, 0), out, consumed) ==
          Decoded::Bad);
    CHECK(Decode(RawHeader(kMagic, kVersion + 1, 1, 0, 0), out, consumed) ==
          Decoded::Bad);
    CHECK(Decode(RawHeader(kMagic, 0, 1, 0, 0), out, consumed) == Decoded::Bad);
}

TEST_CASE("An unknown message type is refused, not skipped", "[amberx]")
{
    // Ignoring a message you do not understand is how two versions of a
    // protocol quietly disagree about state.
    for (uint16_t t : { uint16_t(0), uint16_t(9), uint16_t(64), uint16_t(0xFFFF) })
    {
        Frame out;
        size_t consumed = 0;
        INFO(t);
        CHECK(Decode(RawHeader(kMagic, kVersion, t, 0, 0), out, consumed) ==
              Decoded::Bad);
    }
    CHECK(KnownType(uint16_t(MsgType::SetCookie)));
    CHECK_FALSE(KnownType(9));
}

TEST_CASE("A type cannot ride the wrong kind of channel", "[amberx]")
{
    Frame out;
    size_t consumed = 0;
    // A cookie install addressed to a data channel would let a stream of X11
    // bytes impersonate a control message.
    CHECK(Decode(RawHeader(kMagic, kVersion, uint16_t(MsgType::SetCookie), 5, 0),
                 out, consumed) == Decoded::Bad);
    // And X11 bytes addressed to the control channel are equally wrong.
    CHECK(Decode(RawHeader(kMagic, kVersion, uint16_t(MsgType::ChannelData), 0, 0),
                 out, consumed) == Decoded::Bad);

    // Encode refuses to emit either, so this side never produces one.
    std::vector<uint8_t> wire;
    CHECK_FALSE(Encode(MakeFrame(MsgType::SetCookie, 5, 20), wire));
    CHECK_FALSE(Encode(MakeFrame(MsgType::ChannelData, kControlChannel, 4), wire));
    CHECK(Encode(MakeFrame(MsgType::SetCookie, kControlChannel, 20), wire));
    CHECK(Encode(MakeFrame(MsgType::ChannelData, 5, 4), wire));
}

TEST_CASE("Encode refuses an oversized payload", "[amberx]")
{
    Frame f = MakeFrame(MsgType::ChannelData, 1, 0);
    f.payload.assign(kMaxPayload + 1, 0);
    std::vector<uint8_t> wire;
    // A frame the far end could only reject is never put on the wire.
    CHECK_FALSE(Encode(f, wire));
}

TEST_CASE("Frames decode back to back from one buffer", "[amberx]")
{
    std::vector<uint8_t> stream, one;
    REQUIRE(Encode(MakeFrame(MsgType::ChannelOpen, 1, 0), one));
    stream.insert(stream.end(), one.begin(), one.end());
    REQUIRE(Encode(MakeFrame(MsgType::ChannelData, 1, 10), one));
    stream.insert(stream.end(), one.begin(), one.end());
    REQUIRE(Encode(MakeFrame(MsgType::ChannelClose, 1, 0), one));
    stream.insert(stream.end(), one.begin(), one.end());

    std::vector<MsgType> got;
    size_t at = 0;
    for (;;)
    {
        const std::vector<uint8_t> rest(stream.begin() + static_cast<ptrdiff_t>(at),
                                        stream.end());
        Frame f;
        size_t consumed = 0;
        const Decoded d = Decode(rest, f, consumed);
        if (d != Decoded::Ok)
            break;
        got.push_back(f.type);
        at += consumed;
    }
    REQUIRE(got.size() == 3);
    CHECK(got[0] == MsgType::ChannelOpen);
    CHECK(got[2] == MsgType::ChannelClose);
    CHECK(at == stream.size());
}

TEST_CASE("Nonce comparison is constant time and rejects the empty", "[amberx]")
{
    CHECK(ConstantTimeEqual(Nonce(0x11), Nonce(0x11)));
    CHECK_FALSE(ConstantTimeEqual(Nonce(0x11), Nonce(0x12)));
    CHECK_FALSE(ConstantTimeEqual(Nonce(0x11), std::vector<uint8_t>(8, 0x11)));
    // Two absent nonces must not authenticate each other.
    CHECK_FALSE(ConstantTimeEqual({}, {}));
}

TEST_CASE("Handshake payloads are exact length or nothing", "[amberx]")
{
    const std::vector<uint8_t> n = Nonce(0x42);
    const std::vector<uint8_t> hello = MakeHello(n);
    REQUIRE(hello.size() == kNonceBytes);
    std::vector<uint8_t> back;
    CHECK(ParseHello(hello, back));
    CHECK(back == n);

    // Short, long and empty are all refused rather than read as far as
    // possible — a partially-read nonce is not a nonce.
    CHECK_FALSE(ParseHello(std::vector<uint8_t>(kNonceBytes - 1, 0), back));
    CHECK_FALSE(ParseHello(std::vector<uint8_t>(kNonceBytes + 1, 0), back));
    CHECK_FALSE(ParseHello({}, back));
    // And a nonce of the wrong size is never built.
    CHECK(MakeHello(std::vector<uint8_t>(8, 0)).empty());
}

TEST_CASE("HelloAck carries both nonces and the proof, and splits them back", "[amberx]")
{
    const std::vector<uint8_t> echo = Nonce(0xAA), own = Nonce(0xBB);
    const std::vector<uint8_t> proof(kProofBytes, 0xCC);
    const std::vector<uint8_t> p = MakeHelloAck(echo, own, proof);
    REQUIRE(p.size() == kNonceBytes * 2 + kProofBytes);
    std::vector<uint8_t> gotEcho, gotOwn, gotProof;
    REQUIRE(ParseHelloAck(p, gotEcho, gotOwn, gotProof));
    CHECK(gotEcho == echo);
    CHECK(gotOwn == own);
    CHECK(gotProof == proof);
    // The old two-nonce layout, one byte short, one byte long: all refused.
    CHECK_FALSE(ParseHelloAck(std::vector<uint8_t>(kNonceBytes * 2, 0), gotEcho, gotOwn, gotProof));
    CHECK_FALSE(ParseHelloAck(std::vector<uint8_t>(p.size() - 1, 0), gotEcho, gotOwn, gotProof));
    CHECK_FALSE(ParseHelloAck(std::vector<uint8_t>(p.size() + 1, 0), gotEcho, gotOwn, gotProof));
    CHECK(MakeHelloAck(echo, std::vector<uint8_t>(4, 0), proof).empty());
    CHECK(MakeHelloAck(echo, own, std::vector<uint8_t>(16, 0)).empty());

    std::vector<uint8_t> ap;
    CHECK(MakeAuthProof(proof).size() == kProofBytes);
    CHECK(MakeAuthProof(std::vector<uint8_t>(31, 0)).empty());
    CHECK(ParseAuthProof(proof, ap));
    CHECK(ap == proof);
    CHECK_FALSE(ParseAuthProof(std::vector<uint8_t>(33, 0), ap));
}

TEST_CASE("A cookie install is 16 bytes and a display number", "[amberx]")
{
    const std::vector<uint8_t> c = Cookie(0x5A);
    const std::vector<uint8_t> p = MakeSetCookie(c, 0);
    REQUIRE(p.size() == 20);
    std::vector<uint8_t> gotCookie;
    uint32_t display = 99;
    REQUIRE(ParseSetCookie(p, gotCookie, display));
    CHECK(display == 0);
    CHECK(gotCookie == c);

    // 16 bytes is the only MIT-MAGIC-COOKIE-1 size AmberSSH deals in.
    CHECK(MakeSetCookie(std::vector<uint8_t>(8, 0), 0).empty());
    CHECK(MakeSetCookie({}, 0).empty());
    CHECK_FALSE(ParseSetCookie(std::vector<uint8_t>(19, 0), gotCookie, display));
    CHECK_FALSE(ParseSetCookie(std::vector<uint8_t>(21, 0), gotCookie, display));
}

TEST_CASE("A channel must be opened before it can be addressed", "[amberx]")
{
    ChannelTable t;
    CHECK_FALSE(t.IsOpen(1));
    CHECK(t.Open(1));
    CHECK(t.IsOpen(1));
    CHECK(t.Count() == 1);

    // Reopening is a protocol error, not a no-op: it would silently reset
    // whatever state the first open established.
    CHECK_FALSE(t.Open(1));
    // Channel 0 is control and is never a data channel.
    CHECK_FALSE(t.Open(kControlChannel));

    CHECK(t.Close(1));
    CHECK_FALSE(t.IsOpen(1));
    // Closing twice is equally an error — the second close names a channel
    // this side does not own.
    CHECK_FALSE(t.Close(1));
    CHECK_FALSE(t.Close(999));
}

TEST_CASE("The channel cap is enforced", "[amberx]")
{
    ChannelTable t;
    for (uint32_t i = 1; i <= ChannelTable::kMaxChannels; ++i)
        REQUIRE(t.Open(i));
    CHECK(t.Count() == ChannelTable::kMaxChannels);
    // A remote host that opens connections in a loop hits a wall rather than
    // growing this table until something else fails.
    CHECK_FALSE(t.Open(ChannelTable::kMaxChannels + 1));
    t.Clear();
    CHECK(t.Count() == 0);
    CHECK(t.Open(1));
}

// ---------------------------------------------------------- the host report
// The report is the one message with a variable-length list in it, which
// makes it the one message a length bug could be hiding in. Every test here
// is about what the parser refuses, not what it accepts.

TEST_CASE("A host report survives a round trip with its window list", "[amberx]")
{
    HostReport r;
    r.clients = 3;
    r.windows = 11;
    r.pixmapBytes = 5ull * 1024 * 1024;
    r.x11In = 0x1'0000'0007ull;          // over 32 bits, to catch a truncation
    r.x11Out = 4242;
    r.presents = 900;
    r.dirtyRects = 1800;
    r.ipcHighWater = 65536;
    r.rejected = 2;
    r.windowList.push_back({ 0x200001u, 4u, "xterm" });
    r.windowList.push_back({ 0x400002u, 1u, "" });

    HostReport back;
    REQUIRE(ParseHostReport(MakeHostReport(r), back));
    CHECK(back.clients == 3);
    CHECK(back.windows == 11);
    CHECK(back.pixmapBytes == r.pixmapBytes);
    CHECK(back.x11In == r.x11In);
    CHECK(back.x11Out == 4242);
    CHECK(back.presents == 900);
    CHECK(back.dirtyRects == 1800);
    CHECK(back.ipcHighWater == 65536);
    CHECK(back.rejected == 2);
    REQUIRE(back.windowList.size() == 2);
    CHECK(back.windowList[0].xid == 0x200001u);
    CHECK(back.windowList[0].flags == 4u);
    CHECK(back.windowList[0].title == "xterm");
    CHECK(back.windowList[1].title.empty());
}

TEST_CASE("A host report refuses everything that does not add up", "[amberx]")
{
    HostReport r;
    r.windowList.push_back({ 1u, 0u, "one" });
    std::vector<uint8_t> good = MakeHostReport(r);
    HostReport out;

    // Truncated anywhere: the head, the row header, the title.
    for (size_t cut = 1; cut < good.size(); ++cut)
    {
        std::vector<uint8_t> shorter(good.begin(), good.begin() + static_cast<ptrdiff_t>(cut));
        CHECK_FALSE(ParseHostReport(shorter, out));
    }
    // Trailing rubbish is refused too: a message that parses and has bytes
    // left over is not the message that was sent.
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    CHECK_FALSE(ParseHostReport(longer, out));

    // A layout version this build does not know.
    std::vector<uint8_t> future = good;
    future[0] = 9;
    CHECK_FALSE(ParseHostReport(future, out));

    // A window count larger than the cap, with no rows behind it: the count
    // must be refused before anything is reserved for it.
    std::vector<uint8_t> liar = good;
    liar[52] = 0xff; liar[53] = 0xff; liar[54] = 0xff; liar[55] = 0xff;
    CHECK_FALSE(ParseHostReport(liar, out));
}

TEST_CASE("A host report bounds what it will carry", "[amberx]")
{
    HostReport r;
    for (size_t i = 0; i < kMaxReportWindows + 10; ++i)
        r.windowList.push_back({ static_cast<uint32_t>(i + 1), 0u, "w" });
    r.windowList[0].title = std::string(kMaxWindowTitle + 50, 'x');

    HostReport back;
    REQUIRE(ParseHostReport(MakeHostReport(r), back));
    CHECK(back.windowList.size() == kMaxReportWindows);
    CHECK(back.windowList[0].title.size() == kMaxWindowTitle);
}

TEST_CASE("A window action is a window and one of three verbs", "[amberx]")
{
    uint32_t xid = 0;
    WindowAct act = WindowAct::Show;
    REQUIRE(ParseWindowAction(MakeWindowAction(0x123u, WindowAct::Close), xid, act));
    CHECK(xid == 0x123u);
    CHECK(act == WindowAct::Close);

    // A verb this build does not have is refused rather than clamped: an
    // action the far end meant and this end guessed at is worse than none.
    std::vector<uint8_t> bad = MakeWindowAction(1u, WindowAct::Show);
    bad[4] = 7;
    CHECK_FALSE(ParseWindowAction(bad, xid, act));
    CHECK_FALSE(ParseWindowAction({}, xid, act));
    std::vector<uint8_t> shortAct = MakeWindowAction(1u, WindowAct::Show);
    shortAct.pop_back();
    CHECK_FALSE(ParseWindowAction(shortAct, xid, act));
}
