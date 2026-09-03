#include "SftpClient.h"

#include <ws2tcpip.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace amber
{

namespace
{
// keyboard-interactive: answer every prompt with the password.
thread_local const std::string* tlKbdPassword = nullptr;
void KbdCallback(const char*, int, const char*, int, int numPrompts,
                 const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                 LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void**)
{
    (void)prompts;
    for (int i = 0; i < numPrompts; ++i)
    {
        if (!tlKbdPassword)
            continue;
        responses[i].text = _strdup(tlKbdPassword->c_str());
        responses[i].length = static_cast<unsigned>(tlKbdPassword->size());
    }
}
} // namespace

SftpClient::~SftpClient()
{
    Close();
}

std::string SftpClient::SftpErr(const char* what)
{
    char* msg = nullptr;
    int len = 0;
    if (m_session)
        libssh2_session_last_error(m_session, &msg, &len, 0);
    unsigned long sftpErr = m_sftp ? libssh2_sftp_last_error(m_sftp) : 0;
    std::string s = what;
    if (sftpErr)
    {
        static const char* kNames[] = {
            "ok", "end of file", "no such file", "permission denied", "failure",
            "bad message", "no connection", "connection lost",
            "operation unsupported", "invalid handle", "no such path",
            "file already exists", "write protect", "no media", "no space",
            "quota exceeded", "unknown principal", "lock conflict",
            "directory not empty", "not a directory", "invalid filename",
            "link loop",
        };
        s += ": ";
        s += (sftpErr < sizeof(kNames) / sizeof(kNames[0])) ? kNames[sftpErr]
                                                             : "sftp error";
    }
    else if (msg && len > 0)
        s += std::string(": ") + std::string(msg, static_cast<size_t>(len));
    m_lastErr = s;
    return s;
}

bool SftpClient::Connect(const ConnectionProfile& p, std::string password,
                         std::string passphrase, std::string& err)
{
    Close();
    static bool wsaUp = false;
    if (!wsaUp)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        libssh2_init(0);
        wsaUp = true;
    }
    auto scrub = [&]() {
        if (!password.empty())
            SecureZeroMemory(&password[0], password.size());
        if (!passphrase.empty())
            SecureZeroMemory(&passphrase[0], passphrase.size());
    };

    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port[16];
    snprintf(port, sizeof(port), "%d", p.port);
    if (getaddrinfo(p.host.c_str(), port, &hints, &res) == 0)
    {
        for (addrinfo* ai = res; ai; ai = ai->ai_next)
        {
            m_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (m_sock == INVALID_SOCKET)
                continue;
            if (connect(m_sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
                break;
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        freeaddrinfo(res);
    }
    if (m_sock == INVALID_SOCKET)
    {
        err = "could not connect to " + p.host;
        scrub();
        return false;
    }

    m_session = libssh2_session_init();
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, 30000);
    if (libssh2_session_handshake(m_session, m_sock) != 0)
    {
        err = SftpErr("SSH handshake failed");
        Close();
        scrub();
        return false;
    }

    bool authed = false;
    const char* user = p.username.c_str();
    unsigned ulen = static_cast<unsigned>(p.username.size());
    char* authList = libssh2_userauth_list(m_session, user, ulen);
    if (p.auth == AuthMethod::Agent)
    {
        if (LIBSSH2_AGENT* ag = libssh2_agent_init(m_session))
        {
            if (libssh2_agent_connect(ag) == 0 && libssh2_agent_list_identities(ag) == 0)
            {
                struct libssh2_agent_publickey* id = nullptr;
                struct libssh2_agent_publickey* prev = nullptr;
                while (!authed && libssh2_agent_get_identity(ag, &id, prev) == 0)
                {
                    if (libssh2_agent_userauth(ag, user, id) == 0)
                        authed = true;
                    prev = id;
                }
            }
            libssh2_agent_disconnect(ag);
            libssh2_agent_free(ag);
        }
    }
    else if (p.auth == AuthMethod::PublicKey)
    {
        authed = libssh2_userauth_publickey_fromfile_ex(
                     m_session, user, ulen, nullptr, p.privateKeyPath.c_str(),
                     passphrase.empty() ? nullptr : passphrase.c_str()) == 0;
    }
    if (!authed && !password.empty())
    {
        if (authList && strstr(authList, "password"))
            authed = libssh2_userauth_password(m_session, user, password.c_str()) == 0;
        if (!authed && authList && strstr(authList, "keyboard-interactive"))
        {
            tlKbdPassword = &password;
            authed = libssh2_userauth_keyboard_interactive(m_session, user,
                                                           &KbdCallback) == 0;
            tlKbdPassword = nullptr;
        }
    }
    scrub();
    if (!authed)
    {
        err = "authentication failed";
        Close();
        return false;
    }

    m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp)
    {
        err = SftpErr("SFTP subsystem unavailable");
        Close();
        return false;
    }
    return true;
}

void SftpClient::Close()
{
    if (m_sftp)
    {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session)
    {
        libssh2_session_disconnect(m_session, "bye");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_sock != INVALID_SOCKET)
    {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

std::string SftpClient::Realpath(const std::string& path)
{
    if (!m_sftp)
        return path;
    char real[1024];
    int n = libssh2_sftp_realpath(m_sftp, path.c_str(), real, sizeof(real) - 1);
    return n > 0 ? std::string(real, static_cast<size_t>(n)) : path;
}

bool SftpClient::List(const std::string& dir, std::vector<SftpEntry>& out,
                      std::string& err)
{
    out.clear();
    if (!m_sftp)
    {
        err = "not connected";
        return false;
    }
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(m_sftp, dir.c_str());
    if (!h)
    {
        err = SftpErr("cannot open directory");
        return false;
    }
    char name[1024];
    LIBSSH2_SFTP_ATTRIBUTES at;
    int n;
    while ((n = libssh2_sftp_readdir(h, name, sizeof(name), &at)) > 0)
    {
        std::string nm(name, static_cast<size_t>(n));
        if (nm == "." || nm == "..")
            continue;
        SftpEntry e;
        e.name = nm;
        e.perms = (at.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? at.permissions : 0;
        e.link = LIBSSH2_SFTP_S_ISLNK(e.perms) != 0;
        e.dir = LIBSSH2_SFTP_S_ISDIR(e.perms) != 0;
        if (e.link)
        {
            // Follow the link to classify it (a link to a directory browses).
            LIBSSH2_SFTP_ATTRIBUTES st;
            if (libssh2_sftp_stat_ex(m_sftp, SftpJoin(dir, nm).c_str(),
                                     static_cast<unsigned>(dir.size() + 1 + nm.size()),
                                     LIBSSH2_SFTP_STAT, &st) == 0)
                e.dir = LIBSSH2_SFTP_S_ISDIR(st.permissions) != 0;
        }
        e.size = (at.flags & LIBSSH2_SFTP_ATTR_SIZE) ? at.filesize : 0;
        e.mtime = (at.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? at.mtime : 0;
        if (at.flags & LIBSSH2_SFTP_ATTR_UIDGID)
        {
            e.uid = at.uid;
            e.gid = at.gid;
        }
        out.push_back(std::move(e));
    }
    libssh2_sftp_closedir(h);
    return true;
}

bool SftpClient::Stat(const std::string& path, SftpEntry& out)
{
    if (!m_sftp)
        return false;
    LIBSSH2_SFTP_ATTRIBUTES at;
    if (libssh2_sftp_stat_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size()),
                             LIBSSH2_SFTP_STAT, &at) != 0)
        return false;
    out = SftpEntry{};
    size_t slash = path.find_last_of('/');
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    out.perms = (at.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? at.permissions : 0;
    out.dir = LIBSSH2_SFTP_S_ISDIR(out.perms) != 0;
    out.size = (at.flags & LIBSSH2_SFTP_ATTR_SIZE) ? at.filesize : 0;
    out.mtime = (at.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? at.mtime : 0;
    return true;
}

bool SftpClient::Mkdir(const std::string& path, std::string& err)
{
    if (libssh2_sftp_mkdir_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size()),
                              0755) != 0)
    {
        err = SftpErr("mkdir failed");
        return false;
    }
    return true;
}

bool SftpClient::Rmdir(const std::string& path, std::string& err)
{
    if (libssh2_sftp_rmdir_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size())) != 0)
    {
        err = SftpErr("rmdir failed");
        return false;
    }
    return true;
}

bool SftpClient::Unlink(const std::string& path, std::string& err)
{
    if (libssh2_sftp_unlink_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size())) != 0)
    {
        err = SftpErr("delete failed");
        return false;
    }
    return true;
}

