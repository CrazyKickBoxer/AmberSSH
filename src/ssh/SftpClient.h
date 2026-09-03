// SftpClient.h — one blocking SSH+SFTP connection for file operations and
// remote commands, used from a worker thread by the SFTP browser, the
// remote-file preview and the vitals monitor. Never touches the shell
// channel of a terminal session: it authenticates on its own with the same
// profile and retained secrets.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../profiles/ConnectionProfile.h"

typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_SFTP LIBSSH2_SFTP;

namespace amber
{

struct SftpEntry
{
    std::string name;
    bool dir = false;
    bool link = false;
    uint64_t size = 0;
    uint64_t mtime = 0;       // unix seconds
    uint32_t perms = 0;       // st_mode bits
    uint32_t uid = 0, gid = 0;
};

class SftpClient
{
public:
    // Called during transfers; return false to cancel.
    using Progress = std::function<bool(uint64_t done, uint64_t total)>;

    SftpClient() = default;
    ~SftpClient();
    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;

    // Blocking connect + auth (password / key / agent / keyboard-interactive).
    // Secrets are scrubbed from the arguments' copies before returning.
    bool Connect(const ConnectionProfile& p, std::string password,
                 std::string passphrase, std::string& err);
    void Close();
    bool Connected() const { return m_sftp != nullptr; }

    std::string Realpath(const std::string& path);
    bool List(const std::string& dir, std::vector<SftpEntry>& out, std::string& err);
    bool Stat(const std::string& path, SftpEntry& out);
    bool Mkdir(const std::string& path, std::string& err);
    bool Rmdir(const std::string& path, std::string& err);       // empty dir
    bool Unlink(const std::string& path, std::string& err);
    bool Rename(const std::string& from, const std::string& to, std::string& err);
    bool Chmod(const std::string& path, uint32_t perms, std::string& err);
    // Recursive delete (files + subdirectories).
    bool RemoveTree(const std::string& path, std::string& err);

    bool Download(const std::string& remote, const std::wstring& local,
                  const Progress& cb, std::string& err);
    bool Upload(const std::wstring& local, const std::string& remote,
                const Progress& cb, std::string& err);
    // Whole file into memory (previews). maxBytes caps the read.
    bool ReadFile(const std::string& remote, std::string& out, size_t maxBytes,
                  std::string& err);

    // Runs a command on a fresh exec channel; stdout into `out`.
    bool Exec(const std::string& cmd, std::string& out, std::string& err);

    const std::string& LastError() const { return m_lastErr; }

private:
    std::string SftpErr(const char* what);

    SOCKET m_sock = INVALID_SOCKET;
    LIBSSH2_SESSION* m_session = nullptr;
    LIBSSH2_SFTP* m_sftp = nullptr;
    std::string m_lastErr;
};

// Helpers shared by the browser.
std::string SftpJoin(const std::string& dir, const std::string& name);
std::string SftpParent(const std::string& path);
std::string SftpPermString(uint32_t perms, bool dir, bool link);

} // namespace amber
