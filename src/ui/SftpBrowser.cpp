#include "SftpBrowser.h"
#include "SkinDraw.h"
#include "Theme.h"

#include "../platform/Notify.h"
#include "../ssh/SftpClient.h"
#include "../ssh/SyncPlan.h"
#include "../utility/Hash.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace amber
{
namespace
{

// ------------------------------------------------------------ small helpers
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
std::wstring SizeText(uint64_t b)
{
    wchar_t buf[32];
    if (b < 1024) swprintf_s(buf, L"%llu B", (unsigned long long)b);
    else if (b < 1024ull * 1024) swprintf_s(buf, L"%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return buf;
}
std::wstring UnixTimeText(uint64_t t)
{
    if (!t) return L"";
    time_t tt = (time_t)t;
    tm lt;
    if (localtime_s(&lt, &tt) != 0) return L"";
    wchar_t buf[32];
    wcsftime(buf, 32, L"%Y-%m-%d %H:%M", &lt);
    return buf;
}
std::wstring FileTimeText(const FILETIME& ft)
{
    FILETIME lf; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lf) || !FileTimeToSystemTime(&lf, &st)) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute);
    return buf;
}
std::wstring LocalJoin(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}
std::wstring LocalParent(const std::wstring& p)
{
    // "C:\" -> "" (drive list); "C:\a\b" -> "C:\a"; "C:\a" -> "C:\".
    if (p.size() <= 3) return L"";
    size_t end = p.size();
    if (p[end - 1] == L'\\') --end;
    size_t s = p.find_last_of(L'\\', end - 1);
    if (s == std::wstring::npos) return L"";
    if (s == 2) return p.substr(0, 3);
    return p.substr(0, s);
}

// Hover tracking for the owner-drawn buttons: a push button gets no hover
// state of its own once it is owner-drawn, so the skin has nothing to react to
// without this.
constexpr wchar_t kHoverProp[] = L"amberSftpHover";

LRESULT CALLBACK ButtonHoverProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                 DWORD_PTR)
{
    switch (m)
    {
    case BM_SETSTYLE:
        // IsDialogMessage shuffles BS_DEFPUSHBUTTON between buttons as focus
        // moves, which would overwrite BS_OWNERDRAW and snap one back to the
        // stock theme. The skin is authoritative.
        return 0;
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kHoverProp))
        {
            SetPropW(h, kHoverProp, (HANDLE)1);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kHoverProp);
        InvalidateRect(h, nullptr, TRUE);
        break;
    case WM_NCDESTROY:
        RemovePropW(h, kHoverProp);
        RemoveWindowSubclass(h, ButtonHoverProc, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// Fills the header strip (including the gap past the last column) with the
// skin's panel colour; the item cells themselves are drawn by the list view.
LRESULT CALLBACK HeaderBackProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                DWORD_PTR)
{
    if (m == WM_ERASEBKGND)
    {
        RECT rc;
        GetClientRect(h, &rc);
        DialogPalette pal = MakeDialogPalette();
        HBRUSH b = CreateSolidBrush(amber::skin::LightGround()
                                        ? amber::skin::Dim(pal.bg, 0.97f)
                                        : amber::skin::Dim(pal.field, 1.35f));
        FillRect((HDC)w, &rc, b);
        DeleteObject(b);
        return 1;
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, HeaderBackProc, 1);
    return DefSubclassProc(h, m, w, l);
}

// A tab control paints its own strip behind the items, and that strip is
// stock white. Owner-drawing the items alone is not enough.
LRESULT CALLBACK TabBackProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                             DWORD_PTR)
{
    if (m == WM_ERASEBKGND)
    {
        RECT rc;
        GetClientRect(h, &rc);
        DialogPalette pal = MakeDialogPalette();
        HBRUSH b = CreateSolidBrush(pal.bg);
        FillRect((HDC)w, &rc, b);
        DeleteObject(b);
        return 1;
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, TabBackProc, 1);
    return DefSubclassProc(h, m, w, l);
}

// Owner-drawn header cells arrive at the LIST VIEW, not at the header, so the
// list view is the thing that has to be subclassed to paint them.
LRESULT CALLBACK ListHeaderDrawProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                    DWORD_PTR)
{
    if (m == WM_DRAWITEM)
    {
        auto* dis = (DRAWITEMSTRUCT*)l;
        if (dis->CtlType == ODT_HEADER)
        {
            DialogPalette pal = MakeDialogPalette();
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            HBRUSH b = CreateSolidBrush(amber::skin::LightGround()
                                            ? amber::skin::Dim(pal.bg, 0.97f)
                                            : amber::skin::Dim(pal.field, 1.35f));
            FillRect(dc, &rc, b);
            DeleteObject(b);
            // A single separator on the right edge, in the skin's border
            // colour — enough structure without a 3D bevel.
            RECT sep = { rc.right - 1, rc.top + 2, rc.right, rc.bottom - 2 };
            HBRUSH sb = CreateSolidBrush(pal.border);
            FillRect(dc, &sep, sb);
            DeleteObject(sb);

            wchar_t buf[128] = L"";
            HDITEMW hi = { HDI_TEXT };
            hi.pszText = buf;
            hi.cchTextMax = 128;
            Header_GetItem(dis->hwndItem, dis->itemID, &hi);
            std::wstring label = amber::skin::Label(buf);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, pal.textDim);
            HFONT f = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
            HGDIOBJ of = f ? SelectObject(dc, f) : nullptr;
            RECT tr = rc;
            tr.left += 6;
            tr.right -= 6;
            DrawTextW(dc, label.c_str(), -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
            if (of)
                SelectObject(dc, of);
            return TRUE;
        }
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, ListHeaderDrawProc, 1);
    return DefSubclassProc(h, m, w, l);
}

// Minimal themed prompt (new folder name, permissions, rename...).
bool PromptString(HWND owner, const wchar_t* title, const wchar_t* label,
                  std::wstring& io)
{
    struct P { std::wstring* io; bool ok; HWND edit; HBRUSH bg; HFONT font; };
    static bool reg = false;
    static const wchar_t cls[] = L"AmberSSHSftpPrompt";
    auto proc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        P* p = (P*)GetWindowLongPtrW(h, GWLP_USERDATA);
        DialogPalette pal = MakeDialogPalette();
        switch (m)
        {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            return DefWindowProcW(h, m, w, l);
        case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); FillRect((HDC)w, &rc, p->bg); return 1; }
        case WM_CTLCOLORSTATIC: SetBkColor((HDC)w, pal.bg); SetTextColor((HDC)w, pal.textDim); return (LRESULT)p->bg;
        case WM_CTLCOLOREDIT: SetBkColor((HDC)w, pal.field); SetTextColor((HDC)w, pal.text); return (LRESULT)GetStockObject(DC_BRUSH);
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) { wchar_t b[1024]; GetWindowTextW(p->edit, b, 1024); *p->io = b; p->ok = true; DestroyWindow(h); }
            else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    if (!reg)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        reg = true;
    }
    DialogPalette pal = MakeDialogPalette();
    P p{ &io, false, nullptr, CreateSolidBrush(pal.bg), nullptr };
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    auto px = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    p.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT o = {}; if (owner) GetWindowRect(owner, &o);
    int w = px(420), h = px(150);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls, title,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               o.left + ((o.right - o.left) - w) / 2,
                               o.top + ((o.bottom - o.top) - h) / 2, w, h, owner,
                               nullptr, GetModuleHandleW(nullptr), &p);
    ApplyWindowChrome(dlg);
    auto mk = [&](const wchar_t* c, const wchar_t* t, DWORD st, int x, int y, int cw, int ch, int id) {
        HWND hc = CreateWindowExW(0, c, t, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cw), px(ch),
                                  dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hc, WM_SETFONT, (WPARAM)p.font, TRUE);
        return hc;
    };
    mk(L"STATIC", label, SS_LEFT, 14, 12, 380, 20, -1);
    p.edit = mk(L"EDIT", io.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 36, 380, 26, 100);
    mk(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 220, 74, 84, 28, IDOK);
    mk(L"BUTTON", L"Cancel", WS_TABSTOP, 310, 74, 84, 28, IDCANCEL);
    SetFocus(p.edit);
    SendMessageW(p.edit, EM_SETSEL, 0, -1);
    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DeleteObject(p.bg);
    DeleteObject(p.font);
    return p.ok;
}

// ------------------------------------------------------------ transfers
struct Transfer
{
    uint64_t id = 0;
    bool download = false;
    std::wstring local;
    std::string remote;
    std::wstring name;
    std::atomic<uint64_t> done{ 0 }, total{ 0 };
    // Kept as the original small ints because the queue list and the taskbar
    // read them from the UI thread while the worker writes them; the values
    // map onto amber::XferState, which is what the new columns display.
    // 0 queued 1 running 2 done 3 failed 4 cancelled 5 verifying 6 paused
    std::atomic<int> state{ 0 };
    std::atomic<bool> cancel{ false };
    std::string error;
    bool refreshLocal = false, refreshRemote = false;

    // ---- Stage 6 ---------------------------------------------------------
    // Rate and ETA are derived from these rather than stored, so a stalled
    // transfer's numbers decay instead of freezing at their last good value.
    std::atomic<double> startedAt{ 0.0 };
    std::atomic<uint64_t> resumeFrom{ 0 };
    std::string resumeWhy;               // why it resumed, or why it did not
    std::atomic<int> verify{ 0 };        // amber::VerifyState
    std::string verifyDetail;            // the digests, or why there is none
    std::atomic<int> attempts{ 0 };
    bool verifyRequested = false;
    // Recorded when a partial is first written, and the whole basis on which
    // a later resume is allowed (see SyncPlan.h, DecideResume).
    uint64_t srcSizeAtStart = 0;
    int64_t srcMtimeAtStart = 0;
    bool haveSrcRecord = false;
    // Metadata to put back on the destination after the bytes land.
    int64_t preserveMtime = 0;
    uint32_t preserveMode = 0;

    amber::XferState State() const
    {
        switch (state.load())
        {
        case 1:  return amber::XferState::Running;
        case 2:  return amber::XferState::Done;
        case 3:  return amber::XferState::Failed;
        case 4:  return amber::XferState::Cancelled;
        case 5:  return amber::XferState::Verifying;
        case 6:  return amber::XferState::Paused;
        default: return amber::XferState::Queued;
        }
    }
    amber::VerifyState Verify() const
    {
        return static_cast<amber::VerifyState>(verify.load());
    }
    // Only true when any REQUIRED verification actually passed.
    bool Succeeded() const
    {
        return amber::TransferSucceeded(State(), Verify(), verifyRequested);
    }
};

struct LocalEntry
{
    std::wstring name;
    bool dir = false;
    uint64_t size = 0;
    FILETIME mtime = {};
};

struct EditWatch
{
    std::string remote;
    std::wstring local;
    FILETIME mtime = {};
};

// ------------------------------------------------------------ worker
class Worker
{
public:
    struct Snapshot
    {
        std::string cwd;
        std::vector<SftpEntry> entries;
        std::string status;
        bool connected = false;
        bool busy = true;
        unsigned gen = 0;
    };
    using Job = std::function<void(SftpClient&)>;

    void Start(const ConnectionProfile& p, std::string pw, std::string pp,
               std::string initialDir)
    {
        m_profile = p; m_pw = std::move(pw); m_pp = std::move(pp);
        m_initial = std::move(initialDir);
        m_thread = std::thread(&Worker::Run, this);
    }
    ~Worker()
    {
        { std::lock_guard<std::mutex> lk(m_mx); m_stop = true; m_cv.notify_all(); }
        if (m_thread.joinable()) m_thread.join();
    }
    void Post(Job job)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_jobs.push_back(std::move(job));
        m_cv.notify_all();
    }
    Snapshot Get() { std::lock_guard<std::mutex> lk(m_mx); return m_snap; }
    unsigned Gen() { std::lock_guard<std::mutex> lk(m_mx); return m_snap.gen; }
    void SetStatus(const std::string& s, bool busy)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_snap.status = s; m_snap.busy = busy;
    }
    void Publish(const std::string& cwd, std::vector<SftpEntry> entries)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_snap.cwd = cwd; m_snap.entries = std::move(entries);
        m_snap.status = std::to_string(m_snap.entries.size()) + " items";
        m_snap.busy = false; m_snap.gen++;
    }
    // Lists `dir` (realpath'd) and publishes; on failure keeps the old view.
    void ListDir(SftpClient& c, const std::string& dir)
    {
        SetStatus("listing " + dir + "...", true);
        std::string real = c.Realpath(dir);
        std::vector<SftpEntry> e; std::string err;
        if (c.List(real, e, err)) Publish(real, std::move(e));
        else SetStatus(err, false);
    }
    const std::string& Cwd() { std::lock_guard<std::mutex> lk(m_mx); return m_snap.cwd; }

