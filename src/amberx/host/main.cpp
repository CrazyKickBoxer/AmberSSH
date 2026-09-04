// AmberXHost — the isolated per-session process that will one day contain
// the X server. Today it contains the transport: it proves the secret, runs
// the handshake, tracks channels, and reports counts.
//
// There is no X11 in this file and it does not pretend otherwise. What it
// establishes is everything around the server — the process boundary, the
// authenticated pipe, the channel bookkeeping and the lifetime rules — so
// that when a server core is dropped in, the only new thing is the server.
//
// Rules this process keeps regardless of what it grows into:
//   * it never logs a cookie, a nonce, a secret or a byte of channel data;
//   * it exits when its parent does, whether or not the Job Object got there
//     first;
//   * it opens no listener of any kind.
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../control/Handshake.h"
#include "../control/Pipe.h"
#include "../control/Protocol.h"

using namespace amber::amberx;

namespace
{

struct Args
{
    std::wstring pipe;
    HANDLE secretHandle = nullptr;
    DWORD parentPid = 0;
};

bool ParseArgs(int argc, wchar_t** argv, Args& a)
{
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::wstring k = argv[i];
        const std::wstring v = argv[i + 1];
        if (k == L"--pipe")
            a.pipe = v;
        else if (k == L"--secret-handle")
            a.secretHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(v.c_str(), nullptr, 10)));
        else if (k == L"--parent")
            a.parentPid = static_cast<DWORD>(wcstoul(v.c_str(), nullptr, 10));
        else
            return false;
    }
    return !a.pipe.empty() && a.secretHandle && a.parentPid;
}

// The secret arrives on an inherited anonymous-pipe handle, never on the
// command line or in the environment, so it is not readable from a process
// listing. Read exactly the expected size and close the handle at once.
bool ReadSecret(HANDLE h, std::vector<uint8_t>& secret)
{
    secret.assign(kSecretBytes, 0);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, secret.data(), static_cast<DWORD>(secret.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != secret.size())
    {
        secret.assign(secret.size(), 0);
        secret.clear();
        return false;
    }
    return true;
}

bool ParentGone(HANDLE parent)
{
    return parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0;
}

bool SendStatus(FramedPipe& p, const ChannelTable& t, uint64_t bytesIn, bool cookieSet)
{
    Frame f;
    f.type = MsgType::HostStatus;
    f.channel = kControlChannel;
    f.payload = MakeHostStatus(static_cast<uint32_t>(t.Count()), bytesIn, cookieSet);
    return p.WriteFrame(f);
}

bool SendError(FramedPipe& p, const char* what)
{
    Frame f;
    f.type = MsgType::HostError;
    f.channel = kControlChannel;
    f.payload = MakeHostError(what);   // a fixed description, never an echo
    return p.WriteFrame(f);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Args a;
    if (!ParseArgs(argc, argv, a))
    {
        fputs("AmberXHost: not for direct use — launched by AmberSSH\n", stderr);
        return 64;
    }

    std::vector<uint8_t> secret;
    if (!ReadSecret(a.secretHandle, secret))
        return 65;

    // SYNCHRONIZE only: enough to notice the parent has exited, not enough
    // to do anything to it.
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, a.parentPid);

    std::string err;
    HANDLE h = ConnectClientPipe(a.pipe, 10000, err);
    if (h == INVALID_HANDLE_VALUE)
        return 66;
    FramedPipe pipe;
    pipe.Adopt(h);

    // ---- handshake: the host does not speak until the controller has ----
    HostHandshake hs;
    if (hs.Begin(secret, RandomBytes(kNonceBytes)).state == Step::State::Failed)
        return 67;
    secret.assign(secret.size(), 0);   // the machine has its own copy now
    while (!hs.Done() && !hs.Failed())
    {
        Frame f;
        const PipeRead r = pipe.ReadFrame(f, 10000);
        if (r != PipeRead::Ok)
            return 68;
        const Step s = hs.OnFrame(f);
        if (s.hasSend && !pipe.WriteFrame(s.send))
            return 69;
        if (s.state == Step::State::Failed)
            return 70;
    }

    // ---- main loop: the stub where the X server goes -------------------
    ChannelTable channels;
    uint64_t bytesIn = 0;
    bool cookieSet = false;
    std::vector<uint8_t> cookie;
    uint32_t display = 0;

    for (;;)
    {
        if (ParentGone(parent))
            return 0;
        Frame f;
        const PipeRead r = pipe.ReadFrame(f, 1000);
        if (r == PipeRead::Timeout)
            continue;
        if (r == PipeRead::Closed)
            return 0;
        if (r == PipeRead::Bad)
            return 71;

        switch (f.type)
        {
        case MsgType::SetCookie:
            if (!ParseSetCookie(f.payload, cookie, display))
                SendError(pipe, "cookie payload malformed");
            else
            {
                cookieSet = true;   // the value is kept, never printed
                SendStatus(pipe, channels, bytesIn, cookieSet);
            }
            break;
        case MsgType::ChannelOpen:
            if (!channels.Open(f.channel))
                SendError(pipe, "channel refused");
            else
                SendStatus(pipe, channels, bytesIn, cookieSet);
            break;
        case MsgType::ChannelData:
            if (!channels.IsOpen(f.channel))
                SendError(pipe, "data on a channel that is not open");
            else
                bytesIn += f.payload.size();   // counted, never inspected
            break;
        case MsgType::ChannelClose:
            if (!channels.Close(f.channel))
                SendError(pipe, "close of a channel that is not open");
            else
                SendStatus(pipe, channels, bytesIn, cookieSet);
            break;
        case MsgType::Shutdown:
            SendStatus(pipe, channels, bytesIn, cookieSet);
            return 0;
        default:
            // Hello/HelloAck/AuthProof after the handshake, or a host-only
            // type arriving from the controller: a protocol error.
            SendError(pipe, "unexpected message after handshake");
            return 72;
        }
    }
}
