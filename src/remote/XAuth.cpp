#include "XAuth.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#pragma comment(lib, "bcrypt.lib")

namespace amber
{

namespace
{

// Setup packets are padded to 4-byte boundaries.
size_t Pad4(size_t n) { return (n + 3) & ~size_t(3); }

int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

XDisplay ParseDisplay(const std::string& s)
{
    XDisplay d;
    if (s.empty())
        return d;
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos)
        return d;                       // a display string always has a colon

    d.host = s.substr(0, colon);
    if (d.host.empty() || d.host == "localhost" || d.host == "unix")
        d.host = "127.0.0.1";

    // ":0.1" — the screen suffix is optional and rarely used.
    std::string rest = s.substr(colon + 1);
    const size_t dot = rest.find('.');
    std::string num = dot == std::string::npos ? rest : rest.substr(0, dot);
    std::string scr = dot == std::string::npos ? std::string() : rest.substr(dot + 1);
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos)
        return d;
    if (!scr.empty() && scr.find_first_not_of("0123456789") != std::string::npos)
        return d;

    // A display number is a small integer. Rejecting rather than clamping: a
    // display of 60000 is a mistake or an attempt to steer the connection at
    // some other service's port, and neither should quietly become display 0.
    const long n = std::strtol(num.c_str(), nullptr, 10);
    if (n < 0 || n > 1023)
        return d;
    d.display = static_cast<int>(n);
    d.screen = scr.empty() ? 0 : static_cast<int>(std::strtol(scr.c_str(), nullptr, 10));
    d.valid = true;
    return d;
}

std::string MakeCookieHex()
{
    uint8_t buf[kCookieBytes];
    // The OS CSPRNG, not rand(). A predictable cookie is no cookie.
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    return BytesToHex(std::vector<uint8_t>(buf, buf + sizeof(buf)));
}