private:
    void Run()
    {
        SetStatus("connecting to " + m_profile.host + "...", true);
        std::string err;
        bool ok = m_client.Connect(m_profile, m_pw, m_pp, err);
        if (!m_pw.empty()) SecureZeroMemory(&m_pw[0], m_pw.size());
        if (!m_pp.empty()) SecureZeroMemory(&m_pp[0], m_pp.size());
        if (!ok) { SetStatus(err, false); return; }
        { std::lock_guard<std::mutex> lk(m_mx); m_snap.connected = true; }
        ListDir(m_client, m_initial.empty() ? "." : m_initial);
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [&] { return m_stop || !m_jobs.empty(); });
                if (m_stop) break;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            job(m_client);
        }
        m_client.Close();
    }

    ConnectionProfile m_profile;
    std::string m_pw, m_pp, m_initial;
    SftpClient m_client;
    std::thread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::deque<Job> m_jobs;
    Snapshot m_snap;
};

// ------------------------------------------------------------ tab
struct Tab
{
    ConnectionProfile profile;
    std::unique_ptr<Worker> worker;
    std::string rcwd;
    std::vector<SftpEntry> rentries;
    unsigned rgenSeen = ~0u;
    std::wstring lcwd;
    std::vector<LocalEntry> lentries;
    std::mutex xmx;
    std::vector<std::shared_ptr<Transfer>> xfers;
    std::vector<EditWatch> edits;
    int rsortCol = 0, lsortCol = 0;
    bool rsortAsc = true, lsortAsc = true;
    std::string pendingOpen;       // remote file to open once connected
    uint64_t nextXfer = 1;
    bool connected = false;
    // ---- Stage 6 ---------------------------------------------------------
    // Queue-wide switches. Atomic because the worker thread reads them in the
    // middle of a transfer to decide whether to stop.
    std::atomic<bool> paused{ false };
    bool verifyTransfers = false;      // ask for a SHA-256 check on each one
    bool preserveMeta = true;          // timestamps, and mode where it maps
    amber::RetryPolicy retry;
    // Comparison / sync settings. The direction is explicit and deletion is
    // off, every time: a compare must never become a mirror by default.
    amber::SyncDirection syncDir = amber::SyncDirection::LocalToRemote;
    bool syncDelete = false;
    std::vector<std::string> excludes{ ".git/", "node_modules/", "*.tmp",
                                       "Thumbs.db", ".DS_Store" };
};

static constexpr UINT WM_APP_COMPARE_DONE = WM_APP + 21;

enum
{
    IdTabs = 200, IdLPath, IdLList, IdRPath, IdRList, IdQueue, IdStatus,
    IdBtnUpload, IdBtnDownload, IdBtnRefresh, IdBtnMkdir, IdBtnRename,
    IdBtnDelete, IdBtnOpen, IdBtnNewTab, IdBtnCloseTab, IdBtnCancel, IdBtnPerms,
    IdBtnClearQueue, IdBtnPauseQueue, IdBtnRetryFailed, IdBtnVerify, IdBtnCompare,
    IdBtnSyncDir, IdBtnExcludes, IdBtnSyncDelete,
    // context menu
    IdCtxOpen = 300, IdCtxCopy, IdCtxRename, IdCtxDelete, IdCtxMkdir, IdCtxPerms,
    IdCtxRefresh, IdCtxCopyPath,
};

// ------------------------------------------------------------ browser
class Browser
{
public:
    static Browser& Get()
    {
        static Browser b;
        return b;
    }
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND tabs = nullptr, lpath = nullptr, llist = nullptr, rpath = nullptr, rlist = nullptr;
    HWND queue = nullptr, status = nullptr;
    std::vector<HWND> buttons;
    HFONT font = nullptr, mono = nullptr;
    // Small dim face for Solder Mask silkscreen reference designators.
    HFONT silk = nullptr;
    HBRUSH bgBrush = nullptr, fieldBrush = nullptr;
    HIMAGELIST icons = nullptr;
    UINT dpi = 96;
    std::vector<std::unique_ptr<Tab>> tabList;
    int active = -1;
    unsigned queueSeen = 0;
    double lastEditPoll = 0;
    SftpBrowser::NewTabFn newTabFn = nullptr;
    void* newTabCtx = nullptr;

    void EnsureWindow(HWND own);
    int AddTab(const ConnectionProfile& p, const std::string& pw, const std::string& pp,
               const std::string& initialDir);
    Tab* Active() { return (active >= 0 && active < (int)tabList.size()) ? tabList[(size_t)active].get() : nullptr; }
    void Layout();
    void SwitchTab(int i);
    void CloseTab(int i);
    void RefreshLocal(Tab& t);
    void FillLocal(Tab& t);
    void FillRemote(Tab& t);
    void FillQueue(Tab& t);
    void Poll();
    void NavigateRemote(Tab& t, const std::string& path);
    void NavigateLocal(Tab& t, const std::wstring& path);
    void QueueDownload(Tab& t, const SftpEntry& e, const std::wstring& localDir);
    void QueueUpload(Tab& t, const std::wstring& localPath, const std::string& remoteDir);
    // ---- Stage 6 ---------------------------------------------------------
    // One file with both paths given. The existing two enqueue by directory
    // and recurse, which is right for a drag or a button; a sync plan has
    // already decided every exact path and must not re-walk anything.
    void QueuePair(Tab& t, bool download, const std::wstring& local,
                   const std::string& remote, uint64_t bytes);
    void CompareFolders(Tab& t);
    void ShowPlan(Tab& t, const amber::SyncPlan& plan, bool truncated);
    void EditExcludes(Tab& t);
    void RunTransfer(Tab& t, std::shared_ptr<Transfer> x);
    void OpenRemote(Tab& t, const SftpEntry& e);
    void CopySelection(bool fromRemote);
    void DeleteSelection(bool remote);
    void RenameSelected(bool remote);
    void NewFolder(bool remote);
    void Permissions();
    void ContextMenu(bool remote, POINT pt);
    void SetStatusText(const std::wstring& s) { SetWindowTextW(status, s.c_str()); }
    std::vector<int> Selected(HWND lv)
    {
        std::vector<int> v;
        int i = -1;
        while ((i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) >= 0) v.push_back(i);
        return v;
    }
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK Thunk(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        return Get().Proc(h, m, w, l);
    }
};

int Px(int v) { return MulDiv(v, (int)Browser::Get().dpi, 96); }

// Path edits: Enter navigates.
LRESULT CALLBACK PathEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_KEYDOWN && w == VK_RETURN)
    {
        Browser& b = Browser::Get();
        wchar_t buf[2048];
        GetWindowTextW(h, buf, 2048);
        if (Tab* t = b.Active())
        {
            if (h == b.rpath) b.NavigateRemote(*t, Narrow(buf));
            else b.NavigateLocal(*t, buf);
        }
        return 0;
    }
    if (m == WM_CHAR && w == VK_RETURN) return 0;
    return DefSubclassProc(h, m, w, l);
}

void Browser::EnsureWindow(HWND own)
{
    if (hwnd) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); return; }
    owner = own;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    static bool reg = false;
    static const wchar_t cls[] = L"AmberSSHSftpBrowser";
    if (!reg)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Thunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = cls;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        reg = true;
    }
    DialogPalette pal = MakeDialogPalette();
    dpi = own ? GetDpiForWindow(own) : 96;
    bgBrush = CreateSolidBrush(pal.bg);
    fieldBrush = amber::skin::Stitch()
        ? amber::skin::LiningBrush(dpi, pal.field, RGB(0x33, 0x2E, 0x38))
        : CreateSolidBrush(pal.field);
    // Typography follows the skin: Consolas for the ship computer, Georgia for
    // the brass panel, Arial for Swiss. File listings stay monospaced, because
    // aligned columns of names and sizes matter more than the skin's face.
    font = amber::skin::MakeUiFont(dpi, 15);
    mono = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
    silk = amber::skin::MakeUiFont(dpi, 11);

    int w = Px(1180), h = Px(720);
    RECT o = {}; if (own) GetWindowRect(own, &o);
    hwnd = CreateWindowExW(0, cls, L"AmberSSH — SFTP",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           o.left + 40, o.top + 40, w, h, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    ApplyWindowChrome(hwnd);

    auto mk = [&](const wchar_t* c, const wchar_t* t, DWORD st, int id, HFONT f, DWORD ex = 0) {
        HWND hc = CreateWindowExW(ex, c, t, WS_CHILD | WS_VISIBLE | st, 0, 0, 10, 10, hwnd,
                                  (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hc, WM_SETFONT, (WPARAM)f, TRUE);
        return hc;
    };
    tabs = mk(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_TABS | TCS_FOCUSNEVER, IdTabs, font);
    lpath = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IdLPath, mono);
    rpath = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IdRPath, mono);
    SetWindowSubclass(lpath, PathEditProc, 1, 0);
    SetWindowSubclass(rpath, PathEditProc, 2, 0);
    // No WS_EX_CLIENTEDGE: that is a stock sunken 3D border which belongs to
    // no skin. The panes are outlined by the skin in WM_PAINT instead.
    DWORD lvStyle = WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_SHAREIMAGELISTS;
    llist = mk(WC_LISTVIEWW, L"", lvStyle, IdLList, mono);
    rlist = mk(WC_LISTVIEWW, L"", lvStyle, IdRList, mono);
    queue = mk(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
               IdQueue, mono);
    status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IdStatus, font);

    struct B { const wchar_t* t; int id; };
    static const B kButtons[] = {
        { L"\u2190 Download  F5", IdBtnDownload }, { L"Upload \u2192  F5", IdBtnUpload },
        { L"Open/Edit  F4", IdBtnOpen }, { L"Rename  F2", IdBtnRename },
        { L"New Folder  F7", IdBtnMkdir }, { L"Delete  F8", IdBtnDelete },
        { L"Permissions", IdBtnPerms }, { L"Refresh  Ctrl+R", IdBtnRefresh },
        { L"New Tab  Ctrl+T", IdBtnNewTab }, { L"Close Tab  Ctrl+W", IdBtnCloseTab },
        { L"Cancel Transfer", IdBtnCancel }, { L"Clear Done", IdBtnClearQueue },
        { L"Pause Queue", IdBtnPauseQueue }, { L"Retry Failed", IdBtnRetryFailed },
        { L"Verify: off", IdBtnVerify }, { L"Compare Folders", IdBtnCompare },
        { L"Sync: local → remote", IdBtnSyncDir },
        { L"Exclusions...", IdBtnExcludes },
        { L"Delete extraneous: no", IdBtnSyncDelete },
    };
    // Owner-drawn so every skin's shape language reaches them; hover is
    // tracked by a subclass, as in the connection manager.
    for (const B& b : kButtons)
    {
        HWND btn = mk(L"BUTTON", b.t, WS_TABSTOP | BS_OWNERDRAW, b.id, font);
        SetWindowSubclass(btn, ButtonHoverProc, 1, 0);
        buttons.push_back(btn);
    }
    // The tab strip is owner-drawn for the same reason.
    SetWindowLongPtrW(tabs, GWL_STYLE,
                      GetWindowLongPtrW(tabs, GWL_STYLE) | TCS_OWNERDRAWFIXED);
    SetWindowTheme(tabs, L"", L"");        // drop the stock visual style
    SetWindowSubclass(tabs, TabBackProc, 1, 0);

    for (HWND lv : { llist, rlist, queue })
    {
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_HEADERDRAGDROP);
        ListView_SetBkColor(lv, pal.field);
        ListView_SetTextBkColor(lv, pal.field);
        ListView_SetTextColor(lv, pal.text);
        // A light skin must NOT get the dark-mode visual style, or the rows
        // come out white on white.
        SetWindowTheme(lv, amber::skin::ListTheme(), nullptr);
        SetWindowTheme(ListView_GetHeader(lv), amber::skin::HeaderTheme(), nullptr);
    }
    // Stock folder / document icons.
    icons = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                             ILC_COLOR32 | ILC_MASK, 2, 2);
    for (SHSTOCKICONID sid : { SIID_FOLDER, SIID_DOCNOASSOC })
    {
        SHSTOCKICONINFO sii = { sizeof(sii) };
        if (SUCCEEDED(SHGetStockIconInfo(sid, SHGSI_ICON | SHGSI_SMALLICON, &sii)))
        {
            ImageList_AddIcon(icons, sii.hIcon);
            DestroyIcon(sii.hIcon);
        }
    }
    ListView_SetImageList(llist, icons, LVSIL_SMALL);
    ListView_SetImageList(rlist, icons, LVSIL_SMALL);

    auto col = [&](HWND lv, int i, const wchar_t* t, int w, int fmt = LVCFMT_LEFT) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
        c.pszText = const_cast<wchar_t*>(t); c.cx = Px(w); c.fmt = fmt;
        ListView_InsertColumn(lv, i, &c);
    };
    col(llist, 0, L"Name", 260); col(llist, 1, L"Size", 90, LVCFMT_RIGHT); col(llist, 2, L"Modified", 130);
    col(rlist, 0, L"Name", 260); col(rlist, 1, L"Size", 90, LVCFMT_RIGHT); col(rlist, 2, L"Modified", 130);
    col(rlist, 3, L"Permissions", 100); col(rlist, 4, L"Owner", 70);
    col(queue, 0, L"Transfer", 300); col(queue, 1, L"Dir", 40); col(queue, 2, L"Size", 90, LVCFMT_RIGHT);
    col(queue, 3, L"Progress", 80, LVCFMT_RIGHT);
    col(queue, 4, L"Speed", 90, LVCFMT_RIGHT); col(queue, 5, L"ETA", 70, LVCFMT_RIGHT);
    col(queue, 6, L"Status", 340);

    // Column headers are the one part of a list view the visual style paints
    // itself, and it paints them light. Make them owner-drawn so they follow
    // the skin: the item cells come to the LIST VIEW as WM_DRAWITEM, and the
    // strip either side of them is filled by the header's own subclass.
    for (HWND lv : { llist, rlist, queue })
    {
        HWND hdr = ListView_GetHeader(lv);
        const int n = Header_GetItemCount(hdr);
        for (int i = 0; i < n; ++i)
        {
            HDITEMW hi = { HDI_FORMAT };
            Header_GetItem(hdr, i, &hi);
            hi.fmt |= HDF_OWNERDRAW;
            Header_SetItem(hdr, i, &hi);
        }
        SetWindowSubclass(hdr, HeaderBackProc, 1, 0);
        SetWindowSubclass(lv, ListHeaderDrawProc, 1, 0);
    }

    DragAcceptFiles(hwnd, TRUE);
    SetTimer(hwnd, 1, 100, nullptr);
    Layout();
    ShowWindow(hwnd, SW_SHOW);
}

