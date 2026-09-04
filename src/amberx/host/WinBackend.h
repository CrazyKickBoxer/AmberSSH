// WinBackend.h — what wmain needs from the Windows half of AmberXHost.
// The rest of the backend is the C surface declared in server/amberwin.h.
#pragma once

#include <cstdint>
#include <string>

namespace amber::amberx
{

class FramedPipe;

// Everything the host was launched with that the backend needs. The
// identity fields are AmberSSH's, never the remote's: they are painted on
// the strip no X drawing can reach.
struct HostOptions
{
    bool rootless = true;       // --rootful turns it off
    int width = 1280, height = 800;   // rootful only
    uint32_t display = 0;
    std::string keymap;         // an .xkm path, or empty for the built-in map
    std::string identity;       // "host · user"
    std::string mode = "X11 FORWARDED";   // "RESTRICTED"/"TRUSTED" once Phase 5 enforces them
    std::string sigil;          // the host-key sigil mnemonic
    int skin = 0;               // AmberSSH chrome style index
    bool trusted = false;       // the SECURITY trust level of the session cookie
    int authTimeout = 1200;     // seconds an unused untrusted cookie lives
    int clipboard = 0;          // AMBERWIN_CLIP_*; 0 (off) is the default
    // The X screen rectangle in Windows desktop coordinates; w == 0 means
    // the whole virtual desktop, which is what rootless mode used to assume.
    int deskX = 0, deskY = 0, deskW = 0, deskH = 0;
    int presentCapHz = 0;       // 0 = repaint as often as damage arrives
};

// Registers the window classes and, rootful, the display window. The pipe
// must already be handshaken; the backend owns nothing about its security,
// only its bytes.
bool BackendInit(FramedPipe& pipe, const HostOptions& opt, std::string& err);

// Starts the pipe and server threads, pumps windows until the server has
// returned, and returns the server's exit code.
int BackendRun();

// For the crash handler only: says whether a faulting address lands in — or
// just past — a frame's pixel buffer, and prints that frame's geometry. A bare
// address says nothing; "3,158,016 bytes past frame 0x2000003, window 802x622,
// buffer 802x622" names the bug. Writes at most `cap` bytes including the
// terminator and returns the length, or 0 if the address belongs to no frame.
// Reads live frame state from whichever thread is dying, which is only
// defensible because the process is already ending.
int DescribeAddress(const void* addr, char* out, int cap);

} // namespace amber::amberx
