#include "Pipe.h"

#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace amber::amberx
{

namespace
{

// The current user's SID, as bytes (for EqualSid) and as text (for SDDL).
bool CurrentUserSid(std::vector<uint8_t>& sidBytes, std::wstring& sidText)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    std::vector<uint8_t> buf(need ? need : 1);
    const BOOL ok = GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
    CloseHandle(tok);
    if (!ok)
        return false;
    PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    const DWORD len = GetLengthSid(sid);
    sidBytes.assign(reinterpret_cast<uint8_t*>(sid), reinterpret_cast<uint8_t*>(sid) + len);
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(sid, &s))
        return false;
    sidText = s;
    LocalFree(s);
    return true;
}

// Reads the DACL back off the created pipe and checks it is EXACTLY what was
// asked for: one ACE, access-allowed, naming the current user. Requesting a
// DACL and verifying it are different things — this is the difference
// between "we asked for a private pipe" and "we have one".
bool VerifyPipeDacl(HANDLE pipe, const std::vector<uint8_t>& userSid, std::string& summary)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS || !dacl)
    {
        summary = "DACL unreadable";
        return false;
    }
    bool ok = false;
    ACL_SIZE_INFORMATION info = {};
    if (GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
    {
        if (info.AceCount != 1)
            summary = "DACL has " + std::to_string(info.AceCount) + " ACEs, expected 1";
        else
        {
            void* acePtr = nullptr;
            if (GetAce(dacl, 0, &acePtr))
            {
                auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(acePtr);
                if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
                    summary = "ACE 0 is not access-allowed";
                else if (!EqualSid(&ace->SidStart, const_cast<uint8_t*>(userSid.data())))
                    summary = "ACE 0 names a SID other than the current user";
                else
                {
                    summary = "1 ACE, access-allowed, current user only";
                    ok = true;
                }
            }
        }
    }
    if (sd)
        LocalFree(sd);
    return ok;
}

std::wstring Hex(const std::vector<uint8_t>& b)
{
    static const wchar_t* kHex = L"0123456789abcdef";
    std::wstring s;
    for (uint8_t c : b)
    {
        s.push_back(kHex[c >> 4]);
        s.push_back(kHex[c & 0x0F]);
    }
    return s;
}

} // namespace

std::vector<uint8_t> RandomBytes(size_t n)
{
    std::vector<uint8_t> out(n);
    if (n == 0)
        return out;
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    return out;
}

bool CreateServerPipe(ServerPipe& out, std::string& err)
{
    const std::vector<uint8_t> rnd = RandomBytes(16);
    if (rnd.empty())
    {
        err = "no entropy for the pipe name";
        return false;
    }
    // One SID: the current user. Not SYSTEM, not Administrators, not the
    // logon SID — the narrowest thing that still lets the child we launch
    // (which runs as this user) open the pipe. D:P makes the DACL protected,
    // so nothing is inherited from the object's container.
    std::vector<uint8_t> userSid;
    std::wstring sidText;
    if (!CurrentUserSid(userSid, sidText))
    {
        err = "could not determine the current user SID";
        return false;
    }
    const std::wstring sddl = L"D:P(A;;GA;;;" + sidText + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &sd, nullptr))
    {
        err = "could not build the pipe security descriptor";
        return false;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

    out.name = L"\\\\.\\pipe\\AmberX-" + Hex(rnd);
    out.h = CreateNamedPipeW(
        out.name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,                       // exactly one instance: one host per pipe
        64 * 1024, 64 * 1024,
        0, &sa);
    LocalFree(sd);
    if (out.h == INVALID_HANDLE_VALUE)
    {
        // ERROR_ACCESS_DENIED here with FIRST_PIPE_INSTANCE means the name
        // already existed — the squatting case. Report it as what it is.
        err = GetLastError() == ERROR_ACCESS_DENIED
                  ? "pipe name already taken (refusing to share it)"
                  : "CreateNamedPipe failed";
        out.name.clear();
        return false;
    }
    // Verified, not assumed. A pipe whose DACL is not what was requested is
    // closed before anything can connect to it.
    if (!VerifyPipeDacl(out.h, userSid, out.security))
    {
        CloseHandle(out.h);
        out.h = INVALID_HANDLE_VALUE;
        out.name.clear();
        err = "pipe DACL verification failed: " + out.security;
        return false;
    }
    return true;
}