void Browser::Layout()
{
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = Px(8), tabH = Px(30), pathH = Px(26), btnH = Px(28), qH = Px(150), stH = Px(20);
    int y = m;
    MoveWindow(tabs, m, y, W - 2 * m, tabH, TRUE); y += tabH + Px(4);
    // Toolbar (two rows if narrow).
    // Each button is sized to the label AS IT WILL BE DRAWN, in the skin's own
    // face and case. A fixed width was fine for mixed-case Segoe UI and clips
    // the moment a skin letters in wider uppercase.
    int bx = m, gap = Px(6);
    int rowY = y;
    HDC mdc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(mdc, font);
    for (HWND b : buttons)
    {
        wchar_t raw[128] = L"";
        GetWindowTextW(b, raw, 128);
        int bw = amber::skin::LabelWidth(mdc, raw) + Px(26);
        bw = (std::max)(bw, Px(70));
        if (bx + bw > W - m) { bx = m; rowY += btnH + Px(4); }
        MoveWindow(b, bx, rowY, bw, btnH, TRUE);
        bx += bw + gap;
    }
    SelectObject(mdc, oldFont);
    ReleaseDC(hwnd, mdc);
    y = rowY + btnH + Px(8);
    int paneW = (W - 3 * m) / 2;
    int listTop = y + pathH + Px(4);
    int listH = H - listTop - qH - stH - 3 * m;
    if (listH < Px(80)) listH = Px(80);
    MoveWindow(lpath, m, y, paneW, pathH, TRUE);
    MoveWindow(rpath, 2 * m + paneW, y, paneW, pathH, TRUE);
    MoveWindow(llist, m, listTop, paneW, listH, TRUE);
    MoveWindow(rlist, 2 * m + paneW, listTop, paneW, listH, TRUE);
    int qy = listTop + listH + m;
    MoveWindow(queue, m, qy, W - 2 * m, qH, TRUE);
    MoveWindow(status, m, qy + qH + Px(4), W - 2 * m, stH, TRUE);
}

int Browser::AddTab(const ConnectionProfile& p, const std::string& pw, const std::string& pp,
                    const std::string& initialDir)
{
    auto t = std::make_unique<Tab>();
    t->profile = p;
    t->worker = std::make_unique<Worker>();
    t->worker->Start(p, pw, pp, initialDir);
    wchar_t home[MAX_PATH];
    t->lcwd = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home)) ? home : L"C:\\";
    tabList.push_back(std::move(t));
    int idx = (int)tabList.size() - 1;
    TCITEMW ti = { TCIF_TEXT };
    std::wstring label = Widen(p.name.empty() ? (p.username + "@" + p.host) : p.name);
    ti.pszText = const_cast<wchar_t*>(label.c_str());
    TabCtrl_InsertItem(tabs, idx, &ti);
    SwitchTab(idx);
    return idx;
}

void Browser::SwitchTab(int i)
{
    if (i < 0 || i >= (int)tabList.size()) return;
    active = i;
    TabCtrl_SetCurSel(tabs, i);
    Tab& t = *tabList[(size_t)i];
    t.rgenSeen = ~0u;
    RefreshLocal(t);
    FillRemote(t);
    FillQueue(t);
    SetWindowTextW(hwnd, (L"AmberSSH — SFTP — " + Widen(t.profile.host)).c_str());
}

void Browser::CloseTab(int i)
{
    if (i < 0 || i >= (int)tabList.size()) return;
    tabList.erase(tabList.begin() + i);
    TabCtrl_DeleteItem(tabs, i);
    if (tabList.empty())
    {
        active = -1;
        ListView_DeleteAllItems(llist);
        ListView_DeleteAllItems(rlist);
        ListView_DeleteAllItems(queue);
        SetWindowTextW(rpath, L"");
        SetStatusText(L"No connections — Ctrl+T opens one.");
        return;
    }
    SwitchTab(std::min(i, (int)tabList.size() - 1));
}