std::string BytesToHex(const std::vector<uint8_t>& b)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (uint8_t c : b)
    {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> HexToBytes(const std::string& hex)
{
    if (hex.size() % 2 != 0)
        return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = HexVal(hex[i]), lo = HexVal(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};                  // all or nothing: no partial cookie
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

bool CookieEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    // Length is not secret; the contents are. Comparing every byte regardless
    // of an early mismatch keeps the time independent of where it differs.
    if (a.size() != b.size() || a.empty())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

// ------------------------------------------------------------- .Xauthority
std::vector<XAuthEntry> ParseXAuthority(const std::vector<uint8_t>& f)
{
    std::vector<XAuthEntry> out;
    size_t at = 0;
    auto u16 = [&](uint16_t& v) -> bool
    {
        if (at + 2 > f.size())
            return false;
        // The format is big-endian regardless of the host.
        v = static_cast<uint16_t>((f[at] << 8) | f[at + 1]);
        at += 2;
        return true;
    };
    auto block = [&](std::string& s) -> bool
    {
        uint16_t n = 0;
        if (!u16(n) || at + n > f.size())
            return false;
        s.assign(reinterpret_cast<const char*>(f.data() + at), n);
        at += n;
        return true;
    };
    while (at < f.size())
    {
        XAuthEntry e;
        uint16_t dataLen = 0;
        std::string data;
        // A truncated tail is normal — an X server part-way through writing
        // the file — and must not lose the entries already read.
        if (!u16(e.family) || !block(e.address) || !block(e.number) ||
            !block(e.name) || !block(data))
            break;
        e.data.assign(data.begin(), data.end());
        (void)dataLen;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<uint8_t> CookieForDisplay(const std::vector<XAuthEntry>& entries,
                                      const XDisplay& d)
{
    if (!d.valid)
        return {};
    const std::string want = std::to_string(d.display);
    for (const XAuthEntry& e : entries)
    {
        if (e.name != kMitMagicCookie)
            continue;
        // Strict on the display number: display 0's cookie must not open
        // display 1. Loose on the address, because X servers variously write
        // the host name, "localhost", or nothing at all for the same display.
        if (e.number != want)
            continue;
        if (e.data.size() != kCookieBytes)
            continue;
        return e.data;
    }
    return {};
}

// --------------------------------------------------- the X11 setup packet
XSetupHeader ParseSetupHeader(const uint8_t* data, size_t len)
{
    XSetupHeader h;
    if (len < 12)
        return h;                       // not enough to decide yet
    const uint8_t order = data[0];
    if (order != 'B' && order != 'l')
        return h;                       // not an X11 client
    h.bigEndian = order == 'B';
    auto rd16 = [&](size_t off) -> uint16_t
    {
        return h.bigEndian
                   ? static_cast<uint16_t>((data[off] << 8) | data[off + 1])
                   : static_cast<uint16_t>((data[off + 1] << 8) | data[off]);
    };
    h.protoMajor = rd16(2);
    h.protoMinor = rd16(4);
    h.nameLen = rd16(6);
    h.dataLen = rd16(8);
    h.total = 12 + Pad4(h.nameLen) + Pad4(h.dataLen);
    h.valid = true;
    return h;
}

XAuthVerdict RewriteSetup(const std::vector<uint8_t>& in,
                          const std::vector<uint8_t>& expectFake,
                          const std::vector<uint8_t>& realCookie,
                          std::vector<uint8_t>& out)
{
    out.clear();
    const XSetupHeader h = ParseSetupHeader(in.data(), in.size());
    if (!h.valid)
    {
        // Too short to read is "wait"; wrong byte order is "this is not X11".
        return in.size() < 12 ? XAuthVerdict::NeedMore : XAuthVerdict::Rejected;
    }
    if (in.size() < h.total)
        return XAuthVerdict::NeedMore;

    const size_t nameAt = 12;
    const size_t dataAt = nameAt + Pad4(h.nameLen);
    const std::string name(reinterpret_cast<const char*>(in.data() + nameAt),
                           h.nameLen);
    // Anything but MIT-MAGIC-COOKIE-1 is refused rather than passed through.
    // An unauthenticated connection reaching the real display is the whole
    // failure this module exists to prevent.
    if (name != kMitMagicCookie)
        return XAuthVerdict::Rejected;
    if (h.dataLen != expectFake.size() || expectFake.empty())
        return XAuthVerdict::Rejected;

    const std::vector<uint8_t> got(in.begin() + static_cast<ptrdiff_t>(dataAt),
                                   in.begin() + static_cast<ptrdiff_t>(dataAt + h.dataLen));
    if (!CookieEqual(got, expectFake))
        return XAuthVerdict::Rejected;

    if (realCookie.empty())
    {
        // Verify-only: no local cookie was found, so the packet goes through
        // as it is and the X server's own access control decides. The caller
        // is required to tell the user this happened.
        out = in;
        return XAuthVerdict::Rewritten;
    }
    if (realCookie.size() != kCookieBytes)
        return XAuthVerdict::Rejected;

    // Same packet, real cookie. The lengths are equal by construction (both
    // cookies are 16 bytes), so no offset in the message moves.
    out = in;
    std::copy(realCookie.begin(), realCookie.end(),
              out.begin() + static_cast<ptrdiff_t>(dataAt));
    return XAuthVerdict::Rewritten;
}

std::vector<uint8_t> LoadXAuthorityFile()
{
    wchar_t buf[MAX_PATH * 2];
    std::wstring path;
    if (GetEnvironmentVariableW(L"XAUTHORITY", buf, _countof(buf)) > 0)
        path = buf;
    else
    {
        std::wstring home;
        if (GetEnvironmentVariableW(L"HOME", buf, _countof(buf)) > 0)
            home = buf;
        else if (GetEnvironmentVariableW(L"USERPROFILE", buf, _countof(buf)) > 0)
            home = buf;
        if (home.empty())
            return {};
        path = home + L"\\.Xauthority";
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    std::vector<uint8_t> out;
    // A cookie file is a few hundred bytes; a huge one is not one.
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
        size.QuadPart <= 1024 * 1024)
    {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr))
            out.clear();
        else
            out.resize(got);
    }
    CloseHandle(h);
    return out;
}

} // namespace amber
