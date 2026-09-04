// Pipe.h — the named pipe between AmberSSH and AmberXHost, and framed I/O
// over it.
//
// The pipe is the security boundary the handshake sits behind, so its
// creation is where the prompt's IPC controls actually live:
//
//   * a random, unguessable name;
//   * FILE_FLAG_FIRST_PIPE_INSTANCE, so a squatter cannot pre-create it;
//   * PIPE_REJECT_REMOTE_CLIENTS, so it is never reachable over the network;
//   * a DACL naming exactly one SID — the current user — with no Everyone,
//     no anonymous, no inherited entries (D:P);
//   * on the client side, SECURITY_ANONYMOUS, so a server can never
//     impersonate the connecting process.
//
// Framed I/O is a thin loop over Protocol::Decode: it never hands a frame up
// until Decode says it is whole and sane, and it treats Bad as fatal.
#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "Protocol.h"

namespace amber::amberx
{

enum class PipeRead
{
    Ok,        // `out` holds a frame
    Timeout,   // nothing complete within the wait
    Closed,    // the far end went away
    Bad,       // framing error: close the pipe, there is no recovery
};

// Overlapped, byte-mode, one frame at a time. Owns the handle it is given.
class FramedPipe
{
public:
    FramedPipe() = default;
    ~FramedPipe();
    FramedPipe(const FramedPipe&) = delete;
    FramedPipe& operator=(const FramedPipe&) = delete;

    void Adopt(HANDLE h);
    bool Valid() const { return m_h != INVALID_HANDLE_VALUE && m_h != nullptr; }
    bool WriteFrame(const Frame& f);
    PipeRead ReadFrame(Frame& out, DWORD timeoutMs);
    void Close();

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
    HANDLE m_evRead = nullptr;
    HANDLE m_evWrite = nullptr;
    std::vector<uint8_t> m_buf;
};

// Controller side: creates the listening end. `name` comes back as the full
// \\.\pipe\... path to hand to the child.
struct ServerPipe
{
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring name;
    // What the DACL was verified to contain, read back from the object after
    // creation — never just what was requested.
    std::string security;
};
bool CreateServerPipe(ServerPipe& out, std::string& err);
bool WaitForClient(HANDLE pipe, DWORD timeoutMs);

// Host side.
HANDLE ConnectClientPipe(const std::wstring& name, DWORD timeoutMs, std::string& err);

// OS CSPRNG. Empty on failure — a caller must treat that as "do not proceed",
// never as "use zeros".
std::vector<uint8_t> RandomBytes(size_t n);

} // namespace amber::amberx