void Browser::RefreshLocal(Tab& t)
{
    t.lentries.clear();
    if (t.lcwd.empty())
    {
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d)
            if (drives & (1u << d))
            {
                LocalEntry e; e.name = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\"; e.dir = true;
                t.lentries.push_back(e);
            }
    }
    else
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(LocalJoin(t.lcwd, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                LocalEntry e;
                e.name = fd.cFileName;
                e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                e.mtime = fd.ftLastWriteTime;
                t.lentries.push_back(e);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    FillLocal(t);
}

template <class E, class Key>
void SortEntries(std::vector<E>& v, int col, bool asc, Key key)
{
    std::stable_sort(v.begin(), v.end(), [&](const E& a, const E& b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        int c = key(a, b, col);
        return asc ? c < 0 : c > 0;
    });
}

void Browser::FillLocal(Tab& t)
{
    SortEntries(t.lentries, t.lsortCol, t.lsortAsc, [](const LocalEntry& a, const LocalEntry& b, int col) -> int {
        if (col == 1) return a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        if (col == 2) return static_cast<int>(CompareFileTime(&a.mtime, &b.mtime));
        return _wcsicmp(a.name.c_str(), b.name.c_str());
    });
    SendMessageW(llist, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(llist);
    int idx = 0;
    if (!t.lcwd.empty())
    {
        LVITEMW it = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
        it.iItem = idx++; it.pszText = const_cast<wchar_t*>(L".."); it.iImage = 0; it.lParam = -1;
        ListView_InsertItem(llist, &it);
    }
    for (size_t i = 0; i < t.lentries.size(); ++i)
    {
        const LocalEntry& e = t.lentries[i];
        LVITEMW it = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
        it.iItem = idx++; it.pszText = const_cast<wchar_t*>(e.name.c_str());
        it.iImage = e.dir ? 0 : 1; it.lParam = (LPARAM)i;
        int row = ListView_InsertItem(llist, &it);
        std::wstring sz = e.dir ? L"" : SizeText(e.size), mt = FileTimeText(e.mtime);
        ListView_SetItemText(llist, row, 1, const_cast<wchar_t*>(sz.c_str()));
        ListView_SetItemText(llist, row, 2, const_cast<wchar_t*>(mt.c_str()));
    }
    SendMessageW(llist, WM_SETREDRAW, TRUE, 0);
    SetWindowTextW(lpath, t.lcwd.empty() ? L"(drives)" : t.lcwd.c_str());
}

void Browser::FillRemote(Tab& t)
{
    SortEntries(t.rentries, t.rsortCol, t.rsortAsc, [](const SftpEntry& a, const SftpEntry& b, int col) -> int {
        if (col == 1) return a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        if (col == 2) return a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0);
        return _stricmp(a.name.c_str(), b.name.c_str());
    });
    SendMessageW(rlist, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(rlist);
    int idx = 0;
    if (t.rcwd != "/" && !t.rcwd.empty())
    {
        LVITEMW it = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
        it.iItem = idx++; it.pszText = const_cast<wchar_t*>(L".."); it.iImage = 0; it.lParam = -1;
        ListView_InsertItem(rlist, &it);
    }
    for (size_t i = 0; i < t.rentries.size(); ++i)
    {
        const SftpEntry& e = t.rentries[i];
        std::wstring name = Widen(e.name);
        if (e.link) name += L" \u2192";
        LVITEMW it = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
        it.iItem = idx++; it.pszText = const_cast<wchar_t*>(name.c_str());
        it.iImage = e.dir ? 0 : 1; it.lParam = (LPARAM)i;
        int row = ListView_InsertItem(rlist, &it);
        std::wstring sz = e.dir ? L"" : SizeText(e.size), mt = UnixTimeText(e.mtime);
        std::wstring pm = Widen(SftpPermString(e.perms, e.dir, e.link));
        std::wstring ow = std::to_wstring(e.uid) + L":" + std::to_wstring(e.gid);
        ListView_SetItemText(rlist, row, 1, const_cast<wchar_t*>(sz.c_str()));
        ListView_SetItemText(rlist, row, 2, const_cast<wchar_t*>(mt.c_str()));
        ListView_SetItemText(rlist, row, 3, const_cast<wchar_t*>(pm.c_str()));
        ListView_SetItemText(rlist, row, 4, const_cast<wchar_t*>(ow.c_str()));
    }
    SendMessageW(rlist, WM_SETREDRAW, TRUE, 0);
    SetWindowTextW(rpath, Widen(t.rcwd.empty() ? "(connecting)" : t.rcwd).c_str());
}

void Browser::FillQueue(Tab& t)
{
    std::lock_guard<std::mutex> lk(t.xmx);
    int n = (int)t.xfers.size();
    while (ListView_GetItemCount(queue) > n) ListView_DeleteItem(queue, ListView_GetItemCount(queue) - 1);
    while (ListView_GetItemCount(queue) < n)
    {
        LVITEMW it = { LVIF_TEXT }; it.iItem = ListView_GetItemCount(queue);
        it.pszText = const_cast<wchar_t*>(L""); ListView_InsertItem(queue, &it);
    }
    const double now = static_cast<double>(GetTickCount64()) / 1000.0;
    for (int i = 0; i < n; ++i)
    {
        Transfer& x = *t.xfers[(size_t)i];
        const uint64_t done = x.done.load(), total = x.total.load();
        const int st = x.state.load();
        wchar_t pct[16];
        if (total > 0) swprintf_s(pct, L"%3.0f%%", 100.0 * (double)done / (double)total);
        else swprintf_s(pct, st == 2 ? L"100%%" : L"--");

        // The state column carries everything the user needs to judge the
        // item: what it is doing, whether verification passed, how many
        // attempts it took, and the reason it resumed or restarted.
        std::wstring stText = Widen(amber::XferStateName(x.State()));
        if (x.verifyRequested && x.Verify() != amber::VerifyState::NotRequested)
            stText += L" \u00b7 " + Widen(amber::VerifyStateName(x.Verify()));
        const int tries = x.attempts.load();
        if (tries > 1)
            stText += L" \u00b7 try " + std::to_wstring(tries);
        if (st == 3 && !x.error.empty())
            stText += L": " + Widen(x.error);
        else if (x.Verify() == amber::VerifyState::Failed)
            stText += L": " + Widen(x.verifyDetail);
        else if (x.resumeFrom.load() > 0)
            stText += L" \u00b7 " + Widen(x.resumeWhy);

        // Speed and ETA, derived rather than stored so a stalled transfer's
        // numbers decay instead of freezing at their last good reading.
        std::wstring rate = L"-", eta = L"-";
        if (st == 1)
        {
            const double started = x.startedAt.load();
            const double elapsed = started > 0.0 ? now - started : 0.0;
            const uint64_t moved = done > x.resumeFrom.load()
                                       ? done - x.resumeFrom.load()
                                       : 0;
            rate = Widen(amber::FormatRate(amber::TransferRate(moved, elapsed)));
            eta = Widen(amber::FormatEta(amber::TransferEta(done, total, elapsed)));
        }
        const std::wstring sz = SizeText(total ? total : done);
        ListView_SetItemText(queue, i, 0, const_cast<wchar_t*>(x.name.c_str()));
        ListView_SetItemText(queue, i, 1, const_cast<wchar_t*>(x.download ? L"\u2190" : L"\u2192"));
        ListView_SetItemText(queue, i, 2, const_cast<wchar_t*>(sz.c_str()));
        ListView_SetItemText(queue, i, 3, pct);
        ListView_SetItemText(queue, i, 4, const_cast<wchar_t*>(rate.c_str()));
        ListView_SetItemText(queue, i, 5, const_cast<wchar_t*>(eta.c_str()));
        ListView_SetItemText(queue, i, 6, const_cast<wchar_t*>(stText.c_str()));
    }
}

void Browser::NavigateRemote(Tab& t, const std::string& path)
{
    Worker* w = t.worker.get();
    std::string target = path;
    if (target.empty()) target = ".";
    else if (target[0] != '/' && target != "." && target != "..")
        target = SftpJoin(t.rcwd, target);
    else if (target == "..")
        target = SftpParent(t.rcwd);
    w->Post([w, target](SftpClient& c) { w->ListDir(c, target); });
}

void Browser::NavigateLocal(Tab& t, const std::wstring& path)
{
    std::wstring p = path;
    if (p == L".." ) p = LocalParent(t.lcwd);
    else if (p == L"(drives)") p = L"";
    if (!p.empty())
    {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        {
            SetStatusText(L"Not a folder: " + p);
            return;
        }
    }
    t.lcwd = p;
    RefreshLocal(t);
}

// One transfer, start to finish: resume decision, bytes, metadata,
// verification, and a retry when the failure was worth retrying.
//
// Every judgement here comes from ssh/SyncPlan.h — this function moves bytes
// and reports; it does not decide when appending is safe or when a failure is
// permanent, because those are the parts that must be testable without a
// server.
void Browser::RunTransfer(Tab& t, std::shared_ptr<Transfer> x)
{
    Worker* w = t.worker.get();
    Tab* tp = &t;
    w->Post([tp, x, w](SftpClient& c) {
        if (x->cancel.load()) { x->state = 4; return; }
        // The queue can be paused between a transfer being enqueued and the
        // worker reaching it; a paused item goes back to queued rather than
        // running, and the pump picks it up again on resume.
        if (tp->paused.load()) { x->state = 6; return; }

        // ---- resume decision --------------------------------------------
        amber::ResumeCheck rc;
        rc.mtimeToleranceSec = 2;
        rc.haveRecord = x->haveSrcRecord;
        rc.recordedSize = x->srcSizeAtStart;
        rc.recordedMtime = x->srcMtimeAtStart;
        if (x->download)
        {
            SftpEntry src;
            if (c.Stat(x->remote, src))
            {
                rc.sourceSize = src.size;
                rc.sourceMtime = static_cast<int64_t>(src.mtime);
                x->preserveMtime = static_cast<int64_t>(src.mtime);
                x->preserveMode = src.perms;
            }
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExW(x->local.c_str(), GetFileExInfoStandard, &fad))
                rc.partialSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                                 fad.nFileSizeLow;
        }
        else
        {
            WIN32_FILE_ATTRIBUTE_DATA fad = {};
            if (GetFileAttributesExW(x->local.c_str(), GetFileExInfoStandard, &fad))
            {
                rc.sourceSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                                fad.nFileSizeLow;
                ULARGE_INTEGER ft;
                ft.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                ft.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                // FILETIME is 100 ns ticks since 1601; Unix seconds since 1970.
                rc.sourceMtime = static_cast<int64_t>(ft.QuadPart / 10000000ULL) -
                                 11644473600LL;
                x->preserveMtime = rc.sourceMtime;
            }
            SftpEntry dst;
            if (c.Stat(x->remote, dst))
                rc.partialSize = dst.size;
        }
        std::string why;
        const amber::ResumeDecision rd = amber::DecideResume(rc, why);
        const uint64_t offset = amber::ResumeOffset(rc, rd);
        x->resumeFrom = offset;
        x->resumeWhy = why;
        // Record the source as it is NOW, so a later attempt has the basis it
        // needs to append. Written before any bytes move, because a record
        // written afterwards would describe a source that had already changed.
        x->srcSizeAtStart = rc.sourceSize;
        x->srcMtimeAtStart = rc.sourceMtime;
        x->haveSrcRecord = rc.sourceSize > 0;

        x->attempts.fetch_add(1);
        x->startedAt = static_cast<double>(GetTickCount64()) / 1000.0;
        x->state = 1;
        std::string err;
        auto prog = [&](uint64_t d, uint64_t tot) {
            x->done = d;
            x->total = tot;
            return !x->cancel.load() && !tp->paused.load();
        };
        const bool ok = x->download
                            ? c.DownloadFrom(x->remote, x->local, offset, prog, err)
                            : c.UploadFrom(x->local, x->remote, offset, prog, err);

        if (!ok)
        {
            if (x->cancel.load())
            {
                x->state = 4;
                return;
            }
            if (tp->paused.load())
            {
                // A paused transfer keeps its partial file and its record, so
                // resuming continues rather than starting over.
                x->error = "paused";
                x->state = 6;
                return;
            }
            x->error = err;
            x->state = 3;
            return;
        }

        // ---- metadata ----------------------------------------------------
        // Best effort, and reported rather than assumed: a server that
        // refuses setstat has not failed the transfer.
        if (tp->preserveMeta && x->preserveMtime > 0)
        {
            if (x->download)
            {
                HANDLE h = CreateFileW(x->local.c_str(), FILE_WRITE_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    ULARGE_INTEGER ul;
                    ul.QuadPart = (static_cast<uint64_t>(x->preserveMtime) +
                                   11644473600ULL) * 10000000ULL;
                    FILETIME ft;
                    ft.dwLowDateTime = ul.LowPart;
                    ft.dwHighDateTime = ul.HighPart;
                    SetFileTime(h, nullptr, nullptr, &ft);
                    CloseHandle(h);
                }
                // Unix mode bits have no faithful home on NTFS. Not applied,
                // and the queue detail says so rather than implying it was.
                if (x->preserveMode)
                    x->verifyDetail = "remote mode bits not applied: Windows has "
                                      "no equivalent";
            }
            else
            {
                std::string merr;
                if (!c.SetTimes(x->remote, x->preserveMtime, 0, merr))
                    x->verifyDetail = "timestamp not preserved: " + merr;
            }
        }

        // ---- verification ------------------------------------------------
        if (!x->verifyRequested)
        {
            x->state = 2;
            x->done.store(x->total.load());
        }
        else
        {
            x->state = 5;                  // verifying: NOT done yet
            x->verify = static_cast<int>(amber::VerifyState::Pending);
            std::string localHex, remoteHex, herr;
            bool haveLocal = amber::Sha256File(x->local, localHex, herr);
            bool available = false;
            const bool haveRemote =
                c.RemoteSha256(x->remote, remoteHex, available, herr);
            if (!haveLocal)
            {
                x->verify = static_cast<int>(amber::VerifyState::Unavailable);
                x->verifyDetail = "could not hash the local file: " + herr;
            }
            else if (!haveRemote || !available)
            {
                // No shell, or no hashing command. Reported as unavailable —
                // which, for a REQUIRED verification, is not success.
                x->verify = static_cast<int>(amber::VerifyState::Unavailable);
                x->verifyDetail = "no remote digest: " + herr;
            }
            else if (amber::DigestsMatch(localHex, remoteHex))
            {
                x->verify = static_cast<int>(amber::VerifyState::Passed);
                x->verifyDetail = "sha256 " + localHex.substr(0, 16) + "...";
            }
            else
            {
                x->verify = static_cast<int>(amber::VerifyState::Failed);
                x->verifyDetail = "local " + localHex.substr(0, 16) +
                                  "... != remote " + remoteHex.substr(0, 16) + "...";
                x->error = "checksum mismatch";
            }
            // Done means the bytes landed; whether that counts as SUCCESS is
            // Transfer::Succeeded, which requires the verification to pass.
            x->state = 2;
            x->done.store(x->total.load());
        }

        if (ok && x->refreshRemote)
        {
            std::string cwd = w->Cwd();
            std::vector<SftpEntry> e; std::string e2;
            if (c.List(cwd, e, e2)) w->Publish(cwd, std::move(e));
        }
    });
}

void Browser::QueueDownload(Tab& t, const SftpEntry& e, const std::wstring& localDir)
{
    std::string remote = SftpJoin(t.rcwd, e.name);
    if (e.dir)
    {
        // Enumerate on the worker, then queue every file (creating local dirs).
        Worker* w = t.worker.get();
        Tab* tp = &t;
        std::wstring ldir = LocalJoin(localDir, Widen(e.name));
        w->Post([this, tp, w, remote, ldir](SftpClient& c) {
            std::function<void(const std::string&, const std::wstring&)> walk =
                [&](const std::string& rdir, const std::wstring& ld) {
                CreateDirectoryW(ld.c_str(), nullptr);
                std::vector<SftpEntry> kids; std::string err;
                if (!c.List(rdir, kids, err)) return;
                for (const SftpEntry& k : kids)
                {
                    if (k.dir) { walk(SftpJoin(rdir, k.name), LocalJoin(ld, Widen(k.name))); continue; }
                    auto x = std::make_shared<Transfer>();
                    x->download = true; x->remote = SftpJoin(rdir, k.name);
                    x->local = LocalJoin(ld, Widen(k.name)); x->name = Widen(k.name);
                    x->total = k.size; x->refreshLocal = true;
                    x->verifyRequested = tp->verifyTransfers;
                    { std::lock_guard<std::mutex> lk(tp->xmx); x->id = tp->nextXfer++; tp->xfers.push_back(x); }
                    RunTransfer(*tp, x);
                }
            };
            walk(remote, ldir);
            (void)w;
        });
        return;
    }
    auto x = std::make_shared<Transfer>();
    x->download = true; x->remote = remote; x->local = LocalJoin(localDir, Widen(e.name));
    x->name = Widen(e.name); x->total = e.size; x->refreshLocal = true;
    x->verifyRequested = t.verifyTransfers;
    { std::lock_guard<std::mutex> lk(t.xmx); x->id = t.nextXfer++; t.xfers.push_back(x); }
    RunTransfer(t, x);
}

void Browser::QueueUpload(Tab& t, const std::wstring& localPath, const std::string& remoteDir)
{
    DWORD a = GetFileAttributesW(localPath.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    size_t slash = localPath.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? localPath : localPath.substr(slash + 1);
    std::string remote = SftpJoin(remoteDir, Narrow(name));
    if (a & FILE_ATTRIBUTE_DIRECTORY)
    {
        Worker* w = t.worker.get();
        w->Post([remote](SftpClient& c) { std::string err; c.Mkdir(remote, err); });
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(LocalJoin(localPath, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                QueueUpload(t, LocalJoin(localPath, fd.cFileName), remote);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        w->Post([w](SftpClient& c) { w->ListDir(c, w->Cwd()); });
        return;
    }
    auto x = std::make_shared<Transfer>();
    x->download = false; x->remote = remote; x->local = localPath; x->name = name;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(localPath.c_str(), GetFileExInfoStandard, &fad))
        x->total = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    x->refreshRemote = true;
    x->verifyRequested = t.verifyTransfers;
    { std::lock_guard<std::mutex> lk(t.xmx); x->id = t.nextXfer++; t.xfers.push_back(x); }
    RunTransfer(t, x);
}

void Browser::OpenRemote(Tab& t, const SftpEntry& e)
{
    if (e.dir) { NavigateRemote(t, e.name); return; }
    // Download into a temp folder, open with the default app, and watch for
    // saves — the edited file is re-uploaded automatically.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"AmberSSH\\edit\\" + std::to_wstring(GetCurrentProcessId()) +
                       L"\\" + std::to_wstring(active);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring local = LocalJoin(dir, Widen(e.name));
    std::string remote = SftpJoin(t.rcwd, e.name);
    auto x = std::make_shared<Transfer>();
    x->download = true; x->remote = remote; x->local = local; x->name = Widen(e.name) + L" (open)";
    x->total = e.size;
    { std::lock_guard<std::mutex> lk(t.xmx); x->id = t.nextXfer++; t.xfers.push_back(x); }
    RunTransfer(t, x);
    // Open + register the watch once the download lands (polled in Poll()).
    EditWatch ew; ew.remote = remote; ew.local = local; ew.mtime = {};
    t.edits.push_back(ew);
}

void Browser::CopySelection(bool fromRemote)
{
    Tab* t = Active(); if (!t) return;
    if (fromRemote)
    {
        for (int i : Selected(rlist))
        {
            LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(rlist, &it);
            if (it.lParam < 0 || it.lParam >= (LPARAM)t->rentries.size()) continue;
            if (t->lcwd.empty()) { SetStatusText(L"Pick a local folder first."); return; }
            QueueDownload(*t, t->rentries[(size_t)it.lParam], t->lcwd);
        }
    }
    else
    {
        for (int i : Selected(llist))
        {
            LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(llist, &it);
            if (it.lParam < 0 || it.lParam >= (LPARAM)t->lentries.size()) continue;
            QueueUpload(*t, LocalJoin(t->lcwd, t->lentries[(size_t)it.lParam].name), t->rcwd);
        }
    }
    FillQueue(*t);
}

void Browser::DeleteSelection(bool remote)
{
    Tab* t = Active(); if (!t) return;
    HWND lv = remote ? rlist : llist;
    std::vector<int> sel = Selected(lv);
    if (sel.empty()) return;
    std::wstring q = L"Delete " + std::to_wstring(sel.size()) + (remote ? L" remote item(s)?" : L" local item(s) (to the Recycle Bin)?");
    if (MessageBoxW(hwnd, q.c_str(), L"AmberSSH — SFTP", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    if (remote)
    {
        Worker* w = t->worker.get();
        std::vector<std::pair<std::string, bool>> paths;
        for (int i : sel)
        {
            LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it);
            if (it.lParam < 0) continue;
            const SftpEntry& e = t->rentries[(size_t)it.lParam];
            paths.push_back({ SftpJoin(t->rcwd, e.name), e.dir && !e.link });
        }
        w->Post([w, paths](SftpClient& c) {
            std::string err;
            for (const auto& p : paths)
            {
                bool ok = p.second ? c.RemoveTree(p.first, err) : c.Unlink(p.first, err);
                if (!ok) { w->SetStatus(err, false); }
            }
            w->ListDir(c, w->Cwd());
        });
    }
    else
    {
        std::wstring files;
        for (int i : sel)
        {
            LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it);
            if (it.lParam < 0) continue;
            files += LocalJoin(t->lcwd, t->lentries[(size_t)it.lParam].name);
            files.push_back(L'\0');
        }
        files.push_back(L'\0');
        SHFILEOPSTRUCTW op = {};
        op.hwnd = hwnd; op.wFunc = FO_DELETE; op.pFrom = files.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        SHFileOperationW(&op);
        RefreshLocal(*t);
    }
}

void Browser::RenameSelected(bool remote)
{
    HWND lv = remote ? rlist : llist;
    int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (i >= 0) { SetFocus(lv); ListView_EditLabel(lv, i); }
}

void Browser::NewFolder(bool remote)
{
    Tab* t = Active(); if (!t) return;
    std::wstring name = L"New Folder";
    if (!PromptString(hwnd, L"New Folder", L"Folder name:", name) || name.empty()) return;
    if (remote)
    {
        Worker* w = t->worker.get();
        std::string path = SftpJoin(t->rcwd, Narrow(name));
        w->Post([w, path](SftpClient& c) {
            std::string err;
            if (!c.Mkdir(path, err)) w->SetStatus(err, false);
            w->ListDir(c, w->Cwd());
        });
    }
    else
    {
        CreateDirectoryW(LocalJoin(t->lcwd, name).c_str(), nullptr);
        RefreshLocal(*t);
    }
}

void Browser::Permissions()
{
    Tab* t = Active(); if (!t) return;
    std::vector<int> sel = Selected(rlist);
    if (sel.empty()) return;
    LVITEMW it = { LVIF_PARAM }; it.iItem = sel[0]; ListView_GetItem(rlist, &it);
    if (it.lParam < 0) return;
    const SftpEntry& e = t->rentries[(size_t)it.lParam];
    wchar_t cur[8]; swprintf_s(cur, L"%03o", e.perms & 0777);
    std::wstring v = cur;
    if (!PromptString(hwnd, L"Permissions", (L"Octal mode for " + Widen(e.name) + L" (e.g. 644, 755):").c_str(), v))
        return;
    uint32_t mode = (uint32_t)wcstoul(v.c_str(), nullptr, 8) & 07777;
    std::vector<std::string> paths;
    for (int i : sel)
    {
        LVITEMW it2 = { LVIF_PARAM }; it2.iItem = i; ListView_GetItem(rlist, &it2);
        if (it2.lParam >= 0) paths.push_back(SftpJoin(t->rcwd, t->rentries[(size_t)it2.lParam].name));
    }
    Worker* w = t->worker.get();
    w->Post([w, paths, mode](SftpClient& c) {
        std::string err;
        for (const std::string& p : paths) if (!c.Chmod(p, mode, err)) w->SetStatus(err, false);
        w->ListDir(c, w->Cwd());
    });
}

void Browser::ContextMenu(bool remote, POINT pt)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IdCtxOpen, remote ? L"Open / Edit\tF4" : L"Open\tEnter");
    AppendMenuW(m, MF_STRING, IdCtxCopy, remote ? L"Download to local folder\tF5" : L"Upload to remote folder\tF5");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IdCtxRename, L"Rename\tF2");
    AppendMenuW(m, MF_STRING, IdCtxDelete, L"Delete\tF8");
    AppendMenuW(m, MF_STRING, IdCtxMkdir, L"New Folder\tF7");
    if (remote) AppendMenuW(m, MF_STRING, IdCtxPerms, L"Permissions...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IdCtxCopyPath, L"Copy path");
    AppendMenuW(m, MF_STRING, IdCtxRefresh, L"Refresh\tCtrl+R");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    Tab* t = Active(); if (!t) return;
    HWND lv = remote ? rlist : llist;
    switch (cmd)
    {
    case IdCtxOpen:
    {
        int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); if (i < 0) break;
        LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it);
        if (remote)
        {
            if (it.lParam < 0) NavigateRemote(*t, "..");
            else OpenRemote(*t, t->rentries[(size_t)it.lParam]);
        }
        else
        {
            if (it.lParam < 0) NavigateLocal(*t, L"..");
            else
            {
                const LocalEntry& e = t->lentries[(size_t)it.lParam];
                if (e.dir) NavigateLocal(*t, LocalJoin(t->lcwd, e.name));
                else ShellExecuteW(hwnd, L"open", LocalJoin(t->lcwd, e.name).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        break;
    }
    case IdCtxCopy:   CopySelection(remote); break;
    case IdCtxRename: RenameSelected(remote); break;
    case IdCtxDelete: DeleteSelection(remote); break;
    case IdCtxMkdir:  NewFolder(remote); break;
    case IdCtxPerms:  Permissions(); break;
    case IdCtxRefresh:
        if (remote) NavigateRemote(*t, "."); else RefreshLocal(*t);
        break;
    case IdCtxCopyPath:
    {
        int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); if (i < 0) break;
        LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it);
        std::wstring p = remote ? Widen(it.lParam >= 0 ? SftpJoin(t->rcwd, t->rentries[(size_t)it.lParam].name) : t->rcwd)
                                : (it.lParam >= 0 ? LocalJoin(t->lcwd, t->lentries[(size_t)it.lParam].name) : t->lcwd);
        if (OpenClipboard(hwnd))
        {
            EmptyClipboard();
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (p.size() + 1) * sizeof(wchar_t));
            if (mem) { memcpy(GlobalLock(mem), p.c_str(), (p.size() + 1) * sizeof(wchar_t)); GlobalUnlock(mem); SetClipboardData(CF_UNICODETEXT, mem); }
            CloseClipboard();
        }
        break;
    }
    }
}

