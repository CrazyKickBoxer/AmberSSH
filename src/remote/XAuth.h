// XAuth.h — X11 forwarding authentication, as pure functions.
//
// The reason this module exists is one specific attack. Naive X11 forwarding
// hands the remote host a cookie that authorises access to your display. A
// compromised remote host can then keep that cookie, reconnect later, and read
// your keystrokes and screen — X11 has no per-client isolation worth the name.
//
// So AmberSSH does what OpenSSH does: it generates a FAKE cookie per session,
// gives the remote host that one, and substitutes the REAL local cookie into
// each X11 connection as it is proxied. The remote host never learns the
// credential that actually opens your display.
//
// Every step of that is a pure function of bytes, which is what lets it be
// tested exhaustively with no X server and no network.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

// The only authentication protocol worth supporting. XDM-AUTHORIZATION-1 is
// obsolete and its DES core is not something to reimplement.
inline constexpr char kMitMagicCookie[] = "MIT-MAGIC-COOKIE-1";
inline constexpr size_t kCookieBytes = 16;

// A parsed "host:display[.screen]" address.
struct XDisplay
{
    std::string host = "127.0.0.1";   // "unix"/"localhost"/empty all mean local
    int display = 0;
    int screen = 0;
    bool valid = false;
    // TCP port for the display: 6000 + display.
    int Port() const { return 6000 + display; }
};

// Parses a DISPLAY string. Rejects anything that would resolve to a port
// outside the X range rather than silently clamping — a display number is a
// small integer, and a huge one is a mistake or an attack, never a display.
XDisplay ParseDisplay(const std::string& s);

// 16 random bytes from the OS CSPRNG, as lowercase hex. Empty on failure, and
// the caller must treat empty as "do not forward" rather than as "no cookie
// needed" — falling back to an unauthenticated display is exactly the mistake
// this module exists to prevent.
std::string MakeCookieHex();

// Hex <-> bytes. `HexToBytes` returns empty for odd length or a non-hex digit,
// so a malformed .Xauthority entry cannot produce a partial cookie.
std::string BytesToHex(const std::vector<uint8_t>& b);
std::vector<uint8_t> HexToBytes(const std::string& hex);

// Constant-time comparison. Cookie checking is an authentication decision and
// must not leak position through timing.
bool CookieEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

// ------------------------------------------------------------- .Xauthority
// One entry of an .Xauthority file.
struct XAuthEntry
{
    uint16_t family = 0;          // 0 = IPv4, 256 = "local", 65535 = wild
    std::string address;
    std::string number;           // display number, as text
    std::string name;             // "MIT-MAGIC-COOKIE-1"
    std::vector<uint8_t> data;
};

// Parses the binary .Xauthority format. Malformed input yields the entries
// read so far rather than throwing: a truncated file is common (a half-written
// one from a starting X server) and must not take the session down.
std::vector<XAuthEntry> ParseXAuthority(const std::vector<uint8_t>& file);

// The MIT-MAGIC-COOKIE-1 entry matching this display, or empty. Matching is
// deliberately loose on address (X servers write "hostname", "localhost" or
// nothing) and strict on display number, because connecting to display 0 with
// display 1's cookie must fail rather than half-work.
std::vector<uint8_t> CookieForDisplay(const std::vector<XAuthEntry>& entries,
                                      const XDisplay& d);

// --------------------------------------------------- the X11 setup packet
// Every X11 connection opens with a fixed 12-byte header followed by the
// authorisation protocol name and data, each padded to 4 bytes. That is the
// only part AmberSSH has to understand: it reads the client's fake cookie,
// checks it, writes the real one in its place, and never looks at another byte
// of the X protocol.
struct XSetupHeader
{
    bool bigEndian = false;
    uint16_t protoMajor = 0, protoMinor = 0;
    uint16_t nameLen = 0, dataLen = 0;
    // Total bytes of the setup message, header included.
    size_t total = 0;
    bool valid = false;
};

// Reads the header. `valid` is false when the buffer is too short to decide —
// the caller keeps buffering — and also when the byte-order byte is neither
// 'B' nor 'l', which is not an X11 client at all.
XSetupHeader ParseSetupHeader(const uint8_t* data, size_t len);

// What to do with a connection whose setup packet has arrived.
enum class XAuthVerdict
{
    NeedMore,      // the packet is incomplete; buffer and call again
    Rewritten,     // the fake cookie matched; `out` is the packet to forward
    Rejected,      // wrong protocol or wrong cookie: close the channel
};

// Checks the setup packet's cookie against `expectFake` and, if it matches,
// produces the same packet with `realCookie` substituted.
//
// An EMPTY `realCookie` means "verify but do not substitute": the fake cookie
// is still checked, and the packet is forwarded unchanged for the local X
// server's own access control to judge. That is the case where no .Xauthority
// entry could be found — common on Windows, where people start their X server
// with access control disabled. It is weaker than substitution and the caller
// must say so, but it still refuses a channel that fails the check, which is
// more than forwarding nothing at all would do.
//
// Rejection is the default for anything unexpected — an unknown auth protocol,
// a length that disagrees with the buffer, a cookie of the wrong size. A
// forwarded X11 channel that cannot be authenticated is closed, never passed
// through unauthenticated.
XAuthVerdict RewriteSetup(const std::vector<uint8_t>& in,
                          const std::vector<uint8_t>& expectFake,
                          const std::vector<uint8_t>& realCookie,
                          std::vector<uint8_t>& out);

// A setup packet is a few dozen bytes. Anything claiming to be much larger is
// not a client AmberSSH is willing to buffer for.
inline constexpr size_t kMaxSetupBytes = 4096;

// Reads %XAUTHORITY%, or %HOME%\.Xauthority, or %USERPROFILE%\.Xauthority.
// The one function here that touches the filesystem; returns empty when there
// is no such file, which the caller must treat as "no substitution possible"
// rather than as an error.
std::vector<uint8_t> LoadXAuthorityFile();

} // namespace amber
