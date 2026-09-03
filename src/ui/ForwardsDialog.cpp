#include "ForwardsDialog.h"
#include "Theme.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace amber
{
namespace
{

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
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

enum { IdList = 100, IdType, IdListen, IdHost, IdPort, IdAdd, IdRemove, IdJump, IdOk, IdCancel };

struct Ui
{
    ConnectionProfile* profile = nullptr;
    std::vector<std::string> rules;
    HWND list = nullptr, type = nullptr, listen = nullptr, host = nullptr, port = nullptr, jump = nullptr;
    HBRUSH bg = nullptr;
    HFONT font = nullptr, mono = nullptr;
    bool ok = false;
};

std::wstring Describe(const std::string& r)
{
    if (r.empty()) return L"";
    char kind = r[0];
    std::string rest = r.substr(1);
    switch (kind)
    {
    case 'L': return L"Local    " + Widen(rest) + L"   (localhost:port → remote target)";
    case 'R': return L"Remote   " + Widen(rest) + L"   (server:port → local target)";
    case 'D': return L"Dynamic  SOCKS5 proxy on localhost:" + Widen(rest);
    }
    return Widen(r);
}

void Refill(Ui* ui)
{
    SendMessageW(ui->list, LB_RESETCONTENT, 0, 0);
    for (const std::string& r : ui->rules)
        SendMessageW(ui->list, LB_ADDSTRING, 0, (LPARAM)Describe(r).c_str());
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Ui* ui = (Ui*)GetWindowLongPtrW(h, GWLP_USERDATA);
    DialogPalette pal = MakeDialogPalette();
    switch (m)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); FillRect((HDC)w, &rc, ui->bg); return 1; }
    case WM_CTLCOLORSTATIC: SetBkColor((HDC)w, pal.bg); SetTextColor((HDC)w, pal.textDim); return (LRESULT)ui->bg;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: SetBkColor((HDC)w, pal.field); SetTextColor((HDC)w, pal.text);
        SetDCBrushColor((HDC)w, pal.field); return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_COMMAND:
    {
        int id = LOWORD(w);
        if (id == IdAdd)
        {
            int t = (int)SendMessageW(ui->type, CB_GETCURSEL, 0, 0);
            wchar_t lp[16], hs[256], pt[16];
            GetWindowTextW(ui->listen, lp, 16);
            GetWindowTextW(ui->host, hs, 256);
            GetWindowTextW(ui->port, pt, 16);
            int listenPort = _wtoi(lp), targetPort = _wtoi(pt);
            std::string rule;
            if (listenPort <= 0 || listenPort > 65535)
            {
                MessageBoxW(h, L"Enter a listen port (1-65535).", L"Port forwarding", MB_ICONWARNING);
                return 0;
            }
            if (t == 2)
                rule = "D" + std::to_string(listenPort);
            else
            {
                if (!*hs || targetPort <= 0)
                {
                    MessageBoxW(h, L"Enter the target host and port.", L"Port forwarding", MB_ICONWARNING);
                    return 0;
                }
                rule = std::string(t == 0 ? "L" : "R") + std::to_string(listenPort) + ":" +
                       Narrow(hs) + ":" + std::to_string(targetPort);
            }
            ui->rules.push_back(rule);
            Refill(ui);
            return 0;
        }
        if (id == IdRemove)
        {
            int sel = (int)SendMessageW(ui->list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)ui->rules.size())
            {
                ui->rules.erase(ui->rules.begin() + sel);
                Refill(ui);
            }
            return 0;
        }
        if (id == IdType && HIWORD(w) == CBN_SELCHANGE)
        {
            bool dyn = SendMessageW(ui->type, CB_GETCURSEL, 0, 0) == 2;
            EnableWindow(ui->host, !dyn);
            EnableWindow(ui->port, !dyn);
            return 0;
        }
        if (id == IdOk)
        {
            std::string spec;
            for (const std::string& r : ui->rules)
            {
                if (!spec.empty()) spec += ";";
                spec += r;
            }
            ui->profile->forwards = spec;
            wchar_t jh[256];
            GetWindowTextW(ui->jump, jh, 256);
            ui->profile->jumpHost = Narrow(jh);
            ui->ok = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == IdCancel) { DestroyWindow(h); return 0; }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

bool ForwardsDialog::Show(HWND owner, ConnectionProfile& profile)
{
    static bool reg = false;
    static const wchar_t cls[] = L"AmberSSHForwardsDialog";
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
    DialogPalette pal = MakeDialogPalette();
    Ui ui;
    ui.profile = &profile;
    // Parse the existing spec.
    {
        size_t pos = 0;
        while (pos <= profile.forwards.size())
        {
            size_t semi = profile.forwards.find(';', pos);
            std::string r = profile.forwards.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
            if (!r.empty()) ui.rules.push_back(r);
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
    }
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    auto px = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    ui.bg = CreateSolidBrush(pal.bg);
    ui.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ui.mono = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
    int w = px(620), h = px(470);
    RECT o = {}; if (owner) GetWindowRect(owner, &o);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls,
                               (L"Port Forwarding — " + Widen(profile.name.empty() ? profile.host : profile.name)).c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               o.left + ((o.right - o.left) - w) / 2,
                               o.top + ((o.bottom - o.top) - h) / 2, w, h, owner, nullptr,
                               GetModuleHandleW(nullptr), &ui);
    if (!dlg) return false;
    ApplyWindowChrome(dlg);
    auto mk = [&](const wchar_t* c, const wchar_t* t, DWORD st, int x, int y, int cw, int ch, int id, HFONT f) {
        HWND hc = CreateWindowExW(0, c, t, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cw), px(ch),
                                  dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hc, WM_SETFONT, (WPARAM)f, TRUE);
        return hc;
    };
    mk(L"STATIC", L"Tunnels (applied when the session connects):", SS_LEFT, 14, 12, 500, 20, -1, ui.font);
    ui.list = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | WS_TABSTOP,
                 14, 36, 590, 170, IdList, ui.mono);
    Refill(&ui);

    mk(L"STATIC", L"Type", SS_LEFT, 14, 220, 80, 18, -1, ui.font);
    ui.type = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 14, 240, 120, 120, IdType, ui.font);
    for (const wchar_t* t : { L"Local", L"Remote", L"Dynamic" })
        SendMessageW(ui.type, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(ui.type, CB_SETCURSEL, 0, 0);
    mk(L"STATIC", L"Listen port", SS_LEFT, 146, 220, 90, 18, -1, ui.font);
    ui.listen = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 146, 240, 90, 24, IdListen, ui.mono);
    mk(L"STATIC", L"Target host", SS_LEFT, 248, 220, 120, 18, -1, ui.font);
    ui.host = mk(L"EDIT", L"localhost", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 248, 240, 200, 24, IdHost, ui.mono);
    mk(L"STATIC", L"Target port", SS_LEFT, 460, 220, 90, 18, -1, ui.font);
    ui.port = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 460, 240, 70, 24, IdPort, ui.mono);
    mk(L"BUTTON", L"Add", WS_TABSTOP, 540, 239, 64, 26, IdAdd, ui.font);
    mk(L"BUTTON", L"Remove selected", WS_TABSTOP, 14, 276, 140, 26, IdRemove, ui.font);

    mk(L"STATIC", L"Jump host (user@host[:port], empty = direct):", SS_LEFT, 14, 320, 400, 18, -1, ui.font);
    ui.jump = mk(L"EDIT", Widen(profile.jumpHost).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                 14, 340, 590, 24, IdJump, ui.mono);
    mk(L"STATIC", L"Agent auth uses the running Windows OpenSSH agent or Pageant automatically.",
       SS_LEFT, 14, 372, 590, 18, -1, ui.font);

    mk(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 430, 400, 84, 28, IdOk, ui.font);
    mk(L"BUTTON", L"Cancel", WS_TABSTOP, 520, 400, 84, 28, IdCancel, ui.font);

    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DeleteObject(ui.bg); DeleteObject(ui.font); DeleteObject(ui.mono);
    return ui.ok;
}

} // namespace amber