void Browser::Poll()
{
    Tab* t = Active(); if (!t) return;
    Worker::Snapshot s = t->worker->Get();
    unsigned g = t->worker->Gen();
    if (g != t->rgenSeen)
    {
        t->rgenSeen = g;
        t->rcwd = s.cwd;
        t->rentries = s.entries;
        t->connected = s.connected;
        FillRemote(*t);
        if (!t->pendingOpen.empty() && s.connected)
        {
            std::string want = t->pendingOpen; t->pendingOpen.clear();
            for (const SftpEntry& e : t->rentries)
                if (SftpJoin(t->rcwd, e.name) == want) { OpenRemote(*t, e); break; }
        }
    }
    // Queue + aggregate progress.
    uint64_t done = 0, total = 0; int running = 0; bool anyDoneLocal = false;
    {
        std::lock_guard<std::mutex> lk(t->xmx);
        for (auto& x : t->xfers)
        {
            int st = x->state.load();
            if (st == 1) { ++running; done += x->done.load(); total += x->total.load(); }
            if (st == 2 && x->refreshLocal) { anyDoneLocal = true; x->refreshLocal = false; }
        }
    }
    FillQueue(*t);
    if (anyDoneLocal) RefreshLocal(*t);
    SetTaskbarProgress(hwnd, running ? (total ? (double)done / (double)total : 0.0) : -1.0);
    std::wstring st = Widen(s.status);
    if (running) st += L"   |   " + std::to_wstring(running) + L" transfer(s) running";
    SetStatusText(st);

    // Edit watches: open once the download finished; re-upload on save.
    double now = GetTickCount64() / 1000.0;
    if (now - lastEditPoll > 1.5)
    {
        lastEditPoll = now;
        for (EditWatch& ew : t->edits)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (!GetFileAttributesExW(ew.local.c_str(), GetFileExInfoStandard, &fad)) continue;
            bool downloaded = false;
            {
                std::lock_guard<std::mutex> lk(t->xmx);
                for (auto& x : t->xfers)
                    if (x->download && x->local == ew.local && x->state.load() == 2) downloaded = true;
            }
            if (!downloaded) continue;
            if (ew.mtime.dwLowDateTime == 0 && ew.mtime.dwHighDateTime == 0)
            {
                ew.mtime = fad.ftLastWriteTime;
                ShellExecuteW(hwnd, L"open", ew.local.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                continue;
            }
            if (CompareFileTime(&fad.ftLastWriteTime, &ew.mtime) != 0)
            {
                ew.mtime = fad.ftLastWriteTime;
                auto x = std::make_shared<Transfer>();
                x->download = false; x->remote = ew.remote; x->local = ew.local;
                x->name = ew.local.substr(ew.local.find_last_of(L'\\') + 1) + L" (saved \u2192 re-upload)";
                x->total = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                x->refreshRemote = true;
                { std::lock_guard<std::mutex> lk(t->xmx); x->id = t->nextXfer++; t->xfers.push_back(x); }
                RunTransfer(*t, x);
            }
        }
    }
}