bool SftpClient::Rename(const std::string& from, const std::string& to, std::string& err)
{
    if (libssh2_sftp_rename_ex(m_sftp, from.c_str(), static_cast<unsigned>(from.size()),
                               to.c_str(), static_cast<unsigned>(to.size()),
                               LIBSSH2_SFTP_RENAME_OVERWRITE |
                                   LIBSSH2_SFTP_RENAME_ATOMIC |
                                   LIBSSH2_SFTP_RENAME_NATIVE) != 0)
    {
        err = SftpErr("rename failed");
        return false;
    }
    return true;
}

bool SftpClient::Chmod(const std::string& path, uint32_t perms, std::string& err)
{
    LIBSSH2_SFTP_ATTRIBUTES at = {};
    at.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    at.permissions = perms;
    if (libssh2_sftp_stat_ex(m_sftp, path.c_str(), static_cast<unsigned>(path.size()),
                             LIBSSH2_SFTP_SETSTAT, &at) != 0)
    {
        err = SftpErr("chmod failed");
        return false;
    }
    return true;
}

bool SftpClient::RemoveTree(const std::string& path, std::string& err)
{
    std::vector<SftpEntry> kids;
    if (!List(path, kids, err))
        return Unlink(path, err);   // not a directory: plain delete
    for (const SftpEntry& k : kids)
    {
        std::string kp = SftpJoin(path, k.name);
        if (k.dir && !k.link)
        {
            if (!RemoveTree(kp, err))
                return false;
        }
        else if (!Unlink(kp, err))
            return false;
    }
    return Rmdir(path, err);
}