bool WaitForClient(HANDLE pipe, DWORD timeoutMs)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;
    bool ok = false;
    if (ConnectNamedPipe(pipe, &ov))
        ok = true;
    else
    {
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED)
            ok = true;
        else if (e == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0)
            {
                DWORD n = 0;
                ok = GetOverlappedResult(pipe, &ov, &n, FALSE) != 0;
            }
            else
                CancelIo(pipe);
        }
    }
    CloseHandle(ov.hEvent);
    return ok;
}

HANDLE ConnectClientPipe(const std::wstring& name, DWORD timeoutMs, std::string& err)
{
    if (!WaitNamedPipeW(name.c_str(), timeoutMs))
    {
        err = "the pipe did not become available";
        return INVALID_HANDLE_VALUE;
    }
    // SECURITY_ANONYMOUS: whatever is on the server end, it cannot
    // impersonate this process. A squatting server gets nothing from us.
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        err = "could not open the pipe";
    return h;
}

// ---------------------------------------------------------------- framed
FramedPipe::~FramedPipe()
{
    Close();
}

void FramedPipe::Adopt(HANDLE h)
{
    Close();
    m_h = h;
    m_evRead = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_evWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void FramedPipe::Close()
{
    if (Valid())
    {
        CancelIo(m_h);
        CloseHandle(m_h);
    }
    m_h = INVALID_HANDLE_VALUE;
    if (m_evRead) CloseHandle(m_evRead);
    if (m_evWrite) CloseHandle(m_evWrite);
    m_evRead = m_evWrite = nullptr;
    m_buf.clear();
}

bool FramedPipe::WriteFrame(const Frame& f)
{
    if (!Valid())
        return false;
    std::vector<uint8_t> wire;
    if (!Encode(f, wire))
        return false;            // never put an unencodable frame on the wire
    size_t at = 0;
    while (at < wire.size())
    {
        OVERLAPPED ov = {};
        ov.hEvent = m_evWrite;
        ResetEvent(m_evWrite);
        DWORD n = 0;
        if (!WriteFile(m_h, wire.data() + at, static_cast<DWORD>(wire.size() - at), &n, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            if (WaitForSingleObject(m_evWrite, 5000) != WAIT_OBJECT_0)
            {
                CancelIo(m_h);
                return false;
            }
            if (!GetOverlappedResult(m_h, &ov, &n, FALSE))
                return false;
        }
        at += n;
    }
    return true;
}

PipeRead FramedPipe::ReadFrame(Frame& out, DWORD timeoutMs)
{
    if (!Valid())
        return PipeRead::Closed;
    for (;;)
    {
        size_t consumed = 0;
        switch (Decode(m_buf, out, consumed))
        {
        case Decoded::Ok:
            m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<ptrdiff_t>(consumed));
            return PipeRead::Ok;
        case Decoded::Bad:
            return PipeRead::Bad;
        case Decoded::NeedMore:
            break;
        }
        // Decode refuses any length over kMaxPayload before asking for more,
        // so a buffer that is still incomplete past this size is a logic
        // error, not a large frame.
        if (m_buf.size() > kHeaderBytes + kMaxPayload)
            return PipeRead::Bad;

        uint8_t chunk[64 * 1024];
        OVERLAPPED ov = {};
        ov.hEvent = m_evRead;
        ResetEvent(m_evRead);
        DWORD n = 0;
        if (!ReadFile(m_h, chunk, sizeof(chunk), &n, &ov))
        {
            const DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE)
                return PipeRead::Closed;
            if (e != ERROR_IO_PENDING)
                return PipeRead::Bad;
            if (WaitForSingleObject(m_evRead, timeoutMs) != WAIT_OBJECT_0)
            {
                CancelIo(m_h);
                // The cancelled read may still complete with data: collect it
                // so nothing is lost, then report the timeout.
                if (GetOverlappedResult(m_h, &ov, &n, TRUE) && n > 0)
                    m_buf.insert(m_buf.end(), chunk, chunk + n);
                return PipeRead::Timeout;
            }
            if (!GetOverlappedResult(m_h, &ov, &n, FALSE))
                return GetLastError() == ERROR_BROKEN_PIPE ? PipeRead::Closed : PipeRead::Bad;
        }
        if (n == 0)
            return PipeRead::Closed;
        m_buf.insert(m_buf.end(), chunk, chunk + n);
    }
}

} // namespace amber::amberx