LRESULT Browser::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    DialogPalette pal = MakeDialogPalette();
    switch (m)
    {
    case WM_SIZE: Layout(); return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize = { Px(760), Px(480) };
        return 0;
    }
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, bgBrush);
        amber::skin::Scanlines((HDC)w, rc, dpi);
        if (amber::skin::Meter())
            amber::skin::Brushed((HDC)w, rc, dpi, pal.bg);
        if (amber::skin::Glaze())
            amber::skin::OilSpot((HDC)w, rc, dpi, amber::skin::kOilSpot);
        return 1;
    }
    case WM_PAINT:
    {
        // The skin's outlines around the two file panes, the queue and the
        // path fields. Replaces the stock sunken 3D borders those controls
        // used to carry, which belonged to no skin.
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        const HWND focus = GetFocus();
        for (HWND c : { llist, rlist, queue, lpath, rpath })
        {
            if (!c || !IsWindowVisible(c))
                continue;
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(nullptr, h, (POINT*)&r, 2);
            InflateRect(&r, Px(2), Px(2));
            amber::skin::FrameWell(dc, r, dpi, c == focus);
        }
        // Solder Mask: the two panes are two sections of one board, joined by
        // a copper bus routed down the gutter between them and terminated in
        // gold vias. Every pane carries its silkscreen reference designator.
        if (amber::skin::Traces() && llist && rlist)
        {
            RECT lr, rr;
            GetWindowRect(llist, &lr); MapWindowPoints(nullptr, h, (POINT*)&lr, 2);
            GetWindowRect(rlist, &rr); MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
            const int tw = amber::skin::TraceW(dpi);
            const int bx = (lr.right + rr.left) / 2;
            HPEN pen = amber::skin::RoundPen(tw, pal.border);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, bx, lr.top + Px(6), nullptr);
            LineTo(dc, bx, lr.bottom - Px(6));
            SelectObject(dc, op);
            DeleteObject(pen);
            amber::skin::Via(dc, bx, lr.top + Px(6), tw, pal.accent, pal.bg);
            amber::skin::Via(dc, bx, lr.bottom - Px(6), tw, pal.accent, pal.bg);

            auto ref = [&](HWND c, const wchar_t* s) {
                if (!c) return;
                RECT r;
                GetWindowRect(c, &r);
                MapWindowPoints(nullptr, h, (POINT*)&r, 2);
                RECT t = { r.left, r.top - Px(16), r.right - Px(4), r.top - Px(2) };
                amber::skin::Designator(dc, t, s, silk, pal.textDis);
            };
            ref(llist, L"J1  LOCAL");
            ref(rlist, L"J2  REMOTE");
            ref(queue, L"U3  TRANSFER QUEUE");
        }
        // Horologe: the seam between the panes carries the rehaut run
        // vertically — its ticks fit the gutter exactly — and, when the
        // toolbar leaves room at its right end, a power-reserve gauge shows
        // how much of the active tab's transfers remain. The gauge's pivot is
        // this surface's one blued screw.
        if (amber::skin::Rehaut() && llist && rlist)
        {
            RECT lr, rr;
            GetWindowRect(llist, &lr); MapWindowPoints(nullptr, h, (POINT*)&lr, 2);
            GetWindowRect(rlist, &rr); MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
            const int gx = (lr.right + rr.left) / 2;
            const int T = amber::skin::TickPitch(dpi);
            HBRUSH dimB = CreateSolidBrush(pal.textDim);
            HBRUSH hotB = CreateSolidBrush(pal.text);
            int i = 0;
            for (int y = lr.top + Px(2); y <= lr.bottom - Px(2); y += T, ++i)
            {
                const bool lng = (i % 5) == 0;
                const int hw = lng ? Px(4) : Px(2);
                RECT t = { gx - hw, y, gx + hw, y + 1 };
                FillRect(dc, &t, lng ? hotB : dimB);
            }
            DeleteObject(dimB);
            DeleteObject(hotB);

            if (!buttons.empty())
            {
                RECT lb, cr;
                GetWindowRect(buttons.back(), &lb); MapWindowPoints(nullptr, h, (POINT*)&lb, 2);
                GetClientRect(h, &cr);
                if (cr.right - Px(8) - lb.right >= Px(150))
                {
                    uint64_t done = 0, total = 0;
                    if (Tab* t = Active())
                    {
                        std::lock_guard<std::mutex> lk(t->xmx);
                        for (auto& x : t->xfers)
                        {
                            done += x->done.load();
                            total += x->total.load();
                        }
                    }
                    const double frac = total ? (double)done / (double)total : 0.0;
                    const int R = Px(24);
                    const int cx = cr.right - Px(8) - R - Px(4), cy = lb.bottom - Px(1);
                    // A 120° sector, 210°..330°, ticks every 12°; the needle
                    // sits at the fraction transferred.
                    HPEN tp = CreatePen(PS_SOLID, 1, pal.textDim);
                    HGDIOBJ op = SelectObject(dc, tp);
                    for (int k = 0; k <= 10; ++k)
                    {
                        const double a = (210.0 + 12.0 * k) * 3.14159265358979 / 180.0;
                        const int len = (k % 5 == 0) ? Px(6) : Px(3);
                        MoveToEx(dc, cx + (int)(std::cos(a) * R), cy + (int)(std::sin(a) * R), nullptr);
                        LineTo(dc, cx + (int)(std::cos(a) * (R - len)), cy + (int)(std::sin(a) * (R - len)));
                    }
                    SelectObject(dc, op);
                    DeleteObject(tp);
                    const double na = (210.0 + 120.0 * frac) * 3.14159265358979 / 180.0;
                    HPEN np = amber::skin::RoundPen(Px(2), pal.text);
                    op = SelectObject(dc, np);
                    MoveToEx(dc, cx, cy, nullptr);
                    LineTo(dc, cx + (int)(std::cos(na) * (R - Px(2))), cy + (int)(std::sin(na) * (R - Px(2))));
                    SelectObject(dc, op);
                    DeleteObject(np);
                    amber::skin::BluedScrew(dc, cx, cy, Px(3),
                                            amber::SrgbRef(amber::Chrome().neonB), pal.bg);
                    RECT lab = { cx - R - Px(70), cy - Px(14), cx - R - Px(4), cy };
                    amber::skin::Designator(dc, lab, L"RESERVE", silk, pal.textDim);
                }
            }
        }
        if (amber::skin::Impression() && !buttons.empty())
        {
            // Letterpress: a printer's thick-thin rule under the toolbar.
            RECT lb, cr;
            GetWindowRect(buttons.back(), &lb); MapWindowPoints(nullptr, h, (POINT*)&lb, 2);
            GetClientRect(h, &cr);
            amber::skin::ThickThin(dc, Px(8), lb.bottom + Px(2), cr.right - Px(16),
                                   pal.borderHot, dpi);
        }
        if (amber::skin::Stitch() && llist && rlist)
        {
            // Atelier: a seam down the gutter between the two hides.
            RECT lr, rr;
            GetWindowRect(llist, &lr); MapWindowPoints(nullptr, h, (POINT*)&lr, 2);
            GetWindowRect(rlist, &rr); MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
            const int gx = (lr.right + rr.left) / 2;
            RECT paint = { gx - Px(1), lr.top, gx + Px(1), lr.bottom };
            HBRUSH pb = CreateSolidBrush(pal.border);
            FillRect(dc, &paint, pb);
            DeleteObject(pb);
            amber::skin::StitchRunV(dc, gx - Px(4), lr.top + Px(2), lr.bottom - lr.top - Px(4), dpi, pal.text);
            amber::skin::StitchRunV(dc, gx + Px(4), lr.top + Px(2), lr.bottom - lr.top - Px(4), dpi, pal.text);
        }
        if (amber::skin::Meter() && !buttons.empty())
        {
            // Reference: twin VU meters — L for down, R for up — at the
            // toolbar's right end when it leaves room, driven by the active
            // tab's transfers.
            RECT lb, cr;
            GetWindowRect(buttons.back(), &lb); MapWindowPoints(nullptr, h, (POINT*)&lb, 2);
            GetClientRect(h, &cr);
            if (cr.right - Px(8) - lb.right >= Px(230))
            {
                uint64_t dd = 0, dt = 0, ud = 0, ut = 0;
                if (Tab* t = Active())
                {
                    std::lock_guard<std::mutex> lk(t->xmx);
                    for (auto& x : t->xfers)
                    {
                        if (x->download) { dd += x->done.load(); dt += x->total.load(); }
                        else             { ud += x->done.load(); ut += x->total.load(); }
                    }
                }
                const amber::ChromeSpec& ch = amber::Chrome();
                RECT vr = { cr.right - Px(8) - Px(104), lb.top - Px(3), cr.right - Px(8), lb.bottom + Px(3) };
                RECT vl = { vr.left - Px(110), vr.top, vr.left - Px(6), vr.bottom };
                amber::skin::VuMeter(dc, vl, dt ? (double)dd / (double)dt : 0.0, dpi,
                                     amber::SrgbRef(ch.neonB), RGB(0xEA, 0xF0, 0xFF),
                                     amber::SrgbRef(ch.danger), amber::SrgbRef(ch.danger),
                                     L"L · DOWN", silk);
                amber::skin::VuMeter(dc, vr, ut ? (double)ud / (double)ut : 0.0, dpi,
                                     amber::SrgbRef(ch.neonB), RGB(0xEA, 0xF0, 0xFF),
                                     amber::SrgbRef(ch.danger), amber::SrgbRef(ch.danger),
                                     L"R · UP", silk);
            }
        }
        if (amber::skin::Glaze() && llist && rlist)
        {
            // Tenmoku: a rust rim along the top of each pane, a foot under
            // it, and the gutter between them left as raw clay.
            RECT lr, rr;
            GetWindowRect(llist, &lr); MapWindowPoints(nullptr, h, (POINT*)&lr, 2);
            GetWindowRect(rlist, &rr); MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
            HBRUSH rb = CreateSolidBrush(amber::skin::kRust);
            HBRUSH cb = CreateSolidBrush(amber::skin::kClay);
            for (const RECT& r : { lr, rr })
            {
                RECT rim = { r.left, r.top - Px(4), r.right, r.top - Px(2) };
                RECT foot = { r.left, r.bottom + Px(1), r.right, r.bottom + Px(4) };
                FillRect(dc, &rim, rb);
                FillRect(dc, &foot, cb);
            }
            RECT gutter = { lr.right + Px(2), lr.top, rr.left - Px(2), lr.bottom };
            FillRect(dc, &gutter, cb);
            DeleteObject(rb);
            DeleteObject(cb);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: SetBkColor((HDC)w, pal.bg); SetTextColor((HDC)w, pal.textDim); return (LRESULT)bgBrush;
    case WM_CTLCOLOREDIT: SetBkColor((HDC)w, pal.field); SetTextColor((HDC)w, pal.text); return (LRESULT)fieldBrush;
    case WM_DRAWITEM:
    {
        auto* dis = (DRAWITEMSTRUCT*)l;
        if (dis->CtlType == ODT_BUTTON)
        {
            // Download and Upload are the primary actions; Delete is the
            // destructive one, which the hazard skins stripe.
            const bool accent = dis->CtlID == IdBtnDownload || dis->CtlID == IdBtnUpload;
            const bool danger = dis->CtlID == IdBtnDelete;
            amber::skin::DrawButton(*dis, font, dpi, bgBrush, accent, danger,
                                    GetPropW(dis->hwndItem, kHoverProp) != nullptr);
            return TRUE;
        }
        if (dis->CtlType == ODT_TAB)
        {
            const bool sel = (dis->itemState & ODS_SELECTED) != 0;
            RECT r = dis->rcItem;
            HBRUSH b = CreateSolidBrush(sel ? pal.accent : pal.field);
            HPEN pn = CreatePen(PS_SOLID, 1, sel ? pal.borderHot : pal.border);
            HGDIOBJ ob = SelectObject(dis->hDC, b);
            HGDIOBJ op = SelectObject(dis->hDC, pn);
            amber::skin::Shape(dis->hDC, r,
                               amber::skin::Skinned()
                                   ? Px((int)amber::Chrome().chamfer)
                                   : 0,
                               Px(4), dpi);
            SelectObject(dis->hDC, ob);
            SelectObject(dis->hDC, op);
            DeleteObject(b);
            DeleteObject(pn);
            wchar_t buf[128] = L"";
            TCITEMW ti = { TCIF_TEXT };
            ti.pszText = buf;
            ti.cchTextMax = 128;
            TabCtrl_GetItem(tabs, dis->itemID, &ti);
            std::wstring label = amber::skin::Label(buf);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC,
                         sel ? amber::skin::InkOnRef(pal.accent) : pal.textDim);
            HGDIOBJ of = SelectObject(dis->hDC, font);
            DrawTextW(dis->hDC, label.c_str(), -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dis->hDC, of);
            return TRUE;
        }
        return 0;
    }
    case WM_TIMER: Poll(); return 0;
    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)w;
        POINT pt; DragQueryPoint(drop, &pt);
        RECT rr; GetWindowRect(rlist, &rr); MapWindowPoints(nullptr, h, (POINT*)&rr, 2);
        Tab* t = Active();
        if (t && PtInRect(&rr, pt))
        {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i)
            {
                wchar_t path[MAX_PATH];
                if (DragQueryFileW(drop, i, path, MAX_PATH)) QueueUpload(*t, path, t->rcwd);
            }
            FillQueue(*t);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_NOTIFY:
    {
        NMHDR* nm = (NMHDR*)l;
        Tab* t = Active();
        if (nm->idFrom == IdTabs && nm->code == TCN_SELCHANGE) { SwitchTab(TabCtrl_GetCurSel(tabs)); return 0; }
        if (!t) return 0;
        bool remote = nm->idFrom == IdRList;
        HWND lv = remote ? rlist : llist;
        if ((nm->idFrom == IdLList || nm->idFrom == IdRList))
        {
            if (nm->code == NM_DBLCLK || nm->code == NM_RETURN)
            {
                int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); if (i < 0) return 0;
                LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it);
                if (remote)
                {
                    if (it.lParam < 0) NavigateRemote(*t, "..");
                    else OpenRemote(*t, t->rentries[(size_t)it.lParam]);
                }
                else
                {
                    if (it.lParam < 0) NavigateLocal(*t, L"..");
                    else
                    {
                        const LocalEntry& e = t->lentries[(size_t)it.lParam];
                        if (e.dir) NavigateLocal(*t, t->lcwd.empty() ? e.name : LocalJoin(t->lcwd, e.name));
                        else ShellExecuteW(h, L"open", LocalJoin(t->lcwd, e.name).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                return 0;
            }
            if (nm->code == NM_RCLICK)
            {
                POINT pt; GetCursorPos(&pt);
                ContextMenu(remote, pt);
                return 0;
            }
            if (nm->code == LVN_COLUMNCLICK)
            {
                int col = ((NMLISTVIEW*)l)->iSubItem;
                if (remote) { if (t->rsortCol == col) t->rsortAsc = !t->rsortAsc; else { t->rsortCol = col; t->rsortAsc = true; } FillRemote(*t); }
                else { if (t->lsortCol == col) t->lsortAsc = !t->lsortAsc; else { t->lsortCol = col; t->lsortAsc = true; } FillLocal(*t); }
                return 0;
            }
            if (nm->code == LVN_BEGINLABELEDIT)
            {
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)l;
                return di->item.lParam < 0 ? TRUE : FALSE;   // ".." is not renamable
            }
            if (nm->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)l;
                if (!di->item.pszText || !*di->item.pszText || di->item.lParam < 0) return FALSE;
                std::wstring newName = di->item.pszText;
                if (remote)
                {
                    const SftpEntry& e = t->rentries[(size_t)di->item.lParam];
                    std::string from = SftpJoin(t->rcwd, e.name), to = SftpJoin(t->rcwd, Narrow(newName));
                    Worker* wk = t->worker.get();
                    wk->Post([wk, from, to](SftpClient& c) { std::string err; if (!c.Rename(from, to, err)) wk->SetStatus(err, false); wk->ListDir(c, wk->Cwd()); });
                }
                else
                {
                    const LocalEntry& e = t->lentries[(size_t)di->item.lParam];
                    MoveFileW(LocalJoin(t->lcwd, e.name).c_str(), LocalJoin(t->lcwd, newName).c_str());
                    RefreshLocal(*t);
                }
                return FALSE;   // the refresh redraws with the real name
            }
            if (nm->code == LVN_KEYDOWN)
            {
                WORD vk = ((NMLVKEYDOWN*)l)->wVKey;
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                switch (vk)
                {
                case VK_BACK: if (remote) NavigateRemote(*t, ".."); else NavigateLocal(*t, L".."); return 0;
                case VK_F2: RenameSelected(remote); return 0;
                case VK_F4:
                    if (remote) { int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); if (i >= 0) { LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(lv, &it); if (it.lParam >= 0) OpenRemote(*t, t->rentries[(size_t)it.lParam]); } }
                    return 0;
                case VK_F5: CopySelection(remote); return 0;
                case VK_F7: NewFolder(remote); return 0;
                case VK_F8: case VK_DELETE: DeleteSelection(remote); return 0;
                case 'R': if (ctrl) { if (remote) NavigateRemote(*t, "."); else RefreshLocal(*t); } return 0;
                case 'A': if (ctrl) ListView_SetItemState(lv, -1, LVIS_SELECTED, LVIS_SELECTED); return 0;
                case 'T': if (ctrl && newTabFn) newTabFn(newTabCtx); return 0;
                case 'W': if (ctrl) CloseTab(active); return 0;
                case VK_TAB: SetFocus(remote ? llist : rlist); return 0;
                }
            }
        }
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(w);
        Tab* t = Active();
        bool remoteFocus = GetFocus() == rlist;
        switch (id)
        {
        case IdBtnDownload: CopySelection(true); break;
        case IdBtnUpload:   CopySelection(false); break;
        case IdBtnOpen:
            if (t) { int i = ListView_GetNextItem(rlist, -1, LVNI_SELECTED); if (i >= 0) { LVITEMW it = { LVIF_PARAM }; it.iItem = i; ListView_GetItem(rlist, &it); if (it.lParam >= 0) OpenRemote(*t, t->rentries[(size_t)it.lParam]); } }
            break;
        case IdBtnRename:   RenameSelected(remoteFocus || GetFocus() != llist); break;
        case IdBtnMkdir:    NewFolder(remoteFocus || GetFocus() != llist); break;
        case IdBtnDelete:   DeleteSelection(remoteFocus || GetFocus() != llist); break;
        case IdBtnPerms:    Permissions(); break;
        case IdBtnRefresh:  if (t) { NavigateRemote(*t, "."); RefreshLocal(*t); } break;
        case IdBtnNewTab:   if (newTabFn) newTabFn(newTabCtx); break;
        case IdBtnCloseTab: CloseTab(active); break;
        case IdBtnCancel:
            if (t)
            {
                std::lock_guard<std::mutex> lk(t->xmx);
                for (int i : Selected(queue)) if (i < (int)t->xfers.size()) t->xfers[(size_t)i]->cancel = true;
                if (Selected(queue).empty()) for (auto& x : t->xfers) if (x->state.load() < 2) x->cancel = true;
            }
            break;
        case IdBtnClearQueue:
            if (t)
            {
                // Only finished items go: a running or paused transfer left
                // in the list is the only handle on it.
                std::lock_guard<std::mutex> lk(t->xmx);
                t->xfers.erase(std::remove_if(t->xfers.begin(), t->xfers.end(),
                                              [](const std::shared_ptr<Transfer>& x)
                                              {
                                                  const int s = x->state.load();
                                                  return s == 2 || s == 3 || s == 4;
                                              }),
                               t->xfers.end());
            }
            break;
        case IdBtnPauseQueue:
            if (t)
            {
                const bool now = !t->paused.load();
                t->paused = now;
                if (now)
                {
                    SetStatusText(L"Queue paused. Running transfers stop at the "
                                  L"next block and keep their partial files.");
                }
                else
                {
                    // Resuming re-posts every paused item. Each one runs its
                    // resume decision again, so a source that changed while
                    // the queue was paused restarts rather than appending.
                    std::vector<std::shared_ptr<Transfer>> again;
                    {
                        std::lock_guard<std::mutex> lk(t->xmx);
                        for (auto& x : t->xfers)
                            if (x->state.load() == 6)
                            {
                                x->state = 0;
                                x->error.clear();
                                again.push_back(x);
                            }
                    }
                    for (auto& x : again)
                        RunTransfer(*t, x);
                    SetStatusText(L"Queue resumed (" + std::to_wstring(again.size()) +
                                  L" transfers).");
                }
                SetWindowTextW(GetDlgItem(h, IdBtnPauseQueue),
                               now ? L"Resume Queue" : L"Pause Queue");
            }
            break;
        case IdBtnRetryFailed:
            if (t)
            {
                // Only failures worth retrying are re-queued. A permission
                // error or a full disk fails the same way every time, so
                // retrying it would only look like the button did nothing.
                std::vector<std::shared_ptr<Transfer>> again;
                int skipped = 0;
                {
                    std::lock_guard<std::mutex> lk(t->xmx);
                    for (auto& x : t->xfers)
                    {
                        if (x->state.load() != 3)
                            continue;
                        if (!amber::RetryableError(x->error))
                        {
                            ++skipped;
                            continue;
                        }
                        x->state = 0;
                        x->cancel = false;
                        again.push_back(x);
                    }
                }
                for (auto& x : again)
                    RunTransfer(*t, x);
                std::wstring msg = L"Retrying " + std::to_wstring(again.size()) +
                                   L" transfers.";
                if (skipped)
                    msg += L" " + std::to_wstring(skipped) +
                           L" left alone: the error will not change on a retry.";
                SetStatusText(msg);
            }
            break;
        case IdBtnVerify:
            if (t)
            {
                t->verifyTransfers = !t->verifyTransfers;
                SetWindowTextW(GetDlgItem(h, IdBtnVerify),
                               t->verifyTransfers ? L"Verify: SHA-256" : L"Verify: off");
                SetStatusText(t->verifyTransfers
                                  ? L"New transfers will be checked with SHA-256. "
                                    L"A transfer whose hash cannot be obtained is "
                                    L"reported, not assumed good."
                                  : L"Checksum verification off.");
            }
            break;
        case IdBtnCompare:
            if (t)
                CompareFolders(*t);
            break;
        case IdBtnSyncDir:
            if (t)
            {
                // Cycles the three directions. Explicit rather than inferred
                // from which pane has focus: a sync that picks its own
                // direction is a sync that eventually picks the wrong one.
                t->syncDir = t->syncDir == amber::SyncDirection::LocalToRemote
                                 ? amber::SyncDirection::RemoteToLocal
                             : t->syncDir == amber::SyncDirection::RemoteToLocal
                                 ? amber::SyncDirection::TwoWay
                                 : amber::SyncDirection::LocalToRemote;
                const wchar_t* label =
                    t->syncDir == amber::SyncDirection::LocalToRemote
                        ? L"Sync: local → remote"
                    : t->syncDir == amber::SyncDirection::RemoteToLocal
                        ? L"Sync: remote → local"
                        : L"Sync: two-way";
                SetWindowTextW(GetDlgItem(hwnd, IdBtnSyncDir), label);
                SetStatusText(
                    t->syncDir == amber::SyncDirection::TwoWay
                        ? L"Two-way: each side's newer copy wins, and anything "
                          L"that changed on both is reported as a conflict "
                          L"rather than merged."
                        : L"One-way: a destination that is NEWER than the "
                          L"source is left alone, never overwritten.");
            }
            break;
        case IdBtnExcludes:
            if (t)
                EditExcludes(*t);
            break;
        case IdBtnSyncDelete:
            if (t)
            {
                if (!t->syncDelete)
                {
                    // Turning a copy into a mirror is confirmed here AND
                    // again in the plan, because the plan is where the actual
                    // count of doomed files finally appears.
                    if (MessageBoxW(hwnd,
                                    L"List files the source does not have as "
                                    L"deletions?\r\n\r\nThis turns a copy into a "
                                    L"mirror. The plan will count them, and "
                                    L"AmberSSH still will not delete anything "
                                    L"without you confirming that plan.",
                                    L"AmberSSH — mirror mode",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                        break;
                }
                t->syncDelete = !t->syncDelete;
                SetWindowTextW(GetDlgItem(hwnd, IdBtnSyncDelete),
                               t->syncDelete ? L"Delete extraneous: YES"
                                             : L"Delete extraneous: no");
                SetStatusText(t->syncDelete
                                  ? L"Extraneous files will be LISTED in the plan."
                                  : L"Extraneous files will be ignored.");
            }
            break;
        }
        return 0;
    }
    case WM_APP_COMPARE_DONE:
    {
        // The worker finished walking both trees and built the plan; the
        // confirmation has to happen on the UI thread.
        auto* result =
            reinterpret_cast<std::pair<amber::SyncPlan, bool>*>(l);
        if (result)
        {
            if (Tab* t = Active())
                ShowPlan(*t, result->first, result->second);
            delete result;
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        // Tear down connections; the window is recreated on next open.
        tabList.clear();
        while (TabCtrl_GetItemCount(tabs) > 0) TabCtrl_DeleteItem(tabs, 0);
        active = -1;
        SetTaskbarProgress(h, -1.0);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        hwnd = nullptr;
        if (bgBrush) { DeleteObject(bgBrush); bgBrush = nullptr; }
        if (fieldBrush) { DeleteObject(fieldBrush); fieldBrush = nullptr; }
        if (font) { DeleteObject(font); font = nullptr; }
        if (silk) { DeleteObject(silk); silk = nullptr; }
        if (mono) { DeleteObject(mono); mono = nullptr; }
        if (icons) { ImageList_Destroy(icons); icons = nullptr; }
        buttons.clear();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

void SftpBrowser::OpenTab(HWND owner, const ConnectionProfile& profile,
                          const SecureString& password, const SecureString& passphrase,
                          const std::string& initialDir)
{
    Browser& b = Browser::Get();
    b.EnsureWindow(owner);
    b.AddTab(profile, password.Reveal(), passphrase.Reveal(), initialDir);
}

void SftpBrowser::OpenRemoteFile(HWND owner, const ConnectionProfile& profile,
                                 const SecureString& password,
                                 const SecureString& passphrase,
                                 const std::string& remotePath)
{
    Browser& b = Browser::Get();
    b.EnsureWindow(owner);
    // Reuse a connected tab for the same profile when there is one.
    int idx = -1;
    for (size_t i = 0; i < b.tabList.size(); ++i)
        if (b.tabList[i]->profile.id == profile.id) { idx = (int)i; break; }
    std::string dir = SftpParent(remotePath);
    if (idx < 0)
        idx = b.AddTab(profile, password.Reveal(), passphrase.Reveal(), dir);
    else
    {
        b.SwitchTab(idx);
        b.NavigateRemote(*b.tabList[(size_t)idx], dir);
    }
    b.tabList[(size_t)idx]->pendingOpen = remotePath;
    b.tabList[(size_t)idx]->rgenSeen = ~0u;   // force the pending-open check
}

void SftpBrowser::SetNewTabHandler(NewTabFn fn, void* ctx)
{
    Browser::Get().newTabFn = fn;
    Browser::Get().newTabCtx = ctx;
}

bool SftpBrowser::IsOpen()
{
    return Browser::Get().hwnd != nullptr;
}


// ------------------------------------------------------------- comparison
// Walks both trees, classifies every path, shows the plan, and only then
// asks. Non-destructive up to the confirmation by construction: the walk and
// the plan are pure functions of metadata (ssh/SyncPlan.h), and nothing here
// touches a file until the user has read a summary that counts the deletions
// and the conflicts on their own lines.
namespace
{

// Recursive local walk, relative to `root`. Bounded in depth and in entries
// so a reparse loop or a comparison started at a filesystem root cannot run
// away before the user sees anything.
void WalkLocal(const std::wstring& root, const std::wstring& rel,
               std::vector<amber::SyncEntry>& out, int depth, size_t& budget)
{
    if (depth > 32 || budget == 0)
        return;
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (budget == 0)
            break;
        --budget;
        const std::wstring childRel =
            rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
        std::wstring slashed = childRel;
        for (wchar_t& c : slashed)
            if (c == L'\\')
                c = L'/';
        amber::SyncEntry e;
        e.path = Narrow(slashed);
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // A reparse point is reported as a link and never walked into.
        e.link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.size = e.dir ? 0
                       : ((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                          fd.nFileSizeLow);
        ULARGE_INTEGER ft;
        ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        e.mtime = static_cast<int64_t>(ft.QuadPart / 10000000ULL) - 11644473600LL;
        // Windows has no Unix mode to report. Reporting 0 rather than a
        // plausible-looking 0644 is what stops the planner believing it can
        // preserve permissions that were never there.
        e.mode = 0;
        out.push_back(e);
        if (e.dir && !e.link)
            WalkLocal(root, childRel, out, depth + 1, budget);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

void WalkRemote(SftpClient& c, const std::string& root, const std::string& rel,
                std::vector<amber::SyncEntry>& out, int depth, size_t& budget)
{
    if (depth > 32 || budget == 0)
        return;
    const std::string dir = rel.empty() ? root : SftpJoin(root, rel);
    std::vector<SftpEntry> entries;
    std::string err;
    if (!c.List(dir, entries, err))
        return;
    for (const SftpEntry& s : entries)
    {
        if (s.name == "." || s.name == "..")
            continue;
        if (budget == 0)
            break;
        --budget;
        amber::SyncEntry e;
        e.path = rel.empty() ? s.name : rel + "/" + s.name;
        e.dir = s.dir;
        e.link = s.link;
        e.size = s.dir ? 0 : s.size;
        e.mtime = static_cast<int64_t>(s.mtime);
        e.mode = s.perms & 0777;
        out.push_back(e);
        if (e.dir && !e.link)
            WalkRemote(c, root, e.path, out, depth + 1, budget);
    }
}

} // namespace

void Browser::QueuePair(Tab& t, bool download, const std::wstring& local,
                        const std::string& remote, uint64_t bytes)
{
    // A download needs its parent directory to exist; the plan created the
    // directory steps but they are not executed as transfers.
    if (download)
    {
        const size_t slash = local.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            SHCreateDirectoryExW(nullptr, local.substr(0, slash).c_str(), nullptr);
    }
    auto x = std::make_shared<Transfer>();
    x->download = download;
    x->local = local;
    x->remote = remote;
    const size_t slash = local.find_last_of(L"\\/");
    x->name = slash == std::wstring::npos ? local : local.substr(slash + 1);
    x->total = bytes;
    x->refreshLocal = download;
    x->refreshRemote = !download;
    x->verifyRequested = t.verifyTransfers;
    {
        std::lock_guard<std::mutex> lk(t.xmx);
        x->id = t.nextXfer++;
        t.xfers.push_back(x);
    }
    RunTransfer(t, x);
}

void Browser::EditExcludes(Tab& t)
{
    std::string joined;
    for (const std::string& g : t.excludes)
        joined += (joined.empty() ? "" : ";") + g;
    std::wstring text = Widen(joined);
    if (!PromptString(hwnd, L"Comparison exclusions",
                      L"Exclude patterns (semicolon separated)", text))
        return;
    std::vector<std::string> next;
    const std::string all = Narrow(text);
    size_t pos = 0;
    while (pos <= all.size())
    {
        const size_t semi = all.find(';', pos);
        std::string one = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos
                                              : semi - pos);
        while (!one.empty() && (one.front() == ' ' || one.front() == '\t'))
            one.erase(one.begin());
        while (!one.empty() && (one.back() == ' ' || one.back() == '\t'))
            one.pop_back();
        if (!one.empty())
            next.push_back(one);
        if (semi == std::string::npos)
            break;
        pos = semi + 1;
    }
    t.excludes = std::move(next);
    SetStatusText(L"Excluding " + std::to_wstring(t.excludes.size()) +
                  L" patterns from comparisons.");
}

void Browser::CompareFolders(Tab& t)
{
    if (!t.connected)
    {
        SetStatusText(L"Not connected.");
        return;
    }
    if (t.lcwd.empty())
    {
        SetStatusText(L"Pick a local folder first (the left pane is on the "
                      L"drive list).");
        return;
    }
    const std::wstring localRoot = t.lcwd;
    const std::string remoteRoot = t.rcwd;
    const std::vector<std::string> excludes = t.excludes;
    const amber::SyncDirection dir = t.syncDir;
    const bool del = t.syncDelete;
    Worker* w = t.worker.get();
    HWND target = hwnd;
    SetStatusText(L"Comparing " + localRoot + L" with " + Widen(remoteRoot) + L"...");

    // The walk runs on the worker: a remote tree walk is a long sequence of
    // round trips and must not block the UI thread.
    w->Post([target, localRoot, remoteRoot, excludes, dir, del](SftpClient& c) {
        size_t budgetL = 200000, budgetR = 200000;
        std::vector<amber::SyncEntry> local, remote;
        WalkLocal(localRoot, L"", local, 0, budgetL);
        WalkRemote(c, remoteRoot, "", remote, 0, budgetR);
        const bool truncated = budgetL == 0 || budgetR == 0;

        amber::CompareOptions co;
        co.excludeGlobs = excludes;
        const std::vector<amber::ComparePair> pairs =
            amber::Compare(local, remote, co);
        amber::SyncOptions so;
        so.direction = dir;
        so.deleteExtraneous = del;

        // The plan is built here and handed over whole, so the summary the
        // user reads is the plan's own count rather than a second tally that
        // could disagree with it.
        auto* result = new std::pair<amber::SyncPlan, bool>(
            amber::BuildPlan(pairs, so), truncated);
        PostMessageW(target, WM_APP_COMPARE_DONE, 0,
                     reinterpret_cast<LPARAM>(result));
    });
}

void Browser::ShowPlan(Tab& t, const amber::SyncPlan& plan, bool truncated)
{
    if (plan.steps.empty())
    {
        SetStatusText(L"Nothing to do: the two folders match.");
        return;
    }
    size_t moves = 0, skips = 0, mkdirs = 0;
    uint64_t bytes = 0;
    for (const amber::SyncStep& s : plan.steps)
    {
        if (s.action == amber::SyncAction::Skip)
            ++skips;
        else if (s.action == amber::SyncAction::MkdirLocal ||
                 s.action == amber::SyncAction::MkdirRemote)
            ++mkdirs;
        else if (s.action != amber::SyncAction::Conflict &&
                 !amber::Deletes(s.action))
        {
            ++moves;
            bytes += s.bytes;
        }
    }

    const wchar_t* dirName =
        t.syncDir == amber::SyncDirection::LocalToRemote ? L"local → remote"
        : t.syncDir == amber::SyncDirection::RemoteToLocal ? L"remote → local"
                                                           : L"two-way";
    std::wstring body = L"Plan for ";
    body += dirName;
    body += L":\r\n\r\n    " + std::to_wstring(moves) + L" files to transfer (" +
            SizeText(bytes) + L")\r\n    " + std::to_wstring(mkdirs) +
            L" directories to create\r\n    " + std::to_wstring(skips) +
            L" already match or are left alone\r\n";
    if (plan.conflicts)
        body += L"    " + std::to_wstring(plan.conflicts) +
                L" CONFLICTS — skipped, not guessed at\r\n";
    if (plan.deletions)
        body += L"\r\n    " + std::to_wstring(plan.deletions) +
                L" extraneous files were found. They are LISTED but NOT "
                L"deleted.\r\n";
    if (truncated)
        body += L"\r\nThe trees are larger than the comparison limit, so this "
                L"plan is incomplete.\r\n";

    // A sample of the actual steps, so the counts are not the only thing
    // between the user and the action.
    body += L"\r\nFirst steps:\r\n";
    int shown = 0;
    for (const amber::SyncStep& s : plan.steps)
    {
        if (s.action == amber::SyncAction::Skip)
            continue;
        if (shown++ >= 12)
        {
            body += L"    ...\r\n";
            break;
        }
        body += L"    " + Widen(amber::SyncActionName(s.action)) + L"  " +
                Widen(s.path) + L"\r\n";
    }
    body += L"\r\nQueue the " + std::to_wstring(moves) + L" transfers?";

    if (MessageBoxW(hwnd, body.c_str(), L"AmberSSH — sync plan",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    {
        SetStatusText(L"Sync cancelled — nothing was changed.");
        return;
    }

    // Only transfers are queued. Deletions are deliberately NOT executed:
    // turning a comparison into a mirror needs its own confirmed action, not
    // a checkbox that was read minutes earlier in a different dialog.
    int queued = 0;
    for (const amber::SyncStep& s : plan.steps)
    {
        const bool up = s.action == amber::SyncAction::Upload ||
                        s.action == amber::SyncAction::ReplaceRemote;
        const bool down = s.action == amber::SyncAction::Download ||
                          s.action == amber::SyncAction::ReplaceLocal;
        if (!up && !down)
            continue;
        std::wstring wrel = Widen(s.path);
        for (wchar_t& ch : wrel)
            if (ch == L'/')
                ch = L'\\';
        QueuePair(t, down, t.lcwd + L"\\" + wrel, SftpJoin(t.rcwd, s.path),
                  s.bytes);
        ++queued;
    }
    std::wstring msg = L"Queued " + std::to_wstring(queued) + L" transfers.";
    if (plan.deletions)
        msg += L" No deletions were performed.";
    SetStatusText(msg);
}

} // namespace amber
