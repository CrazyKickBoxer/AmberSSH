// XAuthTests — X11 forwarding authentication.
//
// The property under test is one sentence: a remote host must never learn the
// cookie that opens this display, and a connection that fails the check must
// be closed rather than forwarded. Everything below is that sentence taken
// apart.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "remote/XAuth.h"

using namespace amber;

namespace
{

std::vector<uint8_t> Bytes(std::initializer_list<int> v)
{
    std::vector<uint8_t> out;
    for (int b : v)
        out.push_back(static_cast<uint8_t>(b));
    return out;
}

std::vector<uint8_t> Cookie(uint8_t fill)
{
    return std::vector<uint8_t>(kCookieBytes, fill);
}

void PushU16BE(std::vector<uint8_t>& v, uint16_t n)
{
    v.push_back(static_cast<uint8_t>(n >> 8));
    v.push_back(static_cast<uint8_t>(n & 0xFF));
}

void PushBlockBE(std::vector<uint8_t>& v, const std::string& s)
{
    PushU16BE(v, static_cast<uint16_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

// One .Xauthority record.
void PushAuthEntry(std::vector<uint8_t>& v, uint16_t family,
                   const std::string& addr, const std::string& number,
                   const std::string& name, const std::vector<uint8_t>& data)
{
    PushU16BE(v, family);
    PushBlockBE(v, addr);
    PushBlockBE(v, number);
    PushBlockBE(v, name);
    PushU16BE(v, static_cast<uint16_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
}

// An X11 connection setup packet, little-endian, with the given auth data.
std::vector<uint8_t> Setup(const std::string& proto,
                           const std::vector<uint8_t>& cookie,
                           bool bigEndian = false)
{
    std::vector<uint8_t> v;
    v.push_back(bigEndian ? 'B' : 'l');
    v.push_back(0);
    auto put16 = [&](uint16_t n)
    {
        if (bigEndian)
        {
            v.push_back(static_cast<uint8_t>(n >> 8));
            v.push_back(static_cast<uint8_t>(n & 0xFF));
        }
        else
        {
            v.push_back(static_cast<uint8_t>(n & 0xFF));
            v.push_back(static_cast<uint8_t>(n >> 8));
        }
    };
    put16(11);                                    // protocol major
    put16(0);                                     // protocol minor
    put16(static_cast<uint16_t>(proto.size()));   // auth name length
    put16(static_cast<uint16_t>(cookie.size()));  // auth data length
    put16(0);                                     // unused
    v.insert(v.end(), proto.begin(), proto.end());
    while (v.size() % 4)
        v.push_back(0);
    v.insert(v.end(), cookie.begin(), cookie.end());
    while (v.size() % 4)
        v.push_back(0);
    return v;
}

} // namespace

TEST_CASE("A display string is parsed, and a silly one is refused", "[xauth]")
{
    XDisplay d = ParseDisplay("localhost:0");
    CHECK(d.valid);
    CHECK(d.host == "127.0.0.1");
    CHECK(d.display == 0);
    CHECK(d.Port() == 6000);

    d = ParseDisplay(":1.0");
    CHECK(d.valid);
    CHECK(d.display == 1);
    CHECK(d.screen == 0);
    CHECK(d.Port() == 6001);

    d = ParseDisplay("192.168.1.5:12");
    CHECK(d.valid);
    CHECK(d.host == "192.168.1.5");
    CHECK(d.Port() == 6012);

    CHECK(ParseDisplay("unix:0").host == "127.0.0.1");

    // A display number is a small integer. A huge one is a mistake or an
    // attempt to steer the connection at some other service's port, and
    // clamping it to a valid display would do exactly what was intended.
    CHECK_FALSE(ParseDisplay("localhost:60000").valid);
    CHECK_FALSE(ParseDisplay("localhost:-1").valid);
    CHECK_FALSE(ParseDisplay("localhost:").valid);
    CHECK_FALSE(ParseDisplay("localhost:0x10").valid);
    CHECK_FALSE(ParseDisplay("nocolon").valid);
    CHECK_FALSE(ParseDisplay("").valid);
}

TEST_CASE("Cookies are random, 16 bytes, and never repeat", "[xauth]")
{
    const std::string a = MakeCookieHex();
    REQUIRE(a.size() == kCookieBytes * 2);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    // A predictable cookie is no cookie at all. Sampled rather than argued.
    std::vector<std::string> seen;
    for (int i = 0; i < 200; ++i)
    {
        const std::string c = MakeCookieHex();
        REQUIRE(c.size() == kCookieBytes * 2);
        for (const std::string& p : seen)
            REQUIRE(c != p);
        seen.push_back(c);
    }
}

TEST_CASE("Hex conversion is all-or-nothing", "[xauth]")
{
    const std::vector<uint8_t> b = Bytes({ 0x00, 0x0F, 0xA5, 0xFF });
    CHECK(BytesToHex(b) == "000fa5ff");
    CHECK(HexToBytes("000fa5ff") == b);
    CHECK(HexToBytes("000FA5FF") == b);
    // A malformed entry must not yield a partial cookie that then gets
    // compared against something.
    CHECK(HexToBytes("abc").empty());
    CHECK(HexToBytes("zz").empty());
    CHECK(HexToBytes("00zz").empty());
    CHECK(HexToBytes("").empty());
}

TEST_CASE("Cookie comparison rejects the empty cookie", "[xauth]")
{
    CHECK(CookieEqual(Cookie(0xAB), Cookie(0xAB)));
    CHECK_FALSE(CookieEqual(Cookie(0xAB), Cookie(0xAC)));
    CHECK_FALSE(CookieEqual(Cookie(0xAB), std::vector<uint8_t>(8, 0xAB)));
    // Two absent cookies are not a match. Without this, a missing cookie on
    // both sides would authenticate.
    CHECK_FALSE(CookieEqual({}, {}));
}

TEST_CASE("An .Xauthority file is parsed", "[xauth]")
{
    std::vector<uint8_t> f;
    PushAuthEntry(f, 256, "localhost", "0", kMitMagicCookie, Cookie(0x11));
    PushAuthEntry(f, 0, "192.168.1.5", "1", kMitMagicCookie, Cookie(0x22));
    PushAuthEntry(f, 256, "localhost", "0", "XDM-AUTHORIZATION-1", Cookie(0x33));

    const std::vector<XAuthEntry> e = ParseXAuthority(f);
    REQUIRE(e.size() == 3);
    CHECK(e[0].number == "0");
    CHECK(e[0].name == kMitMagicCookie);
    CHECK(e[0].data == Cookie(0x11));

    CHECK(CookieForDisplay(e, ParseDisplay(":0")) == Cookie(0x11));
    CHECK(CookieForDisplay(e, ParseDisplay(":1")) == Cookie(0x22));
    // Display 0's cookie must not open display 2.
    CHECK(CookieForDisplay(e, ParseDisplay(":2")).empty());
    // Only MIT-MAGIC-COOKIE-1; the XDM entry on display 0 is not picked up as
    // a fallback.
    CHECK(CookieForDisplay(e, ParseDisplay(":0")) != Cookie(0x33));
    CHECK(CookieForDisplay(e, ParseDisplay("bogus")).empty());
}

TEST_CASE("A truncated .Xauthority keeps what it could read", "[xauth]")
{
    // Half-written files are normal — an X server starting up writes one — and
    // must not take the session down.
    std::vector<uint8_t> f;
    PushAuthEntry(f, 256, "localhost", "0", kMitMagicCookie, Cookie(0x11));
    PushAuthEntry(f, 256, "localhost", "1", kMitMagicCookie, Cookie(0x22));
    for (size_t cut = f.size(); cut-- > 0;)
    {
        std::vector<uint8_t> part(f.begin(), f.begin() + static_cast<ptrdiff_t>(cut));
        const std::vector<XAuthEntry> e = ParseXAuthority(part);
        CHECK(e.size() <= 2);
        for (const XAuthEntry& x : e)
            CHECK(x.data.size() <= kCookieBytes);
    }
    CHECK(ParseXAuthority({}).empty());
}

TEST_CASE("A cookie of the wrong length is not offered", "[xauth]")
{
    std::vector<uint8_t> f;
    PushAuthEntry(f, 256, "localhost", "0", kMitMagicCookie,
                  std::vector<uint8_t>(8, 0x55));
    CHECK(CookieForDisplay(ParseXAuthority(f), ParseDisplay(":0")).empty());
}

TEST_CASE("The setup header is read in both byte orders", "[xauth]")
{
    for (bool be : { false, true })
    {
        const std::vector<uint8_t> p = Setup(kMitMagicCookie, Cookie(0x42), be);
        const XSetupHeader h = ParseSetupHeader(p.data(), p.size());
        INFO(std::string(be ? "big endian" : "little endian"));
        REQUIRE(h.valid);
        CHECK(h.bigEndian == be);
        CHECK(h.protoMajor == 11);
        CHECK(h.nameLen == 18);
        CHECK(h.dataLen == kCookieBytes);
        CHECK(h.total == p.size());
    }
    // Too short to decide is not the same as invalid input.
    const std::vector<uint8_t> p = Setup(kMitMagicCookie, Cookie(0x42));
    for (size_t n = 0; n < 12; ++n)
        CHECK_FALSE(ParseSetupHeader(p.data(), n).valid);
    // Neither 'B' nor 'l' is not an X11 client.
    const uint8_t junk[12] = { 'X' };
    CHECK_FALSE(ParseSetupHeader(junk, sizeof(junk)).valid);
}

TEST_CASE("The real cookie replaces the fake one", "[xauth]")
{
    const std::vector<uint8_t> fake = Cookie(0xAA), real = Cookie(0xBB);
    const std::vector<uint8_t> in = Setup(kMitMagicCookie, fake);
    std::vector<uint8_t> out;
    REQUIRE(RewriteSetup(in, fake, real, out) == XAuthVerdict::Rewritten);

    // Same size, same everything except the sixteen bytes that matter.
    REQUIRE(out.size() == in.size());
    const XSetupHeader h = ParseSetupHeader(out.data(), out.size());
    REQUIRE(h.valid);
    const size_t dataAt = 12 + ((h.nameLen + 3) & ~size_t(3));
    const std::vector<uint8_t> got(out.begin() + static_cast<ptrdiff_t>(dataAt),
                                   out.begin() + static_cast<ptrdiff_t>(dataAt + kCookieBytes));
    CHECK(got == real);
    // And the fake is gone: the display never sees what the remote host held.
    CHECK(std::search(out.begin(), out.end(), fake.begin(), fake.end()) == out.end());
}

TEST_CASE("A wrong cookie closes the channel", "[xauth]")
{
    const std::vector<uint8_t> fake = Cookie(0xAA), real = Cookie(0xBB);
    std::vector<uint8_t> out;

    // The credential the remote host actually presented is not ours.
    CHECK(RewriteSetup(Setup(kMitMagicCookie, Cookie(0xCC)), fake, real, out) ==
          XAuthVerdict::Rejected);
    CHECK(out.empty());

    // A cookie of the wrong length is not a near miss, it is a rejection.
    CHECK(RewriteSetup(Setup(kMitMagicCookie, std::vector<uint8_t>(8, 0xAA)),
                       fake, real, out) == XAuthVerdict::Rejected);

    // An authentication protocol we do not implement is refused rather than
    // passed through for the X server to puzzle over.
    CHECK(RewriteSetup(Setup("XDM-AUTHORIZATION-1", Cookie(0xAA)), fake, real,
                       out) == XAuthVerdict::Rejected);

    // No auth at all is the case this whole module exists to stop.
    CHECK(RewriteSetup(Setup("", {}), fake, real, out) == XAuthVerdict::Rejected);

    // Garbage that is not an X11 client.
    std::vector<uint8_t> junk(64, 'X');
    CHECK(RewriteSetup(junk, fake, real, out) == XAuthVerdict::Rejected);
}

TEST_CASE("A partial setup packet asks for more, byte by byte", "[xauth]")
{
    const std::vector<uint8_t> fake = Cookie(0xAA), real = Cookie(0xBB);
    const std::vector<uint8_t> full = Setup(kMitMagicCookie, fake);
    std::vector<uint8_t> out;
    // Every prefix short of the whole packet must buffer rather than decide.
    // A parser that guessed here would authenticate on incomplete input.
    for (size_t n = 0; n < full.size(); ++n)
    {
        std::vector<uint8_t> part(full.begin(), full.begin() + static_cast<ptrdiff_t>(n));
        INFO(n);
        CHECK(RewriteSetup(part, fake, real, out) == XAuthVerdict::NeedMore);
    }
    CHECK(RewriteSetup(full, fake, real, out) == XAuthVerdict::Rewritten);
}

TEST_CASE("Trailing request bytes ride along with the setup packet", "[xauth]")
{
    // X clients pipeline: the first read often carries the setup packet AND
    // the first requests. Those must be forwarded, not dropped.
    const std::vector<uint8_t> fake = Cookie(0xAA), real = Cookie(0xBB);
    std::vector<uint8_t> in = Setup(kMitMagicCookie, fake);
    const size_t setupLen = in.size();
    for (int i = 0; i < 40; ++i)
        in.push_back(static_cast<uint8_t>(0x80 + i));
    std::vector<uint8_t> out;
    REQUIRE(RewriteSetup(in, fake, real, out) == XAuthVerdict::Rewritten);
    REQUIRE(out.size() == in.size());
    for (size_t i = setupLen; i < in.size(); ++i)
        CHECK(out[i] == in[i]);
}

TEST_CASE("With no local cookie the packet passes through, still checked", "[xauth]")
{
    // Windows X servers are often started with access control off and no
    // .Xauthority at all. Refusing outright would break them; forwarding
    // blindly would defeat the point. So the fake is still verified and the
    // packet goes on unchanged for the display's own rules to judge.
    const std::vector<uint8_t> fake = Cookie(0xAA);
    const std::vector<uint8_t> in = Setup(kMitMagicCookie, fake);
    std::vector<uint8_t> out;
    CHECK(RewriteSetup(in, fake, {}, out) == XAuthVerdict::Rewritten);
    CHECK(out == in);
    // A wrong cookie is still a rejection in this mode.
    CHECK(RewriteSetup(Setup(kMitMagicCookie, Cookie(0xCC)), fake, {}, out) ==
          XAuthVerdict::Rejected);
}

TEST_CASE("Without a fake cookie nothing authenticates", "[xauth]")
{
    // If cookie generation failed, every connection must be refused. The
    // dangerous reading of "no cookie" is "no check needed".
    std::vector<uint8_t> out;
    CHECK(RewriteSetup(Setup(kMitMagicCookie, Cookie(0xAA)), {}, Cookie(0xBB),
                       out) == XAuthVerdict::Rejected);
    CHECK(RewriteSetup(Setup("", {}), {}, {}, out) == XAuthVerdict::Rejected);
}
