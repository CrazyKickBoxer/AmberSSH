#include "SftpPanel.h"
#include "Theme.h"
#include "../ssh/RemoteName.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace amber
{
namespace
{

// Live theme palette, refreshed at Open().
COLORREF kBg, kField, kText, kDim, kSelBg, kAccent;
void RefreshPalette()
{
    amber::DialogPalette p = amber::MakeDialogPalette();
    kBg = p.bg; kField = p.field; kText = p.text; kDim = p.textDim;
    kSelBg = p.selBg; kAccent = p.accent;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

struct Entry
{
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

// ---- worker: its own SSH+SFTP connection on a background thread ----------
class Worker
{
public:
    struct Snapshot
    {
        std::string cwd;
        std::vector<Entry> entries;
        std::string status;
        bool busy = true;
        unsigned gen = 0;
    };

    void Start(const ConnectionProfile& p, std::string pw, std::string pp)
    {
        m_profile = p;
        m_pw = std::move(pw);
        m_pp = std::move(pp);
        m_thread = std::thread(&Worker::Run, this);
    }
    ~Worker()
    {
        m_stop.store(true);
        { std::lock_guard<std::mutex> lk(m_mx); m_cv.notify_all(); }
        if (m_thread.joinable())
            m_thread.join();
    }

    Snapshot Get()
    {
        std::lock_guard<std::mutex> lk(m_mx);
        return m_snap;
    }
    void Chdir(const std::string& path)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_pendingCd = path;
        m_cv.notify_all();
    }
    // Local absolute path <-> remote name in the current dir.
    void Download(const std::string& remote, const std::wstring& local)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_xfer = { true, remote, local };
        m_cv.notify_all();
    }
    void Upload(const std::wstring& local, const std::string& remoteName)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_xfer = { false, remoteName, local };
        m_cv.notify_all();
    }

private:
    struct Xfer { bool download = false; std::string remote; std::wstring local; bool valid = false; };

    void SetStatus(const std::string& s, bool busy)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_snap.status = s;
        m_snap.busy = busy;
    }

    void Run()
    {
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        libssh2_init(0);
        SetStatus("connecting to " + m_profile.host + "...", true);

        SOCKET sock = INVALID_SOCKET;
        {
            addrinfo hints = {}, *res = nullptr;
            hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
            char port[16]; snprintf(port, sizeof(port), "%d", m_profile.port);
            if (getaddrinfo(m_profile.host.c_str(), port, &hints, &res) == 0)
            {
                for (addrinfo* ai = res; ai; ai = ai->ai_next)
                {
                    sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                    if (sock == INVALID_SOCKET) continue;
                    if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
                    closesocket(sock); sock = INVALID_SOCKET;
                }
                freeaddrinfo(res);
            }
        }
        if (sock == INVALID_SOCKET) { SetStatus("could not connect", false); return; }

        LIBSSH2_SESSION* session = libssh2_session_init();
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, 20000);
        if (libssh2_session_handshake(session, sock) != 0)
        { SetStatus("SSH handshake failed", false); goto cleanup; }

        // Auth (password / key / agent / keyboard-interactive), best-effort.
        {
            bool authed = false;
            if (m_profile.auth == AuthMethod::Agent)
            {
                if (LIBSSH2_AGENT* ag = libssh2_agent_init(session))
                {
                    if (libssh2_agent_connect(ag) == 0 &&
                        libssh2_agent_list_identities(ag) == 0)
                    {
                        struct libssh2_agent_publickey* id = nullptr, *prev = nullptr;
                        while (!authed &&
                               libssh2_agent_get_identity(ag, &id, prev) == 0)
                        {
                            if (libssh2_agent_userauth(
                                    ag, m_profile.username.c_str(), id) == 0)
                                authed = true;
                            prev = id;
                        }
                    }
                    libssh2_agent_disconnect(ag);
                    libssh2_agent_free(ag);
                }
            }
            else if (m_profile.auth == AuthMethod::PublicKey)
            {
                authed = libssh2_userauth_publickey_fromfile_ex(
                             session, m_profile.username.c_str(),
                             (unsigned)m_profile.username.size(), nullptr,
                             m_profile.privateKeyPath.c_str(),
                             m_pp.empty() ? nullptr : m_pp.c_str()) == 0;
            }
            if (!authed && !m_pw.empty())
                authed = libssh2_userauth_password(
                             session, m_profile.username.c_str(),
                             m_pw.c_str()) == 0;
            if (m_pw.size()) SecureZeroMemory(&m_pw[0], m_pw.size());
            if (m_pp.size()) SecureZeroMemory(&m_pp[0], m_pp.size());
            if (!authed) { SetStatus("authentication failed", false); goto cleanup; }
        }

        m_sftp = libssh2_sftp_init(session);
        if (!m_sftp) { SetStatus("SFTP subsystem unavailable", false); goto cleanup; }

        // Resolve the home directory.
        {
            char home[512] = ".";
            int rl = libssh2_sftp_realpath(m_sftp, ".", home, sizeof(home) - 1);
            m_cwd = (rl > 0) ? std::string(home, rl) : std::string("/");
        }
        List();

        // Command loop.
        while (!m_stop.load())
        {
            std::string cd; Xfer xf;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [&] {
                    return m_stop.load() || !m_pendingCd.empty() || m_xfer.valid;
                });
                cd.swap(m_pendingCd);
                if (m_xfer.valid) { xf = m_xfer; m_xfer = {}; }
            }
            if (m_stop.load()) break;
            if (!cd.empty())
            {
                char real[512];
                std::string target = (cd == "..")
                    ? (m_cwd + "/..") : (cd.front() == '/' ? cd : m_cwd + "/" + cd);
                int rl = libssh2_sftp_realpath(m_sftp, target.c_str(), real,
                                               sizeof(real) - 1);
                if (rl > 0) m_cwd.assign(real, rl);
                List();
            }
            if (xf.valid)
                DoTransfer(xf);
        }

    cleanup:
        if (m_sftp) libssh2_sftp_shutdown(m_sftp);
        if (session)
        {
            libssh2_session_disconnect(session, "bye");
            libssh2_session_free(session);
        }
        closesocket(sock);
    }

    void List()
    {
        SetStatus("listing " + m_cwd + "...", true);
        LIBSSH2_SFTP_HANDLE* h =
            libssh2_sftp_opendir(m_sftp, m_cwd.c_str());
        std::vector<Entry> out;
        if (h)
        {
            char name[512];
            LIBSSH2_SFTP_ATTRIBUTES at;
            int n;
            while ((n = libssh2_sftp_readdir(h, name, sizeof(name), &at)) > 0)
            {
                std::string nm(name, n);
                if (nm == ".") continue;
                Entry e;
                e.name = nm;
                e.dir = LIBSSH2_SFTP_S_ISDIR(at.permissions) != 0;
                e.size = (at.flags & LIBSSH2_SFTP_ATTR_SIZE) ? at.filesize : 0;
                out.push_back(std::move(e));
            }
            libssh2_sftp_closedir(h);
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir > b.dir;
            return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        std::lock_guard<std::mutex> lk(m_mx);
        m_snap.cwd = m_cwd;
        m_snap.entries = std::move(out);
        m_snap.status = std::to_string(m_snap.entries.size()) + " items";
        m_snap.busy = false;
        m_snap.gen++;
    }

    void DoTransfer(const Xfer& xf)
    {
        std::string remotePath = m_cwd + "/" + xf.remote;
        if (xf.download)
        {
            SetStatus("downloading " + xf.remote + "...", true);
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                m_sftp, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
            if (!h) { SetStatus("open failed: " + xf.remote, false); return; }
            FILE* f = _wfopen(xf.local.c_str(), L"wb");
            if (!f) { libssh2_sftp_close(h); SetStatus("cannot write local file", false); return; }
            char buf[32768]; ssize_t n; uint64_t total = 0;
            while ((n = libssh2_sftp_read(h, buf, sizeof(buf))) > 0)
            { fwrite(buf, 1, (size_t)n, f); total += (uint64_t)n; }
            fclose(f); libssh2_sftp_close(h);
            SetStatus("downloaded " + xf.remote + " (" +
                      std::to_string(total) + " bytes)", false);
        }
        else
        {
            SetStatus("uploading " + xf.remote + "...", true);
            FILE* f = _wfopen(xf.local.c_str(), L"rb");
            if (!f) { SetStatus("cannot read local file", false); return; }
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                m_sftp, remotePath.c_str(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                    LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
            if (!h) { fclose(f); SetStatus("remote open failed", false); return; }
            char buf[32768]; size_t r; uint64_t total = 0;
            while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            {
                char* p = buf; size_t left = r;
                while (left > 0)
                {
                    ssize_t w = libssh2_sftp_write(h, p, left);
                    if (w < 0) { left = 0; break; }
                    p += w; left -= (size_t)w; total += (uint64_t)w;
                }
            }
            fclose(f); libssh2_sftp_close(h);
            SetStatus("uploaded " + xf.remote + " (" +
                      std::to_string(total) + " bytes)", false);
            List();
        }
    }

public:
    // generation counter so the UI only rebuilds the list when it changes
    unsigned Gen() { std::lock_guard<std::mutex> lk(m_mx); return m_snap.gen; }

private:
    ConnectionProfile m_profile;
    std::string m_pw, m_pp, m_cwd;
    LIBSSH2_SFTP* m_sftp = nullptr;

    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::string m_pendingCd;
    Xfer m_xfer;
    Snapshot m_snap;
};

// ---- window --------------------------------------------------------------
struct Ui
{
    Worker* worker = nullptr;
    HWND list = nullptr, status = nullptr, path = nullptr;
    HFONT font = nullptr, mono = nullptr;
    HBRUSH bg = nullptr, field = nullptr;
    unsigned seenGen = ~0u;
    std::vector<Entry> entries;
    UINT dpi = 96;
};

enum { IdList = 100, IdDownload, IdUpload, IdUp, IdClose, IdRefresh, IdStatus };

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Ui* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CTLCOLORSTATIC:
    {
        SetBkColor((HDC)wp, kBg);
        SetTextColor((HDC)wp, kDim);
        return (LRESULT)ui->bg;
    }
    case WM_ERASEBKGND:
    {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, ui->bg);
        return 1;
    }
    case WM_TIMER:
    {
        unsigned g = ui->worker->Gen();
        Worker::Snapshot s = ui->worker->Get();
        SetWindowTextW(ui->status, Widen(s.status).c_str());
        if (g != ui->seenGen)
        {
            ui->seenGen = g;
            ui->entries = s.entries;
            SetWindowTextW(ui->path, Widen(s.cwd.empty() ? "/" : s.cwd).c_str());
            SendMessageW(ui->list, LB_RESETCONTENT, 0, 0);
            SendMessageW(ui->list, LB_ADDSTRING, 0, (LPARAM)L"[..]");
            for (const Entry& e : ui->entries)
            {
                std::wstring row = e.dir ? (L"[" + Widen(e.name) + L"]")
                                         : Widen(e.name);
                SendMessageW(ui->list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
            }
        }
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IdList && code == LBN_DBLCLK)
        {
            int sel = (int)SendMessageW(ui->list, LB_GETCURSEL, 0, 0);
            if (sel == 0) { ui->worker->Chdir(".."); return 0; }
            int i = sel - 1;
            if (i >= 0 && i < (int)ui->entries.size() && ui->entries[i].dir)
                ui->worker->Chdir(ui->entries[i].name);
            return 0;
        }
        if (code != BN_CLICKED) return 0;
        switch (id)
        {
        case IdUp: ui->worker->Chdir(".."); return 0;
        case IdRefresh: ui->worker->Chdir("."); return 0;
        case IdClose: DestroyWindow(hwnd); return 0;
        case IdDownload:
        {
            int sel = (int)SendMessageW(ui->list, LB_GETCURSEL, 0, 0);
            int i = sel - 1;
            if (i < 0 || i >= (int)ui->entries.size() || ui->entries[i].dir)
                return 0;
            // The Save dialog is the user's own decision about where this
            // goes, but the name pre-filled into it came from the server.
            // A traversing name is not offered for confirmation at all.
            const amber::NameCheck nc = amber::CheckRemoteName(ui->entries[i].name);
            if (nc != amber::NameCheck::Ok)
            {
                SetWindowTextW(ui->status,
                               (L"Refused \"" + Widen(ui->entries[i].name) + L"\": " +
                                Widen(amber::NameCheckReason(nc))).c_str());
                return 0;
            }
            wchar_t path[MAX_PATH];
            lstrcpynW(path, Widen(ui->entries[i].name).c_str(), MAX_PATH);
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = hwnd; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&ofn))
                ui->worker->Download(ui->entries[i].name, path);
            return 0;
        }
        case IdUpload:
        {
            wchar_t path[MAX_PATH] = L"";
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = hwnd; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn))
            {
                std::wstring w = path;
                size_t slash = w.find_last_of(L"\\/");
                std::string name = Narrow(slash == std::wstring::npos ? w
                                                                      : w.substr(slash + 1));
                ui->worker->Upload(path, name);
            }
            return 0;
        }
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void SftpPanel::Open(HWND owner, const ConnectionProfile& profile,
                     const SecureString& password,
                     const SecureString& passphrase)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    RefreshPalette();

    static bool reg = false;
    static const wchar_t cls[] = L"AmberSSHSftpPanel";
    if (!reg)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        reg = true;
    }

    Worker worker;
    worker.Start(profile, password.Reveal(), passphrase.Reveal());

    Ui ui;
    ui.worker = &worker;
    ui.dpi = owner ? GetDpiForWindow(owner) : 96;
    auto px = [&](int v) { return MulDiv(v, (int)ui.dpi, 96); };
    ui.bg = CreateSolidBrush(kBg);
    ui.field = CreateSolidBrush(kField);
    ui.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    ui.mono = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Cascadia Mono");

    int w = px(560), h = px(560);
    RECT o = {}; if (owner) GetWindowRect(owner, &o);
    int x = o.left + ((o.right - o.left) - w) / 2;
    int y = o.top + ((o.bottom - o.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls,
                               L"AmberSSH — SFTP Browser",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               x, y, w, h, owner, nullptr,
                               GetModuleHandleW(nullptr), &ui);
    if (!dlg) return;

    amber::ApplyWindowChrome(dlg);

    auto mk = [&](const wchar_t* cl, const wchar_t* t, DWORD st, int cx, int cy,
                  int cw, int ch, int id, HFONT f) {
        HWND c = CreateWindowExW(0, cl, t, WS_CHILD | WS_VISIBLE | st,
                                 px(cx), px(cy), px(cw), px(ch), dlg,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                                 nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    };
    ui.path = mk(L"STATIC", L"connecting...", SS_LEFT | SS_PATHELLIPSIS,
                 12, 10, 520, 20, -1, ui.mono);
    ui.list = mk(L"LISTBOX", L"",
                 WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
                 12, 36, 520, 420, IdList, ui.mono);
    mk(L"BUTTON", L"&Up", WS_TABSTOP, 12, 466, 70, 30, IdUp, ui.font);
    mk(L"BUTTON", L"&Refresh", WS_TABSTOP, 88, 466, 84, 30, IdRefresh, ui.font);
    mk(L"BUTTON", L"&Download", WS_TABSTOP, 300, 466, 96, 30, IdDownload, ui.font);
    mk(L"BUTTON", L"U&pload", WS_TABSTOP, 402, 466, 84, 30, IdUpload, ui.font);
    mk(L"BUTTON", L"&Close", WS_TABSTOP, 462, 466, 70, 30, IdClose, ui.font);
    ui.status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS,
                   12, 506, 520, 20, IdStatus, ui.font);

    SetTimer(dlg, 1, 120, nullptr);
    if (owner) EnableWindow(owner, FALSE);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(dlg, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DeleteObject(ui.bg); DeleteObject(ui.field);
    DeleteObject(ui.font); DeleteObject(ui.mono);
}

} // namespace amber
