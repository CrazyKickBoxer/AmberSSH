// AmberXController.h — AmberSSH's side of the AmberXHost process.
//
// Owns everything the host must not be able to influence about its own
// existence: the pipe and its DACL, the per-launch secret and how it is
// delivered, the Job Object that ends the host when this process ends, and
// the handshake that proves the thing on the other end is the child that was
// launched.
//
// Lifetime rule: a controller that fails at any step has nothing running —
// Start() either returns with a proven, handshaken host or with the process
// already killed and every handle closed.
//
// The I/O here is synchronous with timeouts. That is deliberate for now: the
// controller is exercised by --preview-amberx on a throwaway thread, and the
// asynchronous integration into the SSH worker is the next step, not this
// one.
#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "control/Pipe.h"
#include "control/Protocol.h"

namespace amber::amberx
{

class AmberXController
{
public:
    AmberXController() = default;
    ~AmberXController() { Stop(); }
    AmberXController(const AmberXController&) = delete;
    AmberXController& operator=(const AmberXController&) = delete;

    // Creates the pipe, launches AmberXHost.exe inside a Job Object, and
    // completes the handshake. On any failure the host is dead on return.
    // What the host is launched with beyond the transport: the identity
    // badge AmberSSH controls and the remote cannot touch. Set before Start.
    struct Launch
    {
        std::string identity;       // "host · user"
        // The mode word on the strip. "RESTRICTED" and "TRUSTED" are
        // reserved for Phase 5, when the server enforces them; until then
        // AmberSSH says what is true, which is that X11 is being forwarded.
        std::string modeLabel = "X11 FORWARDED";
        std::string sigil;          // host-key sigil mnemonic
        int skin = 0;               // chrome style index
        std::string keymap;         // .xkm path, or empty
        bool rootful = false;
    };
    void Configure(const Launch& l) { m_launch = l; }

    bool Start(std::string& err);
    bool Alive() const;
    // The verified DACL summary from the pipe this host was reached over.
    const std::string& Security() const { return m_security; }

    bool SetCookie(const std::vector<uint8_t>& cookie16, uint32_t display);
    bool OpenChannel(uint32_t id);
    bool SendData(uint32_t id, const uint8_t* data, size_t len);
    bool CloseChannel(uint32_t id);
    // Incoming HostStatus / HostError.
    PipeRead Poll(Frame& out, DWORD timeoutMs);
    // Shutdown frame, wait briefly for a clean exit, then the Job Object
    // takes whatever is left.
    void Stop();

private:
    ServerPipe m_srv;
    FramedPipe m_pipe;
    HANDLE m_job = nullptr;
    HANDLE m_proc = nullptr;
    ChannelTable m_channels;
    std::string m_security;
    Launch m_launch;
    bool m_ready = false;
    void Kill();
};

// --preview-amberx: runs one controller through cookie, channel open, data,
// close and shutdown, writes a plain-text report to %TEMP%\amberx-preview.txt
// and returns 0 on success. Needs no window and no GPU, so it runs before
// App::Init and exits.
int RunPreview();

} // namespace amber::amberx
