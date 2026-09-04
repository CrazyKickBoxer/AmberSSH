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
};

// Registers the window classes and, rootful, the display window. The pipe
// must already be handshaken; the backend owns nothing about its security,
// only its bytes.
bool BackendInit(FramedPipe& pipe, const HostOptions& opt, std::string& err);

// Starts the pipe and server threads, pumps windows until the server has
// returned, and returns the server's exit code.
int BackendRun();

} // namespace amber::amberx