bool SftpClient::Download(const std::string& remote, const std::wstring& local,
                          const Progress& cb, std::string& err)
{
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open_ex(
        m_sftp, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!h)
    {
        err = SftpErr("open failed");
        return false;
    }
    uint64_t total = 0;
    {
        LIBSSH2_SFTP_ATTRIBUTES at;
        if (libssh2_sftp_fstat_ex(h, &at, 0) == 0 && (at.flags & LIBSSH2_SFTP_ATTR_SIZE))
            total = at.filesize;
    }
    FILE* f = _wfopen(local.c_str(), L"wb");
    if (!f)
    {
        libssh2_sftp_close(h);
        err = "cannot write local file";
        return false;
    }
    std::vector<char> buf(256 * 1024);
    uint64_t done = 0;
    bool ok = true;
    for (;;)
    {
        ssize_t n = libssh2_sftp_read(h, buf.data(), buf.size());
        if (n == 0)
            break;
        if (n < 0)
        {
            err = SftpErr("read failed");
            ok = false;
            break;
        }
        fwrite(buf.data(), 1, static_cast<size_t>(n), f);
        done += static_cast<uint64_t>(n);
        if (cb && !cb(done, total))
        {
            err = "cancelled";
            ok = false;
            break;
        }
    }
    fclose(f);
    libssh2_sftp_close(h);
    if (!ok)
        DeleteFileW(local.c_str());
    return ok;
}

bool SftpClient::Upload(const std::wstring& local, const std::string& remote,
                        const Progress& cb, std::string& err)
{
    FILE* f = _wfopen(local.c_str(), L"rb");
    if (!f)
    {
        err = "cannot read local file";
        return false;
    }
    _fseeki64(f, 0, SEEK_END);
    uint64_t total = static_cast<uint64_t>(_ftelli64(f));
    _fseeki64(f, 0, SEEK_SET);
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open_ex(
        m_sftp, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP |
            LIBSSH2_SFTP_S_IROTH,
        LIBSSH2_SFTP_OPENFILE);
    if (!h)
    {
        fclose(f);
        err = SftpErr("remote open failed");
        return false;
    }
    std::vector<char> buf(256 * 1024);
    uint64_t done = 0;
    bool ok = true;
    size_t r;
    while (ok && (r = fread(buf.data(), 1, buf.size(), f)) > 0)
    {
        char* p = buf.data();
        size_t left = r;
        while (left > 0)
        {
            ssize_t w = libssh2_sftp_write(h, p, left);
            if (w < 0)
            {
                err = SftpErr("write failed");
                ok = false;
                break;
            }
            p += w;
            left -= static_cast<size_t>(w);
            done += static_cast<uint64_t>(w);
        }
        if (ok && cb && !cb(done, total))
        {
            err = "cancelled";
            ok = false;
        }
    }
    fclose(f);
    libssh2_sftp_close(h);
    return ok;
}

bool SftpClient::ReadFile(const std::string& remote, std::string& out, size_t maxBytes,
                          std::string& err)
{
    out.clear();
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open_ex(
        m_sftp, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!h)
    {
        err = SftpErr("open failed");
        return false;
    }
    std::vector<char> buf(64 * 1024);
    while (out.size() < maxBytes)
    {
        ssize_t n = libssh2_sftp_read(h, buf.data(),
                                      std::min(buf.size(), maxBytes - out.size()));
        if (n <= 0)
            break;
        out.append(buf.data(), static_cast<size_t>(n));
    }
    libssh2_sftp_close(h);
    return true;
}

bool SftpClient::Exec(const std::string& cmd, std::string& out, std::string& err)
{
    out.clear();
    if (!m_session)
    {
        err = "not connected";
        return false;
    }
    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(m_session);
    if (!ch)
    {
        err = SftpErr("channel open failed");
        return false;
    }
    if (libssh2_channel_exec(ch, cmd.c_str()) != 0)
    {
        err = SftpErr("exec failed");
        libssh2_channel_free(ch);
        return false;
    }
    char buf[8192];
    for (;;)
    {
        ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > (1u << 20))
            break;
    }
    libssh2_channel_send_eof(ch);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    return true;
}

std::string SftpJoin(const std::string& dir, const std::string& name)
{
    if (dir.empty() || dir == "/")
        return "/" + name;
    if (dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

std::string SftpParent(const std::string& path)
{
    if (path.size() <= 1)
        return "/";
    size_t end = path.size();
    if (path[end - 1] == '/')
        --end;
    size_t slash = path.find_last_of('/', end - 1);
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string SftpPermString(uint32_t perms, bool dir, bool link)
{
    std::string s(10, '-');
    s[0] = link ? 'l' : (dir ? 'd' : '-');
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        if (perms & (1u << (8 - i)))
            s[static_cast<size_t>(1 + i)] = rwx[i];
    return s;
}

} // namespace amber
