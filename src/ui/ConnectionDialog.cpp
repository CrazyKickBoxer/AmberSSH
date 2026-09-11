#include "ConnectionDialog.h"

#include <CommCtrl.h>
#include <shlobj.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <vector>

#include "../platform/CredentialStore.h"
#include "SafetyDialog.h"
#include "SkinDraw.h"
#include "SkinFinish.h"
#include "../platform/ConPty.h"
#include "Theme.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace amber
{
namespace
{

constexpr wchar_t kClassName[] = L"AmberSSHConnectionDialog";

int Dpi(int value, UINT dpi);

// Live theme palette — refreshed from amber::gThemeSrgb at each Show() so the
// connection manager always matches the active theme (amber, emerald, ice…).
COLORREF kBg, kField, kText, kTextDim, kTextDis, kBorder, kBorderHot, kAccent,
    kAccentTx, kSelBg, kSelText, kBanner, kBanner2;

void RefreshPalette()
{
    amber::DialogPalette p = amber::MakeDialogPalette();
    kBg = p.bg;         kField = p.field;      kText = p.text;
    kTextDim = p.textDim; kTextDis = p.textDis; kBorder = p.border;
    kBorderHot = p.borderHot; kAccent = p.accent; kAccentTx = p.accentText;
    kSelBg = p.selBg;   kSelText = p.selText;  kBanner = p.banner;
    kBanner2 = p.accent;
}

// Scale a COLORREF towards black (f < 1) or white-ish (f > 1).
COLORREF Dim(COLORREF c, float f)
{
    auto ch = [&](int v) { return (std::min)(255, (int)(v * f + 0.5f)); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

// Owner-drawn check / radio state (the skin draws them itself).
constexpr wchar_t kKindProp[] = L"amberKind";      // 1 check, 2 radio
constexpr wchar_t kCheckProp[] = L"amberChecked";  // non-null = checked

bool Skinned() { return amber::ChromeSkinned(); }
bool Pills() { return amber::Chrome().pills; }
// Hairline skins (Blueprint) draw every shape as an outline and never fill.
bool Hairline() { return Skinned() && amber::Chrome().outline > 0.0f; }
bool UpCase() { return Skinned() && amber::Chrome().uppercase; }

// Label text in the skin's case.
std::wstring SkinLabel(std::wstring s)
{
    if (UpCase())
        for (wchar_t& c : s)
            c = static_cast<wchar_t>(::towupper(c));
    return s;
}

// Diagonal hazard striping inside a rect: a warning ground with black bars
// laid over it. Used by skins that mark destructive controls.
void HazardFill(HDC dc, const RECT& rc, COLORREF warn, UINT dpi)
{
    HBRUSH wb = CreateSolidBrush(warn);
    FillRect(dc, &rc, wb);
    DeleteObject(wb);
    const int h = rc.bottom - rc.top;
    const int pitch = Dpi(12, dpi);
    const int barW = Dpi(5, dpi);
    HBRUSH bb = CreateSolidBrush(RGB(0, 0, 0));
    HGDIOBJ ob = SelectObject(dc, bb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HRGN clip = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
    SelectClipRgn(dc, clip);
    for (int x = rc.left - h; x < rc.right; x += pitch)
    {
        POINT p[4] = { { x, rc.bottom }, { x + h, rc.top },
                       { x + h + barW, rc.top }, { x + barW, rc.bottom } };
        Polygon(dc, p, 4);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(bb);
}

// Chamfered outline / fill (cut top-left and bottom-right corners) when the
// skin asks for it, otherwise a rounded rectangle.
void SkinShape(HDC dc, const RECT& rc, int cut, int radius)
{
    if (Hairline())
    {
        // Blueprint: a drafted rectangle. The caller has already selected a
        // hollow brush for outlines; for fills we still want a rectangle, so
        // the shape is the same either way — only the ink differs.
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        return;
    }
    if (Pills())
    {
        // LCARS: fully rounded ends (capped so tall wells stay well-shaped).
        int r = (std::min)(static_cast<int>(rc.bottom - rc.top), 26);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
        return;
    }
    if (cut > 0)
    {
        POINT p[6] = {
            { rc.left + cut, rc.top }, { rc.right - 1, rc.top },
            { rc.right - 1, rc.bottom - 1 - cut }, { rc.right - 1 - cut, rc.bottom - 1 },
            { rc.left, rc.bottom - 1 }, { rc.left, rc.top + cut },
        };
        Polygon(dc, p, 6);
    }
    else
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
}

// HUD bracket ticks on a field's corners.
void HudTicks(HDC dc, const RECT& r, int len, COLORREF col)
{
    HBRUSH b = CreateSolidBrush(col);
    RECT t;
    auto fill = [&](int x, int y, int w, int h) { t = { x, y, x + w, y + h }; FillRect(dc, &t, b); };
    fill(r.left - 2, r.top - 2, len, 1);       fill(r.left - 2, r.top - 2, 1, len);
    fill(r.right + 1 - len, r.top - 2, len, 1); fill(r.right + 1, r.top - 2, 1, len);
    fill(r.left - 2, r.bottom + 1, len, 1);    fill(r.left - 2, r.bottom + 1 - len, 1, len);
    fill(r.right + 1 - len, r.bottom + 1, len, 1); fill(r.right + 1, r.bottom + 1 - len, 1, len);
    DeleteObject(b);
}

// Hover flag for owner-drawn buttons, tracked by a tiny subclass.
constexpr wchar_t kHoverProp[] = L"amberHover";

LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                           DWORD_PTR)
{
    switch (m)
    {
    case BM_SETSTYLE:
        // IsDialogMessage moves BS_DEFPUSHBUTTON between buttons as focus
        // changes, which would overwrite BS_OWNERDRAW and snap the button
        // back to the stock theme. The owner-draw look is authoritative.
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
        RemoveWindowSubclass(h, HoverProc, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// Control ids ---------------------------------------------------------------
enum : int
{
    IdTree = 100,
    IdStatus,
    IdSessionList,
    IdOpen, IdCancel, IdLoad, IdSave, IdDelete, IdProfileName,
    IdBrowseKey, IdClearCreds, IdSerialRefresh,

    // Fields that need special handling keep fixed ids; the rest are
    // assigned from IdFieldFirst upwards by DefineFields.
    IdHost = 200, IdPort, IdUser,
    IdProtocol = 210,           // 7 radios: 210..216
    IdCloseOnExit = 220,        // 3 radios: 220..222
    IdAuth = 300,               // 4 radios: 300..303
    IdPassword = 310, IdRememberPassword, IdKeyPath, IdPassphrase,
    IdRememberPassphrase,
    IdProxyType = 320,          // 4 radios: 320..323
    IdProxyPassword = 330, IdRememberProxyPassword,
    IdSerialPort = 340,
    IdLocalShell = 350, IdLocalExe,
    IdReconnectMode = 360,      // 3 radios: 360..362
    IdReattachMode = 370,       // 4 radios: 370..373
    IdReattachSession = 380, IdReattachCommand,
    // The VNC page has its own password control: the same profile secret
    // (SecretKind::Password) and the same remember flag as SSH > Auth, so
    // a VNC profile never has two passwords, only one shown where it is used.
    IdVncPassword = 390, IdRememberVncPassword,

    IdFieldFirst = 1000,
};

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring GetText(HWND h)
{
    int len = GetWindowTextLengthW(h);
    if (len <= 0)
        return {};
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize((size_t)len);
    return text;
}

void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

int Dpi(int value, UINT dpi) { return MulDiv(value, (int)dpi, 96); }

std::wstring CrLf(std::string s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            out += "\r\n";
        else
            out.push_back(s[i]);
    }
    return Widen(out);
}

std::string Lf(const std::wstring& w)
{
    std::string s = Narrow(w);
    std::string out;
    for (char c : s)
        if (c != '\r')
            out.push_back(c);
    return out;
}

// Serial ports present right now ("COM3", ...), sorted numerically.
std::vector<std::wstring> ListComPorts()
{
    std::vector<std::wstring> ports;
    std::vector<wchar_t> buf(65536);
    DWORD n = QueryDosDeviceW(nullptr, buf.data(), (DWORD)buf.size());
    for (DWORD i = 0; i < n;)
    {
        std::wstring name(&buf[i]);
        i += (DWORD)name.size() + 1;
        if (name.size() > 3 && name.compare(0, 3, L"COM") == 0 &&
            iswdigit(name[3]))
            ports.push_back(name);
        if (name.empty())
            break;
    }
    std::sort(ports.begin(), ports.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wtoi(a.c_str() + 3) < _wtoi(b.c_str() + 3);
    });
    return ports;
}

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
               int id, HFONT font)
{
    // Labels and button captions carry the skin's case. Edit and list contents
    // are user data and are never transformed.
    std::wstring cased;
    if (text && (_wcsicmp(cls, L"STATIC") == 0 || _wcsicmp(cls, L"BUTTON") == 0))
    {
        cased = SkinLabel(text);
        text = cased.c_str();
    }
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (h && font)
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

HWND MakeEdit(HWND parent, int id, HFONT font, DWORD extra = 0)
{
    // No WS_BORDER: the dialog paints amber outlines (with a focus glow)
    // around every field itself.
    return MakeChild(parent, L"EDIT", L"",
                     WS_TABSTOP | ES_AUTOHSCROLL | extra, id, font);
}

HWND MakeLabel(HWND parent, const wchar_t* text, HFONT font)
{
    return MakeChild(parent, L"STATIC", text, SS_LEFT, -1, font);
}

HWND MakeButton(HWND parent, const wchar_t* text, int id, HFONT font,
                DWORD extra = 0)
{
    // Push buttons are owner-drawn (rounded amber); check boxes and radios
    // keep their auto styles but are un-themed so WM_CTLCOLORSTATIC can
    // color their text amber. Button styles are an enum in the low nibble
    // (BS_TYPEMASK), not flag bits — compare, never AND.
    const DWORD type = extra & 0xF /*BS_TYPEMASK*/;
    const int kind = type == BS_AUTOCHECKBOX ? 1 : type == BS_AUTORADIOBUTTON ? 2 : 0;
    // Every button is owner-drawn: the skin draws push buttons, check boxes
    // and radio buttons itself; check/radio state lives in window props.
    extra = (extra & ~(DWORD)(BS_DEFPUSHBUTTON | 0xF)) | BS_OWNERDRAW;
    HWND h = MakeChild(parent, L"BUTTON", text, WS_TABSTOP | extra, id, font);
    if (h)
    {
        if (kind)
            SetPropW(h, kKindProp, (HANDLE)(INT_PTR)kind);
        SetWindowSubclass(h, HoverProc, 1, 0);
    }
    return h;
}

} // namespace

// ---------------------------------------------------------------------------

bool ConnectionDialog::Show(HWND owner, ProfileStore& store,
                            ConnectionRequest& requestOut)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES |
                                              ICC_STANDARD_CLASSES |
                                              ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    RefreshPalette();   // pick up the current theme before creating controls

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = (WNDPROC)WndProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // painted amber-dark in WM_ERASEBKGND
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc))
            return false;
        registered = true;
    }

    ConnectionDialog dlg(store, requestOut);
    dlg.m_dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();

    int w = Dpi(820, dlg.m_dpi);
    int h = Dpi(700, dlg.m_dpi);

    RECT ownerRect = {};
    if (owner)
        GetWindowRect(owner, &ownerRect);
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - w) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - h) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClassName, L"AmberSSH — Connection",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h,
        owner, nullptr, GetModuleHandleW(nullptr), &dlg);
    if (!hwnd)
        return false;

    // Dark titlebar, themed caption/border, and Termius-style rounded corners.
    amber::ApplyWindowChrome(hwnd);

    if (owner)
        EnableWindow(owner, FALSE);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0))
    {
        // Enter is handled here rather than via DM_GETDEFID: answering that
        // query makes IsDialogMessage re-style the default button with
        // BS_DEFPUSHBUTTON, which destroys the owner-drawn amber look. Enter
        // on a button presses that button; in a multi-line edit it inserts a
        // line; anywhere else it means Open.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            IsWindow(hwnd) && (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd)))
        {
            wchar_t cls[16] = L"";
            GetClassNameW(msg.hwnd, cls, 16);
            const bool multi = _wcsicmp(cls, L"Edit") == 0 &&
                               (GetWindowLongW(msg.hwnd, GWL_STYLE) & ES_MULTILINE);
            if (!multi)
            {
                int id = (_wcsicmp(cls, L"Button") == 0) ? GetDlgCtrlID(msg.hwnd)
                                                         : IdOpen;
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                             (LPARAM)msg.hwnd);
                continue;
            }
        }
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return dlg.m_accepted;
}

INT_PTR CALLBACK ConnectionDialog::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp,
                                                LPARAM lp)
{
    ConnectionDialog* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ConnectionDialog*>(cs->lpCreateParams);
        self->m_dlg = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    else
    {
        self = reinterpret_cast<ConnectionDialog*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->Proc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ConnectionDialog::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        DefineFields();
        BuildControls(hwnd);
        Layout();
        WriteFields(ConnectionProfile{});   // defaults into every control
        SyncAuthEnabled();
        SyncGuardianEnabled();
        ShowPage(Page::Session);
        RefreshSessionList();
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_DPICHANGED:
    {
        m_dpi = HIWORD(wp);
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }

    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, m_bgBrush);
        if (amber::Chrome().scanlines)
        {
            // Faint CRT scanlines across the whole dialog ground.
            HBRUSH sl = CreateSolidBrush(Dim(kBorderHot, 0.10f));
            for (int y = rc.top; y < rc.bottom; y += 4)
            {
                RECT l = { rc.left, y, rc.right, y + 1 };
                FillRect((HDC)wp, &l, sl);
            }
            DeleteObject(sl);
        }
        return 1;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintChrome(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kText);
        SetBkColor(dc, kField);
        return (LRESULT)m_fieldBrush;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        HWND h = (HWND)lp;
        wchar_t cls[16] = L"";
        GetClassNameW(h, cls, 16);
        if (_wcsicmp(cls, L"Edit") == 0)
        {
            // Disabled edits arrive here: keep the field well, dim the text.
            SetTextColor(dc, kTextDis);
            SetBkColor(dc, kField);
            return (LRESULT)m_fieldBrush;
        }
        SetBkColor(dc, kBg);
        if (!IsWindowEnabled(h))
            SetTextColor(dc, kTextDis);
        else if (_wcsicmp(cls, L"Button") == 0)
            SetTextColor(dc, kText);        // classic checkbox / radio text
        else
            SetTextColor(dc, h == m_status ? RGB(234, 172, 92) : kTextDim);
        return (LRESULT)m_bgBrush;
    }

    case WM_CTLCOLORBTN:
        return (LRESULT)m_bgBrush;

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis->CtlType == ODT_BUTTON)
        {
            DrawThemedButton(*dis);
            return TRUE;
        }
        if (dis->CtlType == ODT_LISTBOX)
        {
            DrawSessionRow(*dis);
            return TRUE;
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == IdTree && hdr->code == TVN_SELCHANGEDW)
        {
            auto* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
            ShowPage(static_cast<Page>(tv->itemNew.lParam));
            return 0;
        }
        if (hdr->idFrom == IdTree && hdr->code == NM_CUSTOMDRAW)
        {
            auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(lp);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                const bool sel = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
                if (Pills())
                {
                    // LCARS sidebar: every category is a coloured block with
                    // black lettering; the selected one is orange.
                    int ih = (std::max)(1, (int)SendMessageW(m_tree, TVM_GETITEMHEIGHT, 0, 0));
                    int row = cd->nmcd.rc.top / ih;
                    const amber::ChromeSpec& ch = amber::Chrome();
                    cd->clrText = RGB(0, 0, 0);
                    cd->clrTextBk = sel ? kBorderHot : amber::SrgbRef(ch.bars[row % 4]);
                    return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                }
                if (sel && (amber::skin::Stitch() || amber::skin::Glaze()))
                {
                    // Atelier: a second-cut patch. Tenmoku: the pooled tile.
                    // Both keep the body ink; the mark is added in postpaint.
                    cd->clrTextBk = amber::SrgbRef(amber::Chrome().bars[1]);
                    cd->clrText = kText;
                    return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                }
                if (sel && amber::Chrome().darkText)
                {
                    // A skin that letters in black on colour cannot select by
                    // washing the ground towards the accent — gold at 22% on
                    // mask green is mud. The selected row becomes a plated pad
                    // instead: the full accent, with ink chosen by its luma.
                    cd->clrTextBk = kBorderHot;
                    cd->clrText = amber::InkOn(amber::Chrome().neonA);
                    return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                }
                cd->clrText = sel ? kSelText : kTextDim;
                cd->clrTextBk = sel ? kSelBg : kBg;
                return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT)
            {
                // Skinned: the stock [+]/[-] expander box is the one piece of
                // the tree the OS still draws. Cover it with the row's own
                // ground and put a chevron in its place.
                if (Skinned())
                {
                    HTREEITEM it = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
                    if (it && TreeView_GetChild(m_tree, it))
                    {
                        RECT tr;
                        TreeView_GetItemRect(m_tree, it, &tr, TRUE);
                        const int ind = static_cast<int>(TreeView_GetIndent(m_tree));
                        const int cx = tr.left - ind / 2;
                        const int cy = (tr.top + tr.bottom) / 2;
                        const int s = Dpi(4, m_dpi);
                        if (cx - s - 3 >= cd->nmcd.rc.left)
                        {
                            HDC hdc = cd->nmcd.hdc;
                            const COLORREF ground = GetPixel(hdc, tr.left + 1, tr.top + 2);
                            RECT box = { cx - s - 3, cy - s - 3, cx + s + 4, cy + s + 4 };
                            HBRUSH gb = CreateSolidBrush(ground);
                            FillRect(hdc, &box, gb);
                            DeleteObject(gb);
                            const bool open = (TreeView_GetItemState(m_tree, it, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
                            const bool selRow = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
                            HPEN cp = CreatePen(PS_SOLID, (std::max)(1, Dpi(1, m_dpi)),
                                                selRow ? amber::InkOn((GetRValue(ground) << 16) | (GetGValue(ground) << 8) | GetBValue(ground))
                                                       : kTextDim);
                            HGDIOBJ op = SelectObject(hdc, cp);
                            if (open)
                            {
                                MoveToEx(hdc, cx - s, cy - s / 2, nullptr);
                                LineTo(hdc, cx, cy + s / 2);
                                LineTo(hdc, cx + s + 1, cy - s / 2 - 1);
                            }
                            else
                            {
                                MoveToEx(hdc, cx - s / 2, cy - s, nullptr);
                                LineTo(hdc, cx + s / 2, cy);
                                LineTo(hdc, cx - s / 2 - 1, cy + s + 1);
                            }
                            SelectObject(hdc, op);
                            DeleteObject(cp);
                        }
                    }
                }
                if (Pills())
                {
                    // LCARS sidebar: the gutter is CONSTANT and each block
                    // carries a numeric tag at its right edge, which is what
                    // an Okudagram uses the empty end of a block for.
                    RECT r = cd->nmcd.rc;
                    RECT gap = { r.left, r.bottom - Dpi(4, m_dpi), r.right, r.bottom };
                    FillRect(cd->nmcd.hdc, &gap, m_bgBrush);
                    RECT cap = { r.right - Dpi(4, m_dpi), r.top, r.right, r.bottom };
                    FillRect(cd->nmcd.hdc, &cap, m_bgBrush);

                    int ih = (std::max)(1, (int)SendMessageW(m_tree, TVM_GETITEMHEIGHT, 0, 0));
                    int row = cd->nmcd.rc.top / ih;
                    wchar_t tag[16];
                    swprintf(tag, 16, L"%02d-%04d", row + 1,
                             1000 + (row * 1373) % 9000);
                    RECT tr = { r.left, r.top, r.right - Dpi(10, m_dpi),
                                r.bottom - Dpi(4, m_dpi) };
                    HGDIOBJ of = SelectObject(cd->nmcd.hdc, m_tagFont);
                    int obk = SetBkMode(cd->nmcd.hdc, TRANSPARENT);
                    COLORREF oc = SetTextColor(cd->nmcd.hdc, RGB(0, 0, 0));
                    DrawTextW(cd->nmcd.hdc, tag, -1, &tr,
                              DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
                    SetTextColor(cd->nmcd.hdc, oc);
                    SetBkMode(cd->nmcd.hdc, obk);
                    SelectObject(cd->nmcd.hdc, of);
                    return CDRF_DODEFAULT;
                }
                if (cd->nmcd.uItemState & CDIS_SELECTED)
                {
                    const RECT& r = cd->nmcd.rc;
                    const int my = (r.top + r.bottom) / 2;
                    if (amber::skin::Stitch())
                    {
                        // Atelier: the patch is riveted. A row is too shallow to stitch
                        // without the thread crossing the label.
                        amber::skin::Rivet(cd->nmcd.hdc, r.right - Dpi(10, m_dpi), my,
                                           Dpi(3, m_dpi), kAccent, RGB(0x5C, 0x45, 0x26));
                        return CDRF_DODEFAULT;
                    }
                    if (amber::skin::Glaze())
                    {
                        // Tenmoku: the tile is chopped.
                        amber::skin::Chop(cd->nmcd.hdc, r.right - Dpi(16, m_dpi),
                                          my - Dpi(5, m_dpi), Dpi(10, m_dpi),
                                          amber::SrgbRef(amber::Chrome().danger),
                                          amber::SrgbRef(amber::Chrome().bars[1]));
                        return CDRF_DODEFAULT;
                    }
                }
                if ((cd->nmcd.uItemState & CDIS_SELECTED) && amber::skin::Rehaut())
                {
                    // Horologe: the selected row is an applied index on lume —
                    // raised by a rhodium line above and a shade below.
                    amber::skin::AppliedIndex(cd->nmcd.hdc, cd->nmcd.rc, kText,
                                              amber::skin::Dim(kBg, 0.45f));
                    return CDRF_DODEFAULT;
                }
                // Skin: a neon bar marks the selected category.
                if ((cd->nmcd.uItemState & CDIS_SELECTED) && Skinned())
                {
                    RECT r = cd->nmcd.rc;
                    r.right = r.left + Dpi(3, m_dpi);
                    HBRUSH b = CreateSolidBrush(kBorderHot);
                    FillRect(cd->nmcd.hdc, &r, b);
                    DeleteObject(b);
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == IdSessionList && code == LBN_DBLCLK)
        {
            LoadSelectedProfile();
            return 0;
        }
        if (id == IdSessionList && code == LBN_SELCHANGE)
            return 0;

        if (code == EN_SETFOCUS || code == EN_KILLFOCUS ||
            code == LBN_SETFOCUS || code == LBN_KILLFOCUS)
        {
            InvalidateRect(hwnd, nullptr, FALSE);   // refresh focus outlines
            return 0;
        }

        if (code != BN_CLICKED && code != CBN_SELCHANGE)
            return 0;

        // Owner-drawn check / radio: toggle or select, then fall through to
        // the id-specific handlers (protocol / auth sync).
        if (code == BN_CLICKED && lp && GetPropW((HWND)lp, kKindProp))
        {
            HWND hb = (HWND)lp;
            int kind = (int)(INT_PTR)GetPropW(hb, kKindProp);
            if (kind == 1)
                SetPropW(hb, kCheckProp, GetPropW(hb, kCheckProp) ? nullptr : (HANDLE)1);
            else
            {
                bool grouped = false;
                for (Field& f : m_fields)
                {
                    if (f.kind != Kind::RadioRow)
                        continue;
                    if (std::find(f.ctrls.begin(), f.ctrls.end(), hb) == f.ctrls.end())
                        continue;
                    grouped = true;
                    for (HWND o : f.ctrls)
                    {
                        SetPropW(o, kCheckProp, o == hb ? (HANDLE)1 : nullptr);
                        InvalidateRect(o, nullptr, TRUE);
                    }
                }
                if (!grouped)
                    SetPropW(hb, kCheckProp, (HANDLE)1);
            }
            InvalidateRect(hb, nullptr, TRUE);
        }

        if (id >= IdProtocol && id < IdProtocol + 7)   // seven radios: SSH .. VNC
        {
            SyncProtocol();
            return 0;
        }
        if (id >= IdAuth && id < IdAuth + 4)
        {
            SyncAuthEnabled();
            return 0;
        }
        if (id >= IdReattachMode && id < IdReattachMode + 4)
        {
            SyncGuardianEnabled();
            return 0;
        }
        switch (id)
        {
        case IdOpen:
            if (CollectRequest())
            {
                m_accepted = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case IdCancel:
            m_accepted = false;
            DestroyWindow(hwnd);
            return 0;
        case IdLoad:   LoadSelectedProfile(); return 0;
        case IdSave:   SaveCurrentProfile();  return 0;
        case IdDelete: DeleteSelectedProfile(); return 0;
        case IdBrowseKey: BrowseForKey(); return 0;
        case IdSerialRefresh:
        {
            if (Field* f = FindField(IdSerialPort))
            {
                HWND cb = f->ctrls.empty() ? nullptr : f->ctrls[0];
                if (cb)
                {
                    std::wstring cur = GetText(cb);
                    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
                    for (const std::wstring& p : ListComPorts())
                        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)p.c_str());
                    SetText(cb, cur);
                    SetStatus(L"Serial ports refreshed.");
                }
            }
            return 0;
        }
        case IdClearCreds:
        {
            if (!m_selectedProfileId.empty())
            {
                CredentialStore::Erase(m_selectedProfileId, SecretKind::Password);
                CredentialStore::Erase(m_selectedProfileId, SecretKind::KeyPassphrase);
                CredentialStore::Erase(m_selectedProfileId, SecretKind::ProxyPassword);
                SetStatus(L"Saved credentials cleared for this profile.");
            }
            else
            {
                SetStatus(L"Load a saved session first.");
            }
            return 0;
        }
        default: return 0;
        }
    }

    case WM_CLOSE:
        m_accepted = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        for (HFONT* f : { &m_font, &m_monoFont, &m_headerFont, &m_tagFont })
            if (*f)
            {
                DeleteObject(*f);
                *f = nullptr;
            }
        for (HBRUSH* b : { &m_bgBrush, &m_fieldBrush })
            if (*b)
            {
                DeleteObject(*b);
                *b = nullptr;
            }
        // Deliberately no PostQuitMessage here: Show()'s loop ends when
        // IsWindow() goes false, and a posted WM_QUIT would leak into the
        // application's main loop and terminate the whole process the moment
        // the dialog closes.
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------- field table

void ConnectionDialog::DefineFields()
{
    using P = ConnectionProfile;
    int nextId = IdFieldFirst;
    auto idFor = [&](int fixed, int count) {
        if (fixed)
            return fixed;
        int id = nextId;
        nextId += count;
        return id;
    };
    auto push = [&](Field f) { m_fields.push_back(std::move(f)); };

    auto str = [&](Page pg, std::string P::*m, const wchar_t* label, int width = -1,
                   int fixedId = 0, Kind kind = Kind::Edit) {
        Field f;
        f.page = pg; f.kind = kind; f.id = idFor(fixedId, 1); f.label = label; f.width = width;
        f.get = [m](const P& p) { return Widen(p.*m); };
        f.set = [m](P& p, const std::wstring& v) { p.*m = Narrow(v); };
        push(std::move(f));
    };
    auto multi = [&](Page pg, std::string P::*m, const wchar_t* label) {
        Field f;
        f.page = pg; f.kind = Kind::Multi; f.id = idFor(0, 1); f.label = label;
        f.get = [m](const P& p) { return CrLf(p.*m); };
        f.set = [m](P& p, const std::wstring& v) { p.*m = Lf(v); };
        push(std::move(f));
    };
    auto num = [&](Page pg, int P::*m, const wchar_t* label, int width = 90, int fixedId = 0) {
        Field f;
        f.page = pg; f.kind = Kind::Number; f.id = idFor(fixedId, 1); f.label = label; f.width = width;
        f.get = [m](const P& p) { return std::to_wstring(p.*m); };
        f.set = [m](P& p, const std::wstring& v) { p.*m = _wtoi(v.c_str()); };
        push(std::move(f));
    };
    auto chk = [&](Page pg, bool P::*m, const wchar_t* label, int fixedId = 0) {
        Field f;
        f.page = pg; f.kind = Kind::Check; f.id = idFor(fixedId, 1); f.label = label;
        f.get = [m](const P& p) { return std::wstring(p.*m ? L"1" : L"0"); };
        f.set = [m](P& p, const std::wstring& v) { p.*m = (v == L"1"); };
        push(std::move(f));
    };
    // Index-valued (enum / int) radio row or dropdown.
    auto choice = [&](Page pg, Kind kind, auto P::*m, const wchar_t* label,
                      std::vector<std::wstring> opts, int fixedId = 0, int width = -1) {
        using T = std::remove_reference_t<decltype(std::declval<P&>().*m)>;
        Field f;
        f.page = pg; f.kind = kind; f.id = idFor(fixedId, (int)opts.size());
        f.label = label; f.options = std::move(opts); f.width = width;
        int n = (int)f.options.size();
        f.get = [m](const P& p) { return std::to_wstring(static_cast<int>(p.*m)); };
        f.set = [m, n](P& p, const std::wstring& v) {
            p.*m = static_cast<T>(std::clamp(_wtoi(v.c_str()), 0, n - 1));
        };
        push(std::move(f));
    };
    auto note = [&](Page pg, const wchar_t* text) {
        Field f;
        f.page = pg; f.kind = Kind::Note; f.id = idFor(0, 1); f.label = text;
        push(std::move(f));
    };
    auto secret = [&](Page pg, int fixedId, const wchar_t* label) {
        Field f;
        f.page = pg; f.kind = Kind::Password; f.id = fixedId; f.label = label;
        push(std::move(f));
    };

    // ---- Session ---------------------------------------------------------
    str(Page::Session, &P::host, L"Host name or IP address", -1, IdHost);
    num(Page::Session, &P::port, L"Port", 90, IdPort);
    choice(Page::Session, Kind::RadioRow, &P::protocol, L"Connection type",
           { L"SSH", L"Telnet", L"Rlogin", L"Raw", L"Serial", L"Local", L"VNC" }, IdProtocol);
    str(Page::Session, &P::username, L"Username (auto-login)", -1, IdUser);
    choice(Page::Session, Kind::RadioRow, &P::closeOnExit, L"Close window on exit",
           { L"Always", L"Never", L"Only on clean exit" }, IdCloseOnExit);

    // ---- Logging ---------------------------------------------------------
    choice(Page::Logging, Kind::RadioRow, &P::logMode, L"Session logging",
           { L"None", L"Printable output", L"All session output" });
    str(Page::Logging, &P::logFile, L"Log file name (&&Y &&M &&D date, &&T time, &&H host, &&P port)");
    chk(Page::Logging, &P::logAppend, L"Append to an existing log file (otherwise overwrite)");
    chk(Page::Logging, &P::logFlush, L"Flush the log file frequently");
    note(Page::Logging, L"Logging starts when the session opens. Ctrl+Shift+L still toggles\r\n"
                        L"ad-hoc logging for any tab.");

    // ---- Terminal --------------------------------------------------------
    chk(Page::Terminal, &P::autoWrap, L"Auto wrap mode initially on");
    chk(Page::Terminal, &P::implicitCr, L"Implicit CR in every LF");
    chk(Page::Terminal, &P::implicitLf, L"Implicit LF in every CR");
    choice(Page::Terminal, Kind::RadioRow, &P::localEcho, L"Local echo",
           { L"Auto", L"Force off", L"Force on" });
    choice(Page::Terminal, Kind::RadioRow, &P::localLineEdit, L"Local line editing",
           { L"Auto", L"Force off", L"Force on" });
    str(Page::Terminal, &P::answerback, L"Answerback to ^E", 260);

    // ---- Keyboard --------------------------------------------------------
    {
        Field f;
        f.page = Page::Keyboard; f.kind = Kind::RadioRow; f.id = idFor(0, 2);
        f.label = L"The Backspace key"; f.options = { L"Control-H", L"Control-? (127)" };
        f.get = [](const P& p) { return std::wstring(p.backspaceIsDel ? L"1" : L"0"); };
        f.set = [](P& p, const std::wstring& v) { p.backspaceIsDel = (v == L"1"); };
        push(std::move(f));
    }
    choice(Page::Keyboard, Kind::RadioRow, &P::homeEnd, L"The Home and End keys",
           { L"xterm (ESC[H / ESC[F)", L"Standard (ESC[1~ / ESC[4~)", L"rxvt (ESC[H / ESC Ow)" });
    choice(Page::Keyboard, Kind::RadioRow, &P::fnKeys, L"The Function keys and keypad",
           { L"xterm", L"Linux", L"VT100+", L"SCO" });
    {
        Field f;
        f.page = Page::Keyboard; f.kind = Kind::RadioRow; f.id = idFor(0, 2);
        f.label = L"Initial state of cursor keys"; f.options = { L"Normal", L"Application" };
        f.get = [](const P& p) { return std::wstring(p.appCursorInitial ? L"1" : L"0"); };
        f.set = [](P& p, const std::wstring& v) { p.appCursorInitial = (v == L"1"); };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Keyboard; f.kind = Kind::RadioRow; f.id = idFor(0, 2);
        f.label = L"Initial state of numeric keypad"; f.options = { L"Normal", L"Application" };
        f.get = [](const P& p) { return std::wstring(p.appKeypadInitial ? L"1" : L"0"); };
        f.set = [](P& p, const std::wstring& v) { p.appKeypadInitial = (v == L"1"); };
        push(std::move(f));
    }
    note(Page::Keyboard, L"Shift+Backspace sends the other Backspace code.");

    // ---- Bell ------------------------------------------------------------
    choice(Page::Bell, Kind::RadioRow, &P::bell, L"Action to happen when a bell occurs",
           { L"None", L"Visual (shockwave)", L"System beep", L"Both" });
    chk(Page::Bell, &P::bellTaskbar, L"Flash the taskbar entry when the window is not active");
    chk(Page::Bell, &P::bellOverload, L"Bell is temporarily disabled when over-used (5 in 2 s, mute 5 s)");

    // ---- Features --------------------------------------------------------
    chk(Page::Features, &P::allowAppCursor, L"Allow application cursor keys mode");
    chk(Page::Features, &P::allowAppKeypad, L"Allow application keypad mode");
    chk(Page::Features, &P::allowMouse, L"Allow xterm-style mouse reporting");
    chk(Page::Features, &P::allowRemoteResize, L"Allow remote-controlled terminal resizing");
    chk(Page::Features, &P::allowAltScreen, L"Allow switching to the alternate terminal screen");
    chk(Page::Features, &P::allowRemoteTitle, L"Allow remote-controlled window title changing");
    // Only the remote-to-local direction exists over OSC 52, so this is three
    // options on the same numeric scale vncClipboard uses rather than five.
    choice(Page::Features, Kind::Combo, &P::allowRemoteClipboard, L"Remote clipboard (OSC 52)",
           { L"Disabled", L"Ask each time", L"Always allow" }, 0, 200);
    chk(Page::Features, &P::allowScrollbackClear, L"Allow remote-controlled clearing of scrollback");

    // ---- Window ----------------------------------------------------------
    num(Page::Window, &P::cols, L"Columns", 90);
    num(Page::Window, &P::rows, L"Rows", 90);
    choice(Page::Window, Kind::RadioRow, &P::resizeAction, L"When the window is resized",
           { L"Change the number of rows and columns", L"Change the size of the font", L"Forbid resizing" });
    num(Page::Window, &P::scrollbackLines, L"Lines of scrollback", 110);
    chk(Page::Window, &P::scrollOnKey, L"Reset scrollback on keypress");
    chk(Page::Window, &P::scrollOnOutput, L"Reset scrollback on display activity");

    // ---- Appearance ------------------------------------------------------
    choice(Page::Appearance, Kind::RadioRow, &P::cursor, L"Cursor appearance",
           { L"Block", L"Underline", L"Vertical line" });
    chk(Page::Appearance, &P::cursorBlink, L"Cursor blinks");
    str(Page::Appearance, &P::fontFamily, L"Font family (blank = global setting, e.g. JetBrains Mono)");
    {
        Field f;
        f.page = Page::Appearance; f.kind = Kind::Number; f.id = idFor(0, 1);
        f.label = L"Font size in px (0 = global setting)"; f.width = 90;
        f.get = [](const P& p) { return std::to_wstring((int)(p.fontSize + 0.5f)); };
        f.set = [](P& p, const std::wstring& v) { p.fontSize = (float)_wtoi(v.c_str()); };
        push(std::move(f));
    }
    num(Page::Appearance, &P::gapPx, L"Gap between text and window edge (px)", 90);

    // ---- Behaviour -------------------------------------------------------
    str(Page::Behaviour, &P::windowTitle, L"Window title (blank = session / remote title)");
    chk(Page::Behaviour, &P::warnOnClose, L"Warn before closing a connected session");
    chk(Page::Behaviour, &P::altF4Closes, L"Window closes on ALT-F4");
    chk(Page::Behaviour, &P::altSpaceMenu, L"System menu appears on ALT-Space");
    chk(Page::Behaviour, &P::altEnterFullscreen, L"Full screen on Alt-Enter");

    // ---- Translation -----------------------------------------------------
    choice(Page::Translation, Kind::Combo, &P::charset, L"Remote character set",
           { L"UTF-8", L"ISO-8859-1 (Latin-1)", L"Windows-1252", L"CP437 (DOS)" }, 0, 260);
    chk(Page::Translation, &P::poorMansLineDrawing, L"Poor man's line drawing (+, - and |)");

    // ---- Selection -------------------------------------------------------
    choice(Page::Selection, Kind::RadioRow, &P::mouseButtons, L"Action of mouse buttons",
           { L"Compromise (middle extends, right pastes)", L"Windows (middle extends, right menu)",
             L"xterm (right extends, middle pastes)" });
    chk(Page::Selection, &P::shiftOverridesMouse, L"Shift overrides application's use of the mouse");
    chk(Page::Selection, &P::rectSelectDefault, L"Default selection is rectangular (Alt+drag switches)");
    chk(Page::Selection, &P::autoCopy, L"Auto-copy selected text to the clipboard");

    // ---- Colours ---------------------------------------------------------
    {
        Field f;
        f.page = Page::Colours; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"ANSI palette"; f.width = 260;
        f.options = { L"Global setting", L"Amber Miami", L"Classic xterm" };
        f.get = [](const P& p) { return std::to_wstring(p.palette + 1); };
        f.set = [](P& p, const std::wstring& v) { p.palette = std::clamp(_wtoi(v.c_str()), 0, 2) - 1; };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Colours; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"Theme (order of the View > Theme menu)"; f.width = 260;
        f.options = { L"Global setting", L"Theme 1", L"Theme 2", L"Theme 3", L"Theme 4",
                      L"Theme 5", L"Theme 6", L"Theme 7 (custom)" };
        f.get = [](const P& p) { return std::to_wstring(std::clamp(p.themeId, -1, 6) + 1); };
        f.set = [](P& p, const std::wstring& v) { p.themeId = std::clamp(_wtoi(v.c_str()), 0, 7) - 1; };
        push(std::move(f));
    }
    chk(Page::Colours, &P::allowAnsiColours, L"Allow terminal to specify ANSI colours");
    chk(Page::Colours, &P::allow256Colours, L"Allow terminal to use xterm 256-colour and truecolour modes");
    choice(Page::Colours, Kind::RadioRow, &P::boldStyle, L"Indicate bolded text by changing",
           { L"The colour", L"The font", L"Both" });

    // ---- Connection ------------------------------------------------------
    num(Page::Connection, &P::connectTimeoutSeconds, L"Connection timeout (seconds)", 90);
    num(Page::Connection, &P::keepaliveSeconds, L"Seconds between keepalives (0 = off)", 90);
    chk(Page::Connection, &P::tcpNoDelay, L"Disable Nagle's algorithm (TCP_NODELAY)");
    chk(Page::Connection, &P::tcpKeepalive, L"Enable TCP keepalives (SO_KEEPALIVE)");
    choice(Page::Connection, Kind::RadioRow, &P::ipVersion, L"Internet protocol version",
           { L"Auto", L"IPv4", L"IPv6" });
    str(Page::Connection, &P::logicalHost, L"Logical name of remote host (for host-key storage)");
    note(Page::Connection, L"What happens when a connection drops is on the Guardian page.");

    // ---- Guardian --------------------------------------------------------
    choice(Page::Guardian, Kind::RadioRow, &P::reconnectMode,
           L"When a connected session loses its link",
           { L"Do nothing", L"Ask me", L"Reconnect automatically" }, IdReconnectMode);
    num(Page::Guardian, &P::reconnectMaxAttempts,
        L"Attempts before giving up (0 = keep trying until stopped)", 90);
    num(Page::Guardian, &P::reconnectJitterPercent,
        L"Backoff jitter, per cent (0-50)", 90);
    note(Page::Guardian, L"Waits grow 1s, 2s, 5s, 10s, 30s and then hold at 30s. Jitter spreads\r\n"
                         L"tabs apart so a fleet pointed at one server does not stampede it when\r\n"
                         L"the server or the VPN comes back.");
    note(Page::Guardian, L"Reconnect never resumes past a security question. A changed host key,\r\n"
                         L"a rejected key, a missing password or a failed authentication stops it\r\n"
                         L"and hands the decision to you. Reconnect only arms after a session has\r\n"
                         L"connected once: a failure on the first connect is a settings problem.");
    // Kept short: an uppercase skin renders these labels wider than the
    // lower-case source, and a long one runs off the right edge of the page.
    chk(Page::Guardian, &P::reconnectNotify, L"Notify me on the outcome");
    chk(Page::Guardian, &P::reconnectBanner, L"Mark the drop and the recovery");
    note(Page::Guardian, L"A notification is raised only when AmberSSH is in the background, and\r\n"
                         L"only when a session reconnects or gives up — never per failed attempt.\r\n"
                         L"The mark is drawn over the view, not written into the terminal, so it\r\n"
                         L"never appears in a copy, a search or a session log.");

    // ---- Guardian > Reattach ---------------------------------------------
    choice(Page::Reattach, Kind::RadioRow, &P::reattachMode,
           L"After reconnecting, rejoin",
           { L"Nothing (a new shell)", L"tmux", L"screen", L"A command I give" },
           IdReattachMode);
    str(Page::Reattach, &P::reattachSession, L"tmux / screen session name", 260,
        IdReattachSession);
    str(Page::Reattach, &P::reattachCommand,
        L"Custom reattach command (run once, after authentication)", -1,
        IdReattachCommand);
    note(Page::Reattach, L"tmux runs   tmux attach-session -t NAME || tmux new-session -s NAME\r\n"
                         L"screen runs screen -R NAME\r\n"
                         L"Both attach when the session exists and create it otherwise. Neither\r\n"
                         L"kills a session or detaches anybody else.");
    note(Page::Reattach, L"Processes that were running in a plain shell died with the old\r\n"
                         L"connection and cannot be brought back. Only a multiplexer already\r\n"
                         L"running on the server can carry work across a drop.");

    chk(Page::Reattach, &P::restoreCwd, L"Return to the last known directory");
    chk(Page::Reattach, &P::restoreForwards, L"Re-establish port forwards");
    note(Page::Reattach, L"The directory comes from OSC 7 and is skipped when reattaching, since\r\n"
                         L"a multiplexer brings its own panes back already in place. Forwards\r\n"
                         L"include the SOCKS proxies, and are rebuilt on the new connection.");

    // ---- Data ------------------------------------------------------------
    str(Page::Data, &P::termType, L"Terminal-type string", 260);
    str(Page::Data, &P::termSpeed, L"Terminal speeds (input,output)", 260);
    multi(Page::Data, &P::envVars, L"Environment variables (NAME=value, one per line)");
    note(Page::Data, L"The auto-login username is on the Session page.");

    // ---- Proxy -----------------------------------------------------------
    choice(Page::Proxy, Kind::RadioRow, &P::proxyType, L"Proxy type",
           { L"None", L"SOCKS 4", L"SOCKS 5", L"HTTP CONNECT" }, IdProxyType);
    str(Page::Proxy, &P::proxyHost, L"Proxy hostname", 300);
    num(Page::Proxy, &P::proxyPort, L"Port", 90);
    str(Page::Proxy, &P::proxyUser, L"Username", 260);
    secret(Page::Proxy, IdProxyPassword, L"Password");
    chk(Page::Proxy, &P::rememberProxyPassword, L"Remember proxy password (Credential Manager)", IdRememberProxyPassword);
    str(Page::Proxy, &P::proxyExclude, L"Exclude Hosts/IPs (comma-separated, * and ? wildcards)");
    chk(Page::Proxy, &P::proxyLocalhost, L"Consider proxying local host connections");
    chk(Page::Proxy, &P::proxyDns, L"Do DNS name lookup at proxy end");

    // ---- SSH -------------------------------------------------------------
    str(Page::Ssh, &P::remoteCommand, L"Remote command (blank = login shell)");
    chk(Page::Ssh, &P::noShell, L"Don't start a shell or command at all (tunnels only)");
    chk(Page::Ssh, &P::compression, L"Enable compression");
    str(Page::Ssh, &P::cipherPref, L"Cipher preference (comma list, blank = default: aes256-gcm@openssh.com,aes256-ctr,...)");
    str(Page::Ssh, &P::kexPref, L"Key exchange preference (blank = default: curve25519-sha256,ecdh-sha2-nistp256,...)");
    str(Page::Ssh, &P::hostKeyPref, L"Host key algorithm preference (blank = default: ssh-ed25519,ecdsa-sha2-nistp256,...)");
    str(Page::Ssh, &P::macPref, L"MAC preference (blank = default: hmac-sha2-256,hmac-sha2-512,...)");
    note(Page::Ssh, L"A list none of whose algorithms this build supports refuses the connection\r\n"
                    L"rather than quietly falling back to the defaults.");

    // ---- SSH > Auth ------------------------------------------------------
    choice(Page::SshAuth, Kind::RadioRow, &P::auth, L"Authentication method",
           { L"Password", L"Private key file", L"Keyboard-interactive", L"Agent / Pageant" }, IdAuth);
    secret(Page::SshAuth, IdPassword, L"Password");
    chk(Page::SshAuth, &P::rememberPassword, L"Remember password (Credential Manager)", IdRememberPassword);
    str(Page::SshAuth, &P::privateKeyPath, L"Private key file (OpenSSH format)", -1, IdKeyPath);
    secret(Page::SshAuth, IdPassphrase, L"Key passphrase");
    chk(Page::SshAuth, &P::rememberPassphrase, L"Remember passphrase (Credential Manager)", IdRememberPassphrase);
    chk(Page::SshAuth, &P::agentForward, L"Allow agent forwarding");
    note(Page::SshAuth, L"PuTTY .ppk keys are not loaded directly. Convert to OpenSSH format first.");

    // ---- SSH > X11 -------------------------------------------------------
    chk(Page::SshX11, &P::x11Forward, L"Enable X11 forwarding");
    choice(Page::SshX11, Kind::RadioRow, &P::x11Backend, L"X server",
           { L"External (VcXsrv, X410, ...)", L"AmberX built-in (experimental)" });
    str(Page::SshX11, &P::x11Display, L"X display location (e.g. localhost:0)", 260);
    choice(Page::SshX11, Kind::RadioRow, &P::x11Trust, L"X11 trust",
           {L"Restricted (default)", L"Trusted — saved", L"Trusted — this session only"});
    choice(Page::SshX11, Kind::Combo, &P::x11Clipboard, L"Clipboard (AmberX only)",
           {L"Disabled", L"Ask each transfer", L"Remote → local text",
            L"Local → remote text", L"Both directions, text"});
    note(Page::SshX11, L"Remote X clients are connected to the X server at that display\r\n"
                       L"(VcXsrv, Xming, WSLg). Start it before connecting.");

    // ---- SSH > Remote GUI (AmberX) ---------------------------------------
    // One page for the whole feature, in the order a person decides it: on or
    // off and how trusted, then where the windows go, then what may cross,
    // then how much screen, then how hard to work.
    choice(Page::RemoteGui, Kind::RadioRow, &P::remoteGui, L"Remote GUI",
           {L"Off", L"X11 Restricted", L"X11 Trusted"});
    choice(Page::RemoteGui, Kind::Combo, &P::windowMode, L"Window mode",
           {L"Native Windows windows", L"AmberSSH tabs (not yet)",
            L"AmberSSH panes (not yet)", L"Ask per application (not yet)"});
    choice(Page::RemoteGui, Kind::Combo, &P::x11Clipboard, L"Clipboard",
           {L"Disabled", L"Ask each transfer", L"Remote \x2192 local text",
            L"Local \x2192 remote text", L"Bidirectional text"});
    choice(Page::RemoteGui, Kind::Combo, &P::displayMode, L"Display",
           {L"All monitors", L"Active monitor only", L"Fixed virtual size"});
    num(Page::RemoteGui, &P::displayW, L"Fixed width", 90);
    num(Page::RemoteGui, &P::displayH, L"Fixed height", 90);
    choice(Page::RemoteGui, Kind::Combo, &P::perfMode, L"Performance",
           {L"Auto", L"Quality", L"Balanced", L"Low bandwidth"});
    note(Page::RemoteGui,
         L"Remote GUI runs AmberSSH's own X server (AmberX) in an isolated,\r\n"
         L"low-integrity process, one per session. Restricted is the default and\r\n"
         L"is enforced by the X SECURITY extension; Trusted asks for confirmation.\r\n"
         L"\r\n"
         L"Only native windows are implemented. Tabs and panes need a shared-surface\r\n"
         L"path that is not built yet; choosing one falls back to native windows and\r\n"
         L"says so in the session log.\r\n"
         L"\r\n"
         L"Performance caps how often forwarded windows repaint on this machine.\r\n"
         L"It does not compress the X11 stream: AmberX forwards the protocol as it is.");

    // ---- SSH > Tunnels ---------------------------------------------------
    multi(Page::SshTunnels, &P::forwards, L"Port forwards, one per line or ';'-separated:\r\n"
                                          L"L<port>:<host>:<port>   R<port>:<host>:<port>   D<port>");
    str(Page::SshTunnels, &P::jumpHost, L"Jump host (user@host[:port])");

    // ---- SSH > Host keys -------------------------------------------------
    multi(Page::SshHostKeys, &P::manualHostKeys, L"Manually configured host keys (SHA256 fingerprints, one per line)");
    note(Page::SshHostKeys, L"When any fingerprint is listed it replaces known_hosts for this session:\r\n"
                            L"a listed key is accepted silently, any other key is refused.");

    // ---- Serial ----------------------------------------------------------
    {
        Field f;
        f.page = Page::Serial; f.kind = Kind::ComboEdit; f.id = IdSerialPort;
        f.label = L"Serial line to connect to"; f.width = 200;
        f.options = ListComPorts();
        f.get = [](const P& p) { return Widen(p.serialPort); };
        f.set = [](P& p, const std::wstring& v) { p.serialPort = Narrow(v); };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Serial; f.kind = Kind::ComboEdit; f.id = idFor(0, 1);
        f.label = L"Speed (baud)"; f.width = 200;
        f.options = { L"1200", L"2400", L"4800", L"9600", L"19200", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600" };
        f.get = [](const P& p) { return std::to_wstring(p.serialBaud); };
        f.set = [](P& p, const std::wstring& v) { p.serialBaud = _wtoi(v.c_str()); };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Serial; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"Data bits"; f.width = 120; f.options = { L"5", L"6", L"7", L"8" };
        f.get = [](const P& p) { return std::to_wstring(std::clamp(p.serialDataBits, 5, 8) - 5); };
        f.set = [](P& p, const std::wstring& v) { p.serialDataBits = std::clamp(_wtoi(v.c_str()), 0, 3) + 5; };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Serial; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"Stop bits"; f.width = 120; f.options = { L"1", L"1.5", L"2" };
        f.get = [](const P& p) { return std::to_wstring(p.serialStopBits == 15 ? 1 : p.serialStopBits == 2 ? 2 : 0); };
        f.set = [](P& p, const std::wstring& v) { int i = _wtoi(v.c_str()); p.serialStopBits = i == 1 ? 15 : i == 2 ? 2 : 1; };
        push(std::move(f));
    }
    choice(Page::Serial, Kind::Combo, &P::serialParity, L"Parity",
           { L"None", L"Odd", L"Even", L"Mark", L"Space" }, 0, 120);
    choice(Page::Serial, Kind::Combo, &P::serialFlow, L"Flow control",
           { L"None", L"XON/XOFF", L"RTS/CTS", L"DSR/DTR" }, 0, 120);

    // ---- Local (ConPTY) ---------------------------------------------------
    // The shell list is discovered on this machine, so the choices name what
    // is actually installed. "Custom" hands control to the executable field.
    {
        Field f;
        f.page = Page::Local; f.kind = Kind::Combo; f.id = IdLocalShell;
        f.label = L"Shell";
        f.options.push_back(L"Custom (use the executable below)");
        for (const amber::LocalShell& s : amber::DiscoverLocalShells())
            f.options.push_back(WideFromUtf8(s.name));
        f.get = [](const P& p) {
            if (p.localShellKey.empty())
                return std::wstring(L"0");
            const std::vector<amber::LocalShell> shells = amber::DiscoverLocalShells();
            for (size_t i = 0; i < shells.size(); ++i)
                if (shells[i].key == p.localShellKey)
                    return std::to_wstring(i + 1);
            return std::wstring(L"0");
        };
        f.set = [](P& p, const std::wstring& v) {
            int idx = _wtoi(v.c_str());
            const std::vector<amber::LocalShell> shells = amber::DiscoverLocalShells();
            if (idx <= 0 || static_cast<size_t>(idx) > shells.size())
            {
                p.localShellKey.clear();
                return;
            }
            const amber::LocalShell& s = shells[static_cast<size_t>(idx - 1)];
            p.localShellKey = s.key;
            // Leave the executable and arguments blank so the shell is
            // resolved afresh at every launch; a user who types into those
            // fields is choosing to pin them.
            p.localExe.clear();
            p.localArgs.clear();
            if (p.name.empty())
                p.name = s.name;
        };
        push(std::move(f));
    }
    str(Page::Local, &P::localExe, L"Executable (blank = the shell above)", -1, IdLocalExe);
    str(Page::Local, &P::localArgs, L"Arguments");
    str(Page::Local, &P::localCwd, L"Starting directory (blank = your profile folder)");
    multi(Page::Local, &P::localEnv, L"Environment overrides (NAME=value, one per line)");
    chk(Page::Local, &P::localShellIntegration,
        L"Enable shell integration for this session (OSC 7 and OSC 133)");
    note(Page::Local, L"Shell integration is passed to the shell at launch and lasts only for\r\n"
                      L"this session. No dotfile is read or written. It gives the command\r\n"
                      L"journal, output folding and command jumping something to attach to.");
    note(Page::Local, L"Local sessions never run elevated. Start AmberSSH itself as\r\n"
                      L"administrator if you need an elevated shell.");

    // ---- VNC (docs/vnc.md) -------------------------------------------------
    secret(Page::Vnc, IdVncPassword, L"Password (VNC Authentication, or the VeNCrypt subtype's)");
    chk(Page::Vnc, &P::rememberPassword, L"Remember password (Credential Manager)", IdRememberVncPassword);
    chk(Page::Vnc, &P::vncViewOnly, L"View only (no keys, pointer or clipboard reach the server)");
    str(Page::Vnc, &P::vncViaProfileId,
        L"Tunnel through a connected SSH session (its profile name; blank = direct TCP)");
    choice(Page::Vnc, Kind::Combo, &P::vncTls, L"Encryption",
           { L"None (plain RFB; VNC Authentication protects only the password)",
             L"VeNCrypt / TLS with an X.509 server certificate (required, never downgraded)" });
    num(Page::Vnc, &P::vncDensity, L"Particles per pixel (1-4; reduced past the GPU budget, and said so)", 90);
    num(Page::Vnc, &P::vncSolidity, L"Solidity % (0 = loose swarm, 100 = exact reconstruction)", 90);
    num(Page::Vnc, &P::vncParticleSize, L"Particle size (1-3 px)", 90);
    num(Page::Vnc, &P::vncDisturbance, L"Disturbance % (how much a changed pixel stirs its particles)", 90);
    choice(Page::Vnc, Kind::Combo, &P::vncEncodings, L"Encodings",
           { L"ZRLE, Hextile, CopyRect, Raw", L"Hextile, ZRLE, CopyRect, Raw", L"Raw only" }, 0, 240);
    choice(Page::Vnc, Kind::Combo, &P::vncCursorMode, L"Cursor",
           { L"Local particle cursor, in the server's shape", L"The server draws the cursor into the picture" },
           0, 300);
    choice(Page::Vnc, Kind::Combo, &P::vncDesktopSize, L"Desktop size (asked of the server; it must allow it)",
           { L"The server's own", L"Fit this window, and follow it when it resizes", L"1280 \x00d7 720",
             L"1366 \x00d7 768", L"1600 \x00d7 900", L"1920 \x00d7 1080", L"2560 \x00d7 1440", L"Custom (below)" },
           0, 300);
    num(Page::Vnc, &P::vncCustomW, L"Custom width", 90);
    num(Page::Vnc, &P::vncCustomH, L"Custom height", 90);
    num(Page::Vnc, &P::vncGlow, L"Glow % (bloom on the desktop; the terminal's is 100)", 90);
    num(Page::Vnc, &P::vncVividness, L"Vividness % (saturation and contrast; 100 = the decoded colours)", 90);
    num(Page::Vnc, &P::vncMotion, L"Motion speed % (25-800; the swarm's drift, the motion style and the effects)", 90);
    choice(Page::Vnc, Kind::Combo, &P::vncTransition, L"Redraw style (how a changed region appears)",
           { L"None: the new pixels at once", L"Burn", L"Dissolve", L"Scan wipe", L"Emboss flash",
             L"Light speed: the particles fly in", L"Shear plates: blocks slide and turn as one",
             L"Iris: a front with a burning rim", L"Sonic boom: out, back, a flash on landing",
             L"Shatter and reform: glass shards", L"Odometer: the columns spin and stop" }, 0, 280);
    chk(Page::Vnc, &P::vncFxPrism, L"Motion: red and blue separate along a moving particle (prism)");
    chk(Page::Vnc, &P::vncFxTails, L"Motion: a streak's tail bends by the field it flew through");
    chk(Page::Vnc, &P::vncFxTrails, L"Motion: three sub-positions a frame (exposure, not a smear)");
    chk(Page::Vnc, &P::vncFxIgnite, L"Motion: energy travels as a front, conducted by the picture");
    chk(Page::Vnc, &P::vncFxShock, L"Effect: a click sends a shockwave (right click pulls inward)");
    choice(Page::Vnc, Kind::Combo, &P::vncShockStyle, L"Shockwave style",
           { L"Ring", L"Water drop (ripples)", L"Splash", L"Vortex" }, 0, 200);
    chk(Page::Vnc, &P::vncFxEdge, L"Effect: edges glow (window borders, text outlines)");
    chk(Page::Vnc, &P::vncFxHeat, L"Effect: changed pixels run hot and lift until they cool");
    chk(Page::Vnc, &P::vncFxMaterialise, L"Effect: the picture materialises on connect and resize");
    note(Page::Vnc, L"With any effect on the desktop is not pixel-exact even at solidity 100 (the overlay\r\n"
                    L"says FX, not faithful). Turn all four off for the exact picture.");
    choice(Page::Vnc, Kind::Combo, &P::vncClipboard, L"Clipboard",
           { L"Disabled", L"Ask each transfer", L"Remote \x2192 local text", L"Local \x2192 remote text",
             L"Bidirectional text" }, 0, 200);
    note(Page::Vnc, L"Clipboard is text only, Latin-1 on the wire (RFB cut text). Local \x2192 remote is\r\n"
                    L"Ctrl+V or the palette's \"VNC: send clipboard\", and goes through the paste guard\r\n"
                    L"first; remote \x2192 local lands on the Windows clipboard, never typed anywhere.");
    note(Page::Vnc, L"An unverifiable server certificate is shown as a fingerprint to accept, then\r\n"
                    L"pinned; a different certificate later is an alarm, not a warning.");

    // ---- Telnet ----------------------------------------------------------
    chk(Page::Telnet, &P::telnetPassive, L"Passive telnet negotiation (respond only, never initiate)");
    chk(Page::Telnet, &P::telnetKeyboard, L"Keyboard sends Telnet special commands (Ctrl+C = IP, Ctrl+Z = SUSP, Ctrl+D = EOF)");
    chk(Page::Telnet, &P::telnetNewline, L"Return key sends Telnet New Line (CR LF) instead of CR NUL");

    // ---- Rlogin ----------------------------------------------------------
    str(Page::Rlogin, &P::rloginLocalUser, L"Local username (blank = same as the session username)", 300);
    note(Page::Rlogin, L"Rlogin sends the local and remote user names, the terminal type\r\n"
                       L"and speed at connect time; the server may then ask for a password.");

    // ---- Effects ---------------------------------------------------------
    {
        Field f;
        f.page = Page::Effects; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"Effect preset (applied when the session opens)"; f.width = 260;
        f.options = { L"Global settings", L"Classic Amber", L"Clean Amber", L"Deep Phosphor",
                      L"Miami Night", L"Performance 4K" };
        static const char* names[] = { "", "Classic Amber", "Clean Amber", "Deep Phosphor",
                                       "Miami Night", "Performance 4K" };
        f.get = [](const P& p) {
            for (int i = 0; i < 6; ++i)
                if (p.effectPreset == names[i])
                    return std::to_wstring(i);
            return std::wstring(L"0");
        };
        f.set = [](P& p, const std::wstring& v) { p.effectPreset = names[std::clamp(_wtoi(v.c_str()), 0, 5)]; };
        push(std::move(f));
    }
    {
        Field f;
        f.page = Page::Effects; f.kind = Kind::Combo; f.id = idFor(0, 1);
        f.label = L"Particles per cell"; f.width = 160;
        f.options = { L"Global setting", L"8", L"16", L"24", L"32", L"48", L"64", L"96", L"128", L"160", L"256" };
        static const int dens[] = { 0, 8, 16, 24, 32, 48, 64, 96, 128, 160, 256 };
        f.get = [](const P& p) {
            for (int i = 0; i < 11; ++i)
                if (p.densityPpc == dens[i])
                    return std::to_wstring(i);
            return std::wstring(L"0");
        };
        f.set = [](P& p, const std::wstring& v) { p.densityPpc = dens[std::clamp(_wtoi(v.c_str()), 0, 10)]; };
        push(std::move(f));
    }
}

ConnectionDialog::Field* ConnectionDialog::FindField(int id)
{
    for (Field& f : m_fields)
        if (f.id == id)
            return &f;
    return nullptr;
}

// ------------------------------------------------------------- construction

void ConnectionDialog::BuildControls(HWND parent)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, m_dpi);
    // Typography is part of the skin: a skin may name its own face (Consolas
    // for the ship computer, Georgia for the brass panel, Arial for Swiss),
    // and condensedFont only picks a condensed default for those that do not.
    if (Skinned() && amber::ChromeFace())
    {
        wcscpy_s(ncm.lfMessageFont.lfFaceName, amber::ChromeFace());
        ncm.lfMessageFont.lfHeight = -Dpi(15, m_dpi);
        ncm.lfMessageFont.lfWeight =
            amber::Chrome().condensedFont ? FW_SEMIBOLD : FW_NORMAL;
    }
    else if (amber::Chrome().condensedFont)
    {
        wcscpy_s(ncm.lfMessageFont.lfFaceName, L"Bahnschrift SemiCondensed");
        ncm.lfMessageFont.lfHeight = -Dpi(15, m_dpi);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    }
    m_font = CreateFontIndirectW(&ncm.lfMessageFont);

    m_bgBrush = CreateSolidBrush(kBg);
    // Atelier lines every field with the check through WM_CTLCOLOREDIT.
    m_fieldBrush = amber::skin::Stitch()
        ? amber::skin::LiningBrush(m_dpi, kField, RGB(0x33, 0x2E, 0x38))
        : CreateSolidBrush(kField);
    m_monoFont = CreateFontW(-Dpi(14, m_dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Cascadia Mono");
    const wchar_t* headFace =
        (Skinned() && amber::ChromeFace()) ? amber::ChromeFace()
        : amber::Chrome().condensedFont       ? L"Bahnschrift Condensed"
                                              : L"Cascadia Mono";
    m_headerFont = CreateFontW(-Dpi(amber::Chrome().condensedFont ? 30 : 24, m_dpi), 0, 0, 0,
                               FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, headFace);
    m_tagFont = CreateFontW(-Dpi(11, m_dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Bahnschrift Condensed");

    m_status = MakeChild(parent, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IdStatus,
                         m_font);
    ShowWindow(m_status, SW_SHOW);

    // --- always-visible action buttons -----------------------------------
    for (auto [id, text] : { std::pair<int, const wchar_t*>{ IdOpen, L"&Open" },
                             { IdCancel, L"Cancel" } })
    {
        HWND b = MakeButton(parent, text, id, m_font,
                            id == IdOpen ? BS_DEFPUSHBUTTON : 0);
        ShowWindow(b, SW_SHOW);
    }

    // --- table-driven fields ---------------------------------------------
    for (Field& f : m_fields)
    {
        if (f.kind != Kind::Check && f.kind != Kind::Note)
            f.label_ = MakeLabel(parent, f.label.c_str(), m_font);
        switch (f.kind)
        {
        case Kind::Edit:
            f.ctrls.push_back(MakeEdit(parent, f.id, m_font));
            break;
        case Kind::Number:
            f.ctrls.push_back(MakeEdit(parent, f.id, m_font, ES_NUMBER));
            break;
        case Kind::Password:
            f.ctrls.push_back(MakeEdit(parent, f.id, m_font, ES_PASSWORD));
            break;
        case Kind::Multi:
            f.ctrls.push_back(MakeChild(parent, L"EDIT", L"",
                                        WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                            ES_AUTOVSCROLL | ES_WANTRETURN,
                                        f.id, m_font));
            break;
        case Kind::Check:
            f.ctrls.push_back(MakeButton(parent, f.label.c_str(), f.id, m_font,
                                         BS_AUTOCHECKBOX));
            break;
        case Kind::Combo:
        case Kind::ComboEdit:
        {
            HWND cb = MakeChild(parent, L"COMBOBOX", L"",
                                WS_TABSTOP | WS_VSCROLL |
                                    (f.kind == Kind::Combo ? CBS_DROPDOWNLIST : CBS_DROPDOWN),
                                f.id, m_font);
            for (const std::wstring& o : f.options)
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)o.c_str());
            if (f.kind == Kind::Combo)
                SendMessageW(cb, CB_SETCURSEL, 0, 0);
            f.ctrls.push_back(cb);
            break;
        }
        case Kind::RadioRow:
            for (size_t i = 0; i < f.options.size(); ++i)
                f.ctrls.push_back(MakeButton(parent, f.options[i].c_str(), f.id + (int)i,
                                             m_font, BS_AUTORADIOBUTTON |
                                                         (i == 0 ? WS_GROUP : 0)));
            break;
        case Kind::Note:
            f.ctrls.push_back(MakeLabel(parent, f.label.c_str(), m_font));
            break;
        }
    }
    // Radio rows and fields the caller shows/hides as a group are all created
    // hidden; ShowPage reveals the active page.

    // --- Session page extras: saved sessions list + name + buttons ----------
    m_sessionExtra.push_back(MakeLabel(parent, L"Saved sessions", m_font));
    m_sessionExtra.push_back(MakeChild(parent, L"LISTBOX", L"",
                                       WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                           LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,
                                       IdSessionList, m_font));
    m_sessionExtra.push_back(MakeLabel(parent, L"Profile name", m_font));
    m_sessionExtra.push_back(MakeEdit(parent, IdProfileName, m_font));
    m_sessionExtra.push_back(MakeButton(parent, L"&Load", IdLoad, m_font));
    m_sessionExtra.push_back(MakeButton(parent, L"&Save", IdSave, m_font));
    m_sessionExtra.push_back(MakeButton(parent, L"&Delete", IdDelete, m_font));

    // --- page buttons ------------------------------------------------------
    HWND browse = MakeButton(parent, L"B&rowse...", IdBrowseKey, m_font);
    HWND clear = MakeButton(parent, L"Clear saved credentials", IdClearCreds, m_font);
    HWND refresh = MakeButton(parent, L"Refresh ports", IdSerialRefresh, m_font);
    (void)browse; (void)clear; (void)refresh;

    // ------------------------------------------------- amber styling pass
    // Terminal-flavored mono font in every input, inner padding for the
    // borderless edits, dark scrollbars, dark combos.
    for (HWND h = GetWindow(parent, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        wchar_t cls[16] = L"";
        GetClassNameW(h, cls, 16);
        if (_wcsicmp(cls, L"Edit") == 0)
        {
            SendMessageW(h, WM_SETFONT, (WPARAM)m_monoFont, TRUE);
            SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(Dpi(6, m_dpi), Dpi(6, m_dpi)));
            if (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE)
                SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        }
        else if (_wcsicmp(cls, L"ListBox") == 0)
        {
            SendMessageW(h, WM_SETFONT, (WPARAM)m_monoFont, TRUE);
            SendMessageW(h, LB_SETITEMHEIGHT, 0, Dpi(24, m_dpi));
            SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        }
        else if (_wcsicmp(cls, L"ComboBox") == 0)
        {
            // Classic rendering honors our WM_CTLCOLOR* brushes; the themed
            // combo insists on a light frame.
            SetWindowTheme(h, L" ", L" ");
            SendMessageW(h, WM_SETFONT, (WPARAM)m_monoFont, TRUE);
        }
    }

    // Last: selecting the initial tree node fires TVN_SELCHANGED -> ShowPage
    // -> Layout synchronously, which must find every page control created.
    BuildTree(parent);
}

void ConnectionDialog::BuildTree(HWND parent)
{
    m_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 TVS_HASBUTTONS | TVS_FULLROWSELECT |
                                 TVS_SHOWSELALWAYS,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)IdTree,
                             GetModuleHandleW(nullptr), nullptr);
    if (m_font)
        SendMessageW(m_tree, WM_SETFONT, (WPARAM)m_font, TRUE);
    SendMessageW(m_tree, TVM_SETBKCOLOR, 0, (LPARAM)kBg);
    SendMessageW(m_tree, TVM_SETTEXTCOLOR, 0, (LPARAM)kTextDim);
    SendMessageW(m_tree, TVM_SETITEMHEIGHT, Dpi(22, m_dpi), 0);
    SetWindowTheme(m_tree, L"DarkMode_Explorer", nullptr);

    auto insert = [&](HTREEITEM parentItem, const wchar_t* text, Page page)
    {
        TVINSERTSTRUCTW tvis = {};
        std::wstring cased = SkinLabel(text);
        tvis.hParent = parentItem;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = cased.data();
        tvis.item.lParam = (LPARAM)page;
        return (HTREEITEM)SendMessageW(m_tree, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
    };

    HTREEITEM session = insert(TVI_ROOT, L"Session", Page::Session);
    insert(session, L"Logging", Page::Logging);
    HTREEITEM term = insert(TVI_ROOT, L"Terminal", Page::Terminal);
    insert(term, L"Keyboard", Page::Keyboard);
    insert(term, L"Bell", Page::Bell);
    insert(term, L"Features", Page::Features);
    HTREEITEM window = insert(TVI_ROOT, L"Window", Page::Window);
    insert(window, L"Appearance", Page::Appearance);
    insert(window, L"Behaviour", Page::Behaviour);
    insert(window, L"Translation", Page::Translation);
    insert(window, L"Selection", Page::Selection);
    insert(window, L"Colours", Page::Colours);
    HTREEITEM conn = insert(TVI_ROOT, L"Connection", Page::Connection);
    HTREEITEM guard = insert(conn, L"Guardian", Page::Guardian);
    insert(guard, L"Reattach", Page::Reattach);
    insert(conn, L"Data", Page::Data);
    insert(conn, L"Proxy", Page::Proxy);
    HTREEITEM ssh = insert(conn, L"SSH", Page::Ssh);
    insert(ssh, L"Auth", Page::SshAuth);
    insert(ssh, L"X11", Page::SshX11);
    insert(ssh, L"Remote GUI", Page::RemoteGui);
    insert(ssh, L"Tunnels", Page::SshTunnels);
    insert(ssh, L"Host keys", Page::SshHostKeys);
    insert(conn, L"Serial", Page::Serial);
    insert(conn, L"Local", Page::Local);
    insert(conn, L"Telnet", Page::Telnet);
    insert(conn, L"Rlogin", Page::Rlogin);
    insert(conn, L"VNC", Page::Vnc);
    insert(TVI_ROOT, L"Effects", Page::Effects);

    for (HTREEITEM it : { session, term, window, conn, ssh, guard })
        SendMessageW(m_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)it);
    SendMessageW(m_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)session);
}

// ------------------------------------------------------------------ layout

void ConnectionDialog::Layout()
{
    if (!m_dlg)
        return;
    RECT rc;
    GetClientRect(m_dlg, &rc);
    const int pad = Dpi(12, m_dpi);
    const int treeW = Dpi(200, m_dpi);
    const int btnW = Dpi(96, m_dpi);
    const int btnH = Dpi(30, m_dpi);
    const int rowH = Dpi(26, m_dpi);
    const int gap = Dpi(6, m_dpi);
    const int bannerH = Dpi(58, m_dpi);   // wordmark strip painted in WM_PAINT

    // LCARS elbow frame: a side bar runs down the left edge; the tree and
    // the panel move right to make room.
    const int side = amber::Chrome().elbow ? Dpi(22, m_dpi) : 0;
    int bottom = rc.bottom - pad - btnH;
    MoveWindow(m_tree, pad + side, pad + bannerH + (side ? Dpi(10, m_dpi) : 0), treeW,
               bottom - pad - bannerH - gap - (side ? Dpi(10, m_dpi) : 0), TRUE);
    MoveWindow(m_status, pad, bottom + Dpi(4, m_dpi),
               rc.right - pad * 3 - btnW * 2 - gap, btnH, TRUE);
    MoveWindow(GetDlgItem(m_dlg, IdCancel), rc.right - pad - btnW, bottom, btnW,
               btnH, TRUE);
    MoveWindow(GetDlgItem(m_dlg, IdOpen), rc.right - pad - btnW * 2 - gap, bottom,
               btnW, btnH, TRUE);

    const int px = pad + side + treeW + pad;
    const int pw = rc.right - px - pad;
    int y = pad + bannerH + (side ? Dpi(10, m_dpi) : 0);

    HDC dc = GetDC(m_dlg);
    HGDIOBJ of = SelectObject(dc, m_font);
    // Measure what will actually be DRAWN. On an uppercase skin the control
    // was created with cased text, which is wider than the source string —
    // measuring the original clips the last radio in a row.
    auto textW = [&](const std::wstring& s) {
        std::wstring cased = SkinLabel(s);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, cased.c_str(), (int)cased.size(), &sz);
        return sz.cx;
    };
    auto lines = [](const std::wstring& s) {
        int n = 1;
        for (wchar_t c : s)
            if (c == L'\n')
                ++n;
        return n;
    };

    auto placeField = [&](Field& f)
    {
        if (f.ctrls.empty())
            return;                      // controls not built yet
        int w = f.width < 0 ? pw : Dpi(f.width, m_dpi);
        switch (f.kind)
        {
        case Kind::Note:
        {
            int h = rowH * lines(f.label) - Dpi(4, m_dpi);
            MoveWindow(f.ctrls[0], px, y, pw, h, TRUE);
            y += h + gap;
            break;
        }
        case Kind::Check:
            MoveWindow(f.ctrls[0], px, y, pw, rowH, TRUE);
            y += rowH + gap;
            break;
        case Kind::RadioRow:
        {
            MoveWindow(f.label_, px, y, pw, rowH - Dpi(6, m_dpi), TRUE);
            y += rowH - Dpi(4, m_dpi);
            int x = px;
            for (size_t i = 0; i < f.ctrls.size(); ++i)
            {
                int w2 = textW(f.options[i]) + Dpi(28, m_dpi);
                if (x + w2 > px + pw && x > px)
                {
                    x = px;
                    y += rowH;
                }
                MoveWindow(f.ctrls[i], x, y, w2, rowH, TRUE);
                x += w2 + Dpi(10, m_dpi);
            }
            y += rowH + gap;
            break;
        }
        case Kind::Multi:
        {
            int lh = rowH * lines(f.label) - Dpi(6, m_dpi);
            MoveWindow(f.label_, px, y, pw, lh, TRUE);
            y += lh + Dpi(2, m_dpi);
            MoveWindow(f.ctrls[0], px, y, pw, rowH * 4, TRUE);
            y += rowH * 4 + gap;
            break;
        }
        default:
        {
            MoveWindow(f.label_, px, y, pw, rowH - Dpi(6, m_dpi), TRUE);
            y += rowH - Dpi(4, m_dpi);
            const bool combo = f.kind == Kind::Combo || f.kind == Kind::ComboEdit;
            MoveWindow(f.ctrls[0], px, y, w, combo ? rowH * 9 : rowH, TRUE);
            // A Browse button sits to the right of the key path.
            if (f.id == IdKeyPath)
            {
                int bw = Dpi(90, m_dpi);
                MoveWindow(f.ctrls[0], px, y, pw - bw - gap, rowH, TRUE);
                MoveWindow(GetDlgItem(m_dlg, IdBrowseKey), px + pw - bw, y, bw, rowH, TRUE);
            }
            if (f.id == IdSerialPort)
            {
                int bw = Dpi(110, m_dpi);
                MoveWindow(GetDlgItem(m_dlg, IdSerialRefresh), px + w + gap, y, bw, rowH, TRUE);
            }
            y += rowH + gap;
            break;
        }
        }
    };

    // Session: host / port / type / user, then the saved-session block, then
    // the close-on-exit choice.
    if (m_page == Page::Session)
    {
        for (Field& f : m_fields)
            if (f.page == Page::Session && f.id != IdCloseOnExit)
                placeField(f);
        // extras
        if (m_sessionExtra.size() < 7)
        {
            SelectObject(dc, of);
            ReleaseDC(m_dlg, dc);
            return;                      // controls not built yet
        }
        MoveWindow(m_sessionExtra[0], px, y, pw, rowH - Dpi(6, m_dpi), TRUE);
        y += rowH - Dpi(4, m_dpi);
        int listH = Dpi(96, m_dpi);
        MoveWindow(m_sessionExtra[1], px, y, pw, listH, TRUE);
        y += listH + gap;
        MoveWindow(m_sessionExtra[2], px, y, pw, rowH - Dpi(6, m_dpi), TRUE);
        y += rowH - Dpi(4, m_dpi);
        MoveWindow(m_sessionExtra[3], px, y, pw, rowH, TRUE);
        y += rowH + gap;
        int bx = px;
        for (int i = 4; i < 7; ++i)
        {
            MoveWindow(m_sessionExtra[i], bx, y, Dpi(80, m_dpi), btnH, TRUE);
            bx += Dpi(80, m_dpi) + gap;
        }
        y += btnH + gap;
        if (Field* f = FindField(IdCloseOnExit))
            placeField(*f);
    }
    else
    {
        for (Field& f : m_fields)
            if (f.page == m_page)
                placeField(f);
        if (m_page == Page::SshAuth)
        {
            MoveWindow(GetDlgItem(m_dlg, IdClearCreds), px, y, Dpi(190, m_dpi), btnH, TRUE);
            y += btnH + gap;
        }
    }
    SelectObject(dc, of);
    ReleaseDC(m_dlg, dc);
}

void ConnectionDialog::ShowPage(Page page)
{
    for (Field& f : m_fields)
    {
        const int show = (f.page == page) ? SW_SHOW : SW_HIDE;
        if (f.label_)
            ShowWindow(f.label_, show);
        for (HWND h : f.ctrls)
            ShowWindow(h, show);
    }
    for (HWND h : m_sessionExtra)
        ShowWindow(h, page == Page::Session ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(m_dlg, IdBrowseKey), page == Page::SshAuth ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(m_dlg, IdClearCreds), page == Page::SshAuth ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(m_dlg, IdSerialRefresh), page == Page::Serial ? SW_SHOW : SW_HIDE);
    // Host and port mean nothing to a local shell; a serial line has neither.
    {
        Field* pf = FindField(IdProtocol);
        const int proto = pf ? _wtoi(FieldValue(*pf).c_str()) : 0;
        const BOOL netw = (proto != static_cast<int>(Protocol::Serial) &&
                           proto != static_cast<int>(Protocol::Local));
        EnableWindow(GetDlgItem(m_dlg, IdHost), netw);
        EnableWindow(GetDlgItem(m_dlg, IdPort), netw);
    }

    m_page = page;
    Layout();
    InvalidateRect(m_dlg, nullptr, TRUE);
}

// ------------------------------------------------------------ field values

std::wstring ConnectionDialog::FieldValue(const Field& f) const
{
    if (f.ctrls.empty())
        return {};
    switch (f.kind)
    {
    case Kind::Check:
        return GetPropW(f.ctrls[0], kCheckProp) ? L"1" : L"0";
    case Kind::Combo:
        return std::to_wstring((int)SendMessageW(f.ctrls[0], CB_GETCURSEL, 0, 0));
    case Kind::RadioRow:
        for (size_t i = 0; i < f.ctrls.size(); ++i)
            if (GetPropW(f.ctrls[i], kCheckProp))
                return std::to_wstring(i);
        return L"0";
    case Kind::Note:
        return {};
    default:
        return GetText(f.ctrls[0]);
    }
}

void ConnectionDialog::SetFieldValue(Field& f, const std::wstring& v)
{
    if (f.ctrls.empty())
        return;
    switch (f.kind)
    {
    case Kind::Check:
        SetPropW(f.ctrls[0], kCheckProp, v == L"1" ? (HANDLE)1 : nullptr);
        InvalidateRect(f.ctrls[0], nullptr, TRUE);
        break;
    case Kind::Combo:
        SendMessageW(f.ctrls[0], CB_SETCURSEL, (WPARAM)_wtoi(v.c_str()), 0);
        break;
    case Kind::RadioRow:
    {
        int idx = std::clamp(_wtoi(v.c_str()), 0, (int)f.ctrls.size() - 1);
        for (size_t i = 0; i < f.ctrls.size(); ++i)
        {
            SetPropW(f.ctrls[i], kCheckProp, (int)i == idx ? (HANDLE)1 : nullptr);
            InvalidateRect(f.ctrls[i], nullptr, TRUE);
        }
        break;
    }
    case Kind::Note:
        break;
    default:
        SetText(f.ctrls[0], v);
        break;
    }
}

void ConnectionDialog::WriteFields(const ConnectionProfile& p)
{
    for (Field& f : m_fields)
        if (f.get)
            SetFieldValue(f, f.get(p));
    m_lastProtocol = (int)p.protocol;
}

void ConnectionDialog::ReadFields(ConnectionProfile& p)
{
    for (Field& f : m_fields)
        if (f.set)
            f.set(p, FieldValue(f));
    // Sanity floors the store also applies; keep the dialog honest.
    if (p.cols < 20) p.cols = 80;
    if (p.rows < 5)  p.rows = 25;
    if (p.connectTimeoutSeconds < 1) p.connectTimeoutSeconds = 15;
    if (p.keepaliveSeconds < 0) p.keepaliveSeconds = 0;
    if (p.scrollbackLines < 0) p.scrollbackLines = 0;
    if (p.gapPx < 0) p.gapPx = 0;
    if (p.termType.empty()) p.termType = "xterm-256color";
    if (p.termSpeed.empty()) p.termSpeed = "38400,38400";
    if (p.serialBaud <= 0) p.serialBaud = 9600;
}

// ------------------------------------------------------------ session list

void ConnectionDialog::RefreshSessionList()
{
    HWND list = GetDlgItem(m_dlg, IdSessionList);
    if (!list)
        return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& p : m_store.All())
    {
        std::wstring label = Widen(p.name.empty() ? p.host : p.name);
        int index = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(list, LB_SETITEMDATA, index, (LPARAM)index);
    }
}

void ConnectionDialog::LoadSelectedProfile()
{
    HWND list = GetDlgItem(m_dlg, IdSessionList);
    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)m_store.All().size())
    {
        SetStatus(L"Select a saved session first.");
        return;
    }
    const ConnectionProfile& p = m_store.All()[(size_t)sel];
    m_selectedProfileId = p.id;

    WriteFields(p);
    SetText(GetDlgItem(m_dlg, IdProfileName), Widen(p.name));

    // Remembered secrets are fetched from the Credential Manager, never a file.
    SetText(GetDlgItem(m_dlg, IdPassword), L"");
    SetText(GetDlgItem(m_dlg, IdVncPassword), L"");
    SetText(GetDlgItem(m_dlg, IdPassphrase), L"");
    SetText(GetDlgItem(m_dlg, IdProxyPassword), L"");
    if (p.rememberPassword)
    {
        SecureString pw;
        if (CredentialStore::Load(p.id, SecretKind::Password, pw))
        {
            const RevealedSecret r(pw);
            const RevealedWide w(Widen(r.Get()));
            SetWindowTextW(GetDlgItem(m_dlg, p.protocol == Protocol::Vnc ? IdVncPassword : IdPassword),
                           w.c_str());
        }
    }
    if (p.rememberPassphrase)
    {
        SecureString pp;
        if (CredentialStore::Load(p.id, SecretKind::KeyPassphrase, pp))
        {
            const RevealedSecret r(pp);
            const RevealedWide w(Widen(r.Get()));
            SetWindowTextW(GetDlgItem(m_dlg, IdPassphrase), w.c_str());
        }
    }
    if (p.rememberProxyPassword)
    {
        SecureString pp;
        if (CredentialStore::Load(p.id, SecretKind::ProxyPassword, pp))
        {
            const RevealedSecret r(pp);
            const RevealedWide w(Widen(r.Get()));
            SetWindowTextW(GetDlgItem(m_dlg, IdProxyPassword), w.c_str());
        }
    }

    SyncAuthEnabled();
    SyncGuardianEnabled();
    SetStatus(L"Loaded \"" + Widen(p.name.empty() ? p.host : p.name) + L"\".");
}

void ConnectionDialog::SaveCurrentProfile()
{
    ConnectionProfile p;
    if (!m_selectedProfileId.empty())
    {
        if (const ConnectionProfile* existing = m_store.Find(m_selectedProfileId))
            p = *existing;
    }
    if (p.id.empty())
        p.id = MakeUuid();

    ReadFields(p);
    if (p.protocol != Protocol::Serial && p.protocol != Protocol::Local && p.host.empty())
    {
        SetStatus(L"A host name is required before saving.");
        return;
    }
    if (p.protocol == Protocol::Serial && p.serialPort.empty())
    {
        SetStatus(L"A serial line (COM port) is required before saving.");
        return;
    }
    if (p.port < 0 || p.port > 65535)
        p.port = ProtocolDefaultPort(p.protocol);
    p.name = Narrow(GetText(GetDlgItem(m_dlg, IdProfileName)));
    if (p.name.empty())
    {
        if (p.protocol == Protocol::Serial)
            p.name = p.serialPort;
        else if (p.protocol == Protocol::Local)
        {
            p.name = "Local shell";
            for (const amber::LocalShell& s : amber::DiscoverLocalShells())
                if (s.key == p.localShellKey)
                    p.name = s.name;
        }
        else
            p.name = p.host;
    }

    // Secrets go to the Credential Manager, keyed by profile id — never to JSON.
    auto secret = [&](bool remember, int editId, SecretKind kind)
    {
        if (remember)
        {
            SecureString s(Narrow(GetText(GetDlgItem(m_dlg, editId))));
            CredentialStore::Store(p.id, kind, s);
        }
        else
            CredentialStore::Erase(p.id, kind);
    };
    SyncRememberPassword(p);
    secret(p.rememberPassword, p.protocol == Protocol::Vnc ? IdVncPassword : IdPassword, SecretKind::Password);
    secret(p.rememberPassphrase, IdPassphrase, SecretKind::KeyPassphrase);
    secret(p.rememberProxyPassword, IdProxyPassword, SecretKind::ProxyPassword);

    ApplyRemoteGui(p);

    // Trusted X11 is an opt-in behind a typed confirmation, asked whenever
    // trust is being turned on or changed to the other kind — never when a
    // profile that already has it is merely edited. Declining leaves the
    // profile restricted; it never fails the save.
    if (p.x11Trust != 0)
    {
        const ConnectionProfile* prev = m_store.Find(p.id);
        if (!prev || prev->x11Trust != p.x11Trust)
        {
            if (!ShowTrustedX11Dialog(m_dlg, p.host, p.x11Trust == 2))
            {
                p.x11Trust = 0;
                if (p.remoteGui == 2)
                    p.remoteGui = 1;    // declining leaves it restricted, not off
            }
        }
    }

    m_selectedProfileId = m_store.Upsert(p);
    std::string err;
    if (!m_store.Save(&err))
    {
        SetStatus(L"Could not save profiles: " + Widen(err));
        return;
    }
    RefreshSessionList();
    SetStatus(L"Saved \"" + Widen(p.name) + L"\".");
}

void ConnectionDialog::DeleteSelectedProfile()
{
    HWND list = GetDlgItem(m_dlg, IdSessionList);
    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)m_store.All().size())
    {
        SetStatus(L"Select a saved session to delete.");
        return;
    }
    ConnectionProfile p = m_store.All()[(size_t)sel];

    std::wstring prompt = L"Delete saved session \"" +
                          Widen(p.name.empty() ? p.host : p.name) + L"\"?";
    if (MessageBoxW(m_dlg, prompt.c_str(), L"AmberSSH",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    CredentialStore::Erase(p.id, SecretKind::Password);
    CredentialStore::Erase(p.id, SecretKind::KeyPassphrase);
    CredentialStore::Erase(p.id, SecretKind::ProxyPassword);
    m_store.Remove(p.id);
    if (m_selectedProfileId == p.id)
        m_selectedProfileId.clear();

    std::string err;
    m_store.Save(&err);
    RefreshSessionList();
    SetStatus(L"Deleted.");
}

// Remote GUI is the authority for AmberX, so the three older fields are
// brought into step with it before anything else looks at them. They keep
// their own meaning when Remote GUI is off, which is what leaves the
// external-X-server path on the X11 page working exactly as before.
//
// Called from BOTH saving and connecting. Only saving would be the obvious
// place and it is not enough: a user who picks a Remote GUI mode and presses
// Open without pressing Save would get a session with the setting ignored and
// nothing to say why.
void ConnectionDialog::ApplyRemoteGui(ConnectionProfile& p)
{
    if (p.remoteGui != 0)
    {
        p.x11Forward = true;
        p.x11Backend = 1;
        if (p.remoteGui == 2 && p.x11Trust == 0)
            p.x11Trust = 1;
        else if (p.remoteGui == 1)
            p.x11Trust = 0;
    }
    else if (p.x11Backend == 1)
    {
        // Remote GUI turned off on a profile that used AmberX: turn the
        // backend off with it rather than leaving a host that nothing asked
        // for.
        p.x11Backend = 0;
        p.x11Trust = 0;
    }
}

bool ConnectionDialog::CollectRequest()
{
    ConnectionProfile& p = m_out.profile;
    p = ConnectionProfile{};
    p.id = m_selectedProfileId.empty() ? MakeUuid() : m_selectedProfileId;
    ReadFields(p);
    SyncRememberPassword(p);
    p.name = Narrow(GetText(GetDlgItem(m_dlg, IdProfileName)));
    // Connecting applies the same rule saving does, so "set it and connect"
    // works without a save first.
    ApplyRemoteGui(p);

    if (p.protocol == Protocol::Serial)
    {
        if (p.serialPort.empty())
        {
            SetStatus(L"Choose a serial line (COM port) on the Serial page.");
            return false;
        }
    }
    else if (p.protocol == Protocol::Local)
    {
        // no host, port or user; a blank shell and executable mean the
        // machine's default shell, resolved at launch
    }
    else
    {
        if (p.host.empty())
        {
            SetStatus(L"Enter a host name or IP address.");
            SetFocus(GetDlgItem(m_dlg, IdHost));
            return false;
        }
        if (p.port <= 0 || p.port > 65535)
        {
            SetStatus(L"Enter a port between 1 and 65535.");
            SetFocus(GetDlgItem(m_dlg, IdPort));
            return false;
        }
        if ((p.protocol == Protocol::Ssh || p.protocol == Protocol::Rlogin) &&
            p.username.empty())
        {
            SetStatus(L"Enter a username.");
            SetFocus(GetDlgItem(m_dlg, IdUser));
            return false;
        }
    }
    if (p.protocol == Protocol::Ssh && p.auth == AuthMethod::PublicKey &&
        p.privateKeyPath.empty())
    {
        SetStatus(L"Choose a private key file, or switch to password auth.");
        return false;
    }
    if (p.proxyType != ProxyType::None && p.proxyHost.empty())
    {
        SetStatus(L"Enter the proxy host name, or set the proxy type to None.");
        return false;
    }

    m_out.password.Assign(Narrow(GetText(GetDlgItem(m_dlg, p.protocol == Protocol::Vnc ? IdVncPassword : IdPassword))));
    m_out.passphrase.Assign(Narrow(GetText(GetDlgItem(m_dlg, IdPassphrase))));
    m_out.proxyPassword.Assign(Narrow(GetText(GetDlgItem(m_dlg, IdProxyPassword))));

    // Wipe the edit controls so the secrets do not linger in window text.
    SetText(GetDlgItem(m_dlg, IdPassword), L"");
    SetText(GetDlgItem(m_dlg, IdVncPassword), L"");
    SetText(GetDlgItem(m_dlg, IdPassphrase), L"");
    SetText(GetDlgItem(m_dlg, IdProxyPassword), L"");
    return true;
}

void ConnectionDialog::SyncRememberPassword(ConnectionProfile& p) const
{
    // Two check boxes share one flag (SSH > Auth and VNC); the one on the
    // page that applies to the protocol is the one that counts.
    // The check boxes are owner-drawn and keep their state in a window
    // property, not in BM_GETCHECK (which always says unchecked for them):
    // read it the way every other field is read.
    const int id = p.protocol == Protocol::Vnc ? IdRememberVncPassword : IdRememberPassword;
    for (const Field& f : m_fields)
        if (f.id == id)
        {
            p.rememberPassword = FieldValue(f) == L"1";
            return;
        }
}

void ConnectionDialog::SyncAuthEnabled()
{
    Field* auth = FindField(IdAuth);
    int idx = auth ? _wtoi(FieldValue(*auth).c_str()) : 0;
    bool pw = idx == 0 || idx == 2;
    bool key = idx == 1;
    EnableWindow(GetDlgItem(m_dlg, IdPassword), pw);
    EnableWindow(GetDlgItem(m_dlg, IdRememberPassword), pw);
    EnableWindow(GetDlgItem(m_dlg, IdKeyPath), key);
    EnableWindow(GetDlgItem(m_dlg, IdBrowseKey), key);
    EnableWindow(GetDlgItem(m_dlg, IdPassphrase), key);
    EnableWindow(GetDlgItem(m_dlg, IdRememberPassphrase), key);
}

void ConnectionDialog::SyncGuardianEnabled()
{
    // The session name only means something for tmux and screen; the command
    // only for Custom. Greying the rest is how the page says which one of the
    // three the far end will actually be asked to run.
    Field* mode = FindField(IdReattachMode);
    const int idx = mode ? _wtoi(FieldValue(*mode).c_str()) : 0;
    const bool named = idx == 1 || idx == 2;      // tmux / screen
    const bool custom = idx == 3;
    if (Field* f = FindField(IdReattachSession))
        for (HWND h : f->ctrls)
            EnableWindow(h, named);
    if (Field* f = FindField(IdReattachCommand))
        for (HWND h : f->ctrls)
            EnableWindow(h, custom);
}

void ConnectionDialog::SyncProtocol()
{
    // Like PuTTY: switching the connection type swaps the port only when it
    // still holds the previous type's default.
    Field* pf = FindField(IdProtocol);
    if (!pf)
        return;
    int now = _wtoi(FieldValue(*pf).c_str());
    int prevDef = ProtocolDefaultPort(static_cast<Protocol>(m_lastProtocol));
    int nowDef = ProtocolDefaultPort(static_cast<Protocol>(now));
    HWND port = GetDlgItem(m_dlg, IdPort);
    int cur = _wtoi(GetText(port).c_str());
    if (cur == prevDef || cur == 0)
        SetText(port, std::to_wstring(nowDef));
    m_lastProtocol = now;
    static const wchar_t* hints[] = {
        L"SSH: encrypted shell (Connection > SSH for auth, tunnels, X11).",
        L"Telnet: plain text, port 23 (Connection > Telnet for negotiation).",
        L"Rlogin: plain text, port 513 (Connection > Rlogin for the local user).",
        L"Raw: bare TCP stream, no protocol (set the port).",
        L"Serial: local COM port (Connection > Serial for line settings).",
        L"Local: a shell on this machine — PowerShell, cmd, WSL (Connection > Local).",
        L"VNC: a remote desktop drawn in particles, port 5900 (Connection > VNC for password, tunnel, TLS).",
    };
    SetStatus(hints[std::clamp(now, 0, 6)]);

    // Local needs no host, port or user: choosing it should leave Open ready
    // to press. The shell defaults to PowerShell (7 when installed, else
    // Windows PowerShell) unless the user already picked a shell or typed an
    // executable.
    if (now == static_cast<int>(Protocol::Local))
    {
        Field* shell = FindField(IdLocalShell);
        Field* exe = FindField(IdLocalExe);
        const bool custom = shell && _wtoi(FieldValue(*shell).c_str()) == 0;
        const bool typed = exe && !FieldValue(*exe).empty();
        if (shell && custom && !typed)
        {
            const std::vector<amber::LocalShell> shells = amber::DiscoverLocalShells();
            for (size_t i = 0; i < shells.size(); ++i)
                if (shells[i].key == "pwsh" || shells[i].key == "powershell")
                {
                    SetFieldValue(*shell, std::to_wstring(i + 1));   // options[0] is Custom
                    break;
                }
        }
        SetStatus(L"Local: a shell on this machine. Nothing else is needed \x2014 press Open.");
    }
}

void ConnectionDialog::BrowseForKey()
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = m_dlg;
    ofn.lpstrFilter = L"OpenSSH private keys\0*.*\0PEM files\0*.pem\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select an OpenSSH private key";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
    {
        SetText(GetDlgItem(m_dlg, IdKeyPath), path);
        std::wstring lower(path);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".ppk") == 0)
            SetStatus(L"PuTTY .ppk keys must be converted to OpenSSH format "
                      L"before use.");
    }
}

// ------------------------------------------------------------------ chrome

void ConnectionDialog::PaintChrome(HDC dc)
{
    RECT rc;
    GetClientRect(m_dlg, &rc);
    const int pad = Dpi(12, m_dpi);
    const int bannerH = Dpi(58, m_dpi);
    // Horologe: the header band is engine-turned before anything is set on
    // it. The band only — labels and fields below the divider stay plain.
    if (amber::skin::Rehaut())
    {
        RECT band = { 0, 0, rc.right, pad + bannerH - Dpi(12, m_dpi) };
        amber::skin::Guilloche(dc, band, m_dpi, kBg);
    }
    if (amber::skin::Meter())
    {
        // Reference: the header band is brushed plate.
        RECT band = { 0, 0, rc.right, pad + bannerH - Dpi(12, m_dpi) };
        amber::skin::Brushed(dc, band, m_dpi, kBg);
    }
    if (amber::skin::Glaze())
    {
        // Tenmoku: oil spots on the band, rust along the top where the glaze
        // thins, and the whole dialog standing on its raw stoneware foot.
        RECT band = { 0, 0, rc.right, pad + bannerH - Dpi(12, m_dpi) };
        amber::skin::OilSpot(dc, band, m_dpi, amber::skin::kOilSpot);
        RECT rim = { 0, 0, rc.right, Dpi(2, m_dpi) };
        HBRUSH rb = CreateSolidBrush(amber::skin::kRust);
        FillRect(dc, &rim, rb);
        DeleteObject(rb);
        amber::skin::Foot(dc, rc, m_dpi, amber::skin::kClay);
    }
    if (amber::skin::Stitch())
    {
        // Atelier: the dialog is one piece of hide — painted edge, stitch
        // inside it. Controls sit a full pad in from the edge, so neither
        // touches them.
        amber::skin::EdgePaint(dc, rc, m_dpi, kBorder);
        RECT in = { rc.left + Dpi(3, m_dpi), rc.top + Dpi(3, m_dpi),
                    rc.right - Dpi(3, m_dpi), rc.bottom - Dpi(3, m_dpi) };
        amber::skin::StitchRect(dc, in, 0, m_dpi, kText);
    }

    // ---- wordmark with a faked phosphor glow -----------------------------
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, m_headerFont);
    const wchar_t mark[] = L"AmberSSH";
    const int markLen = 8;
    int tx = pad + Dpi(2, m_dpi), ty = pad + Dpi(4, m_dpi);
    // Wordmark glow: theme amber in Classic, neon (magenta halo, cyan face)
    // in a skinned dialog.
    const bool skin = Skinned();
    // The glow taps are a phosphor halo. On a light ground they read as a
    // muddy drop shadow instead, so a light skin prints the wordmark flat.
    const bool halo = !(skin && amber::Chrome().lightGround);
    if (halo)
    {
        SetTextColor(dc, skin ? Dim(kBanner2, 0.35f) : RGB(84, 46, 12));
        for (POINT o : { POINT{ 2, 2 }, { -2, 2 }, { 2, -2 }, { -2, -2 },
                         { 0, 3 }, { 3, 0 } })
            TextOutW(dc, tx + o.x, ty + o.y, mark, markLen);
        SetTextColor(dc, skin ? Dim(kBanner2, 0.65f) : RGB(150, 86, 26));
        for (POINT o : { POINT{ 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } })
            TextOutW(dc, tx + o.x, ty + o.y, mark, markLen);
    }
    SetTextColor(dc, skin ? kBanner : RGB(255, 208, 126));
    TextOutW(dc, tx, ty, mark, markLen);

    SIZE ms = {};
    GetTextExtentPoint32W(dc, mark, markLen, &ms);
    SelectObject(dc, m_font);
    SetTextColor(dc, kTextDim);
    const wchar_t sub[] = L"// nixie particle terminal";
    // The tagline hangs off the wordmark's height, capped so a tall boutique
    // face (Josefin, Jost) cannot push it down onto the divider.
    TextOutW(dc, tx + ms.cx + Dpi(14, m_dpi),
             ty + (std::min)(static_cast<int>(ms.cy) - Dpi(19, m_dpi), Dpi(14, m_dpi)), sub,
             (int)wcslen(sub));
    SelectObject(dc, oldFont);

    // ---- divider: amber fading out to the right --------------------------
    int dy = pad + bannerH - Dpi(10, m_dpi);
    int x0 = pad, x1 = rc.right - pad;
    if (amber::Chrome().elbow)
    {
        // LCARS frame: the elbow, the block column that continues its leg on
        // the same gutter, and the end cap that terminates the top bar. The
        // geometry lives in SkinDraw so the About panel frames identically.
        const amber::ChromeSpec& ch = amber::Chrome();
        const int H = Dpi(10, m_dpi);
        const int V = Dpi(22, m_dpi);      // matches the reserved side gutter
        const int G = Dpi(4, m_dpi);
        const int top = dy - H;
        const int leg = H + H + Dpi(34, m_dpi);
        const int end = amber::skin::Elbow(dc, 0, top, x1, H, V, leg,
                                           amber::SrgbRef(ch.bars[0]));
        amber::skin::BlockColumn(dc, 0, end + G, V, rc.bottom - pad, G,
                                 ch.bars, 1, m_dpi);
        amber::skin::EndCap(dc, x1, top, Dpi(58, m_dpi), H, G,
                            amber::SrgbRef(ch.bars[1]), kBg);
    }
    else if (amber::skin::Impression())
    {
        // Letterpress: a printer's thick-thin rule, and one fleuron at the
        // head of the sheet.
        amber::skin::ThickThin(dc, pad, dy - Dpi(4, m_dpi), x1 - pad, kBorderHot, m_dpi);
        amber::skin::Fleuron(dc, x1 - Dpi(16, m_dpi), dy - Dpi(24, m_dpi), Dpi(10, m_dpi),
                             kBorderHot);
    }
    else if (amber::skin::Stitch())
    {
        // Atelier: a seam — two stitch lines with the painted edge between.
        RECT seam = { pad, dy - Dpi(5, m_dpi), x1, dy + Dpi(1, m_dpi) };
        HBRUSH pb = CreateSolidBrush(kBorder);
        FillRect(dc, &seam, pb);
        DeleteObject(pb);
        amber::skin::StitchRun(dc, pad, dy - Dpi(8, m_dpi), x1 - pad, m_dpi, kText);
        amber::skin::StitchRun(dc, pad, dy + Dpi(4, m_dpi), x1 - pad, m_dpi, kText);
    }
    else if (amber::skin::Meter())
    {
        // Reference: a bevelled rule with the model plate engraved beside it.
        RECT rule = { pad, dy - Dpi(2, m_dpi), x1, dy };
        amber::skin::AppliedIndex(dc, rule, RGB(0xE6, 0xE8, 0xEB), RGB(0x7E, 0x82, 0x87));
        RECT plate = { x1 - Dpi(260, m_dpi), pad, x1 - Dpi(6, m_dpi), pad + Dpi(18, m_dpi) };
        amber::skin::Engrave(dc, plate, L"AMBER SSH  ·  MODEL 1.0", m_tagFont, kTextDim,
                             RGB(0xE6, 0xE8, 0xEB),
                             DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    else if (amber::skin::Glaze())
    {
        // Tenmoku: the divider is the rim — rust where the glaze thins — and
        // the studio's chop sits beside the wordmark.
        RECT rim = { pad, dy - Dpi(2, m_dpi), x1, dy };
        HBRUSH rb = CreateSolidBrush(amber::skin::kRust);
        FillRect(dc, &rim, rb);
        DeleteObject(rb);
        RECT under = { pad, dy, x1, dy + 1 };
        HBRUSH ub = CreateSolidBrush(amber::skin::Dim(kBg, 0.5f));
        FillRect(dc, &under, ub);
        DeleteObject(ub);
        amber::skin::Chop(dc, x1 - Dpi(22, m_dpi), pad + Dpi(2, m_dpi), Dpi(14, m_dpi),
                          amber::SrgbRef(amber::Chrome().danger), kBg);
    }
    else if (amber::skin::Rehaut())
    {
        // Horologe: the divider is the rehaut minute track, the tagline sits
        // beneath it as six-o'clock microtext, and the one blued screw is at
        // the track's right end.
        amber::DialogPalette hp = amber::MakeDialogPalette();
        const int sr = Dpi(5, m_dpi);
        amber::skin::MinuteTrack(dc, pad, dy, x1 - pad - sr * 4, m_dpi, hp,
                                 nullptr, true);   // no numerals: the tagline sits where they would
        amber::skin::BluedScrew(dc, x1 - sr, dy - sr, sr,
                                amber::SrgbRef(amber::Chrome().neonB), kBg);
        // The tree starts directly under the divider, so the microtext that
        // would sit at six o'clock goes to the band's top right instead.
        RECT six = { x1 - Dpi(260, m_dpi), pad, x1 - Dpi(16, m_dpi),
                     pad + Dpi(18, m_dpi) };
        HGDIOBJ sf = SelectObject(dc, m_tagFont);
        SetTextColor(dc, hp.textDim);
        DrawTextW(dc, L"GPU PARTICLE TERMINAL", -1, &six,
                  DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, sf);
    }
    else if (amber::skin::Traces())
    {
        // Solder Mask: the divider is a routed copper bus, not a rule. It
        // drops one step through a 45 degree mitre and terminates in gold
        // vias, and the header carries the board's own designator.
        amber::DialogPalette tp = amber::MakeDialogPalette();
        amber::skin::TraceBus(dc, pad, dy, x1 - pad, m_dpi, tp, kBg);
        RECT dr = { x1 - Dpi(150, m_dpi), dy - Dpi(30, m_dpi), x1,
                    dy - Dpi(14, m_dpi) };
        amber::skin::Designator(dc, dr, L"J1  SESSION BUS", m_tagFont,
                                tp.textDim);
    }
    else if (amber::skin::Ornament())
    {
        // Steampunk: the divider becomes a tiling brass rail driven by a gear
        // train at the empty right-hand end of the header band. Both are
        // centred on the divider line and live entirely in the header's own
        // chrome, so no control is displaced.
        amber::DialogPalette op = amber::MakeDialogPalette();
        const int band = 22;
        const int gear = Dpi(15, m_dpi);
        const int gx = x1 - Dpi(74, m_dpi);
        const int railW = (std::max)(Dpi(80, m_dpi), (int)(gx - gear - pad));
        amber::skin::EdgeRail(dc, pad, dy - Dpi(band, m_dpi) / 2, railW, m_dpi,
                              op, band);
        amber::skin::GearCluster(dc, gx, dy, gear, op, kBg);
    }
    else if (Hairline())
    {
        // Blueprint: the dialog is a drawing sheet. A double drafted border
        // runs around the whole page, the wordmark sits inside a title block
        // ruled off in the corner, and ticks mark every corner of the sheet.
        const amber::ChromeSpec& ch = amber::Chrome();
        int pen = (std::max)(1, Dpi((int)ch.outline, m_dpi));
        HPEN ink = CreatePen(PS_SOLID, pen, amber::SrgbRef(ch.neonB));
        HGDIOBJ op = SelectObject(dc, ink);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        int m0 = Dpi(6, m_dpi), m1 = Dpi(11, m_dpi);
        Rectangle(dc, m0, m0, rc.right - m0, rc.bottom - m0);
        Rectangle(dc, m1, m1, rc.right - m1, rc.bottom - m1);
        // Title block around the wordmark.
        int tbR = pad + Dpi(300, m_dpi), tbB = pad + bannerH - Dpi(14, m_dpi);
        Rectangle(dc, m1, m1, tbR, tbB);
        MoveToEx(dc, m1, tbB - Dpi(16, m_dpi), nullptr);
        LineTo(dc, tbR, tbB - Dpi(16, m_dpi));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(ink);
        HudTicks(dc, RECT{ m1, m1, rc.right - m1, rc.bottom - m1 },
                 Dpi(16, m_dpi), amber::SrgbRef(ch.neonA));
    }
    else if (amber::Chrome().lightGround)
    {
        // Swiss: one heavy rule under the wordmark and a single red square at
        // the left margin. No gradient, no glow, nothing else.
        const amber::ChromeSpec& ch = amber::Chrome();
        HBRUSH rule = CreateSolidBrush(amber::SrgbRef(ch.neonB));
        RECT rr = { pad, dy, x1, dy + Dpi(4, m_dpi) };
        FillRect(dc, &rr, rule);
        DeleteObject(rule);
        HBRUSH red = CreateSolidBrush(amber::SrgbRef(ch.neonA));
        RECT sq = { pad, dy - Dpi(16, m_dpi), pad + Dpi(12, m_dpi), dy };
        FillRect(dc, &sq, red);
        DeleteObject(red);
    }
    else
    for (int i = 0; i < 96; ++i)
    {
        float t = i / 95.0f;
        int xs = x0 + (int)((x1 - x0) * (i / 96.0f));
        int xe = x0 + (int)((x1 - x0) * ((i + 1) / 96.0f)) + 1;
        auto ch = [&](int a, int b) { return a + (int)((b - a) * t); };
        COLORREF from = Skinned() ? kBorderHot : RGB(255, 170, 48);
        COLORREF to = Skinned() ? Dim(kBorderHot, 0.08f) : RGB(26, 18, 9);
        HBRUSH band = CreateSolidBrush(RGB(ch(GetRValue(from), GetRValue(to)),
                                           ch(GetGValue(from), GetGValue(to)),
                                           ch(GetBValue(from), GetBValue(to))));
        RECT seg = { xs, dy, (std::min)(xe, x1), dy + Dpi(2, m_dpi) };
        FillRect(dc, &seg, band);
        DeleteObject(band);
    }

    // ---- outlines around every visible field (focus ring in hot amber) ---
    HWND focus = GetFocus();
    for (HWND h = GetWindow(m_dlg, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(h))
            continue;
        wchar_t cls[16] = L"";
        GetClassNameW(h, cls, 16);
        if (_wcsicmp(cls, L"Edit") != 0 && _wcsicmp(cls, L"ListBox") != 0 &&
            _wcsicmp(cls, L"ComboBox") != 0)
            continue;
        RECT r;
        GetWindowRect(h, &r);
        MapWindowPoints(nullptr, m_dlg, (POINT*)&r, 2);
        if (_wcsicmp(cls, L"ComboBox") == 0)
            r.bottom = r.top + Dpi(26, m_dpi);   // the closed part only
        InflateRect(&r, Dpi(2, m_dpi), Dpi(2, m_dpi));
        COLORREF line = !IsWindowEnabled(h) ? Dim(kBorder, 0.6f)
                        : (h == focus)      ? kBorderHot
                                            : kBorder;
        if (Skinned())
        {
            // Chamfered outline (hollow polygon), a soft outer glow when
            // focused, and HUD bracket ticks on the corners.
            const int cut = Dpi((int)amber::Chrome().chamfer, m_dpi);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            if (h == focus)
            {
                RECT g = r;
                InflateRect(&g, 2, 2);
                HPEN gp = CreatePen(PS_SOLID, 1, Dim(kBorderHot, 0.45f));
                HGDIOBJ op0 = SelectObject(dc, gp);
                SkinShape(dc, g, cut + 2, 0);
                SelectObject(dc, op0);
                DeleteObject(gp);
            }
            HPEN pen = CreatePen(PS_SOLID, 1, line);
            HGDIOBJ op = SelectObject(dc, pen);
            SkinShape(dc, r, cut, 0);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            if (amber::Chrome().hudBrackets && IsWindowEnabled(h))
                HudTicks(dc, r, Dpi(6, m_dpi), h == focus ? kBorderHot : Dim(kBorderHot, 0.55f));
            continue;
        }
        HBRUSH b = CreateSolidBrush(line);
        FrameRect(dc, &r, b);
        if (h == focus)
        {
            InflateRect(&r, 1, 1);
            FrameRect(dc, &r, b);
        }
        DeleteObject(b);
    }

    // ---- the Direct2D finish over the header band --------------------------
    // Gradients, glows and grain the GDI pass cannot draw, laid over it. The
    // band only: the content area belongs to the controls.
    if (Skinned())
    {
        RECT band = { 0, 0, rc.right, dy - Dpi(2, m_dpi) };
        amber::finish::Pass p(dc, band);
        if (p.ok())
        {
            const amber::ChromeSpec& ch = amber::Chrome();
            if (amber::skin::Rehaut())
            {
                // Sunburst: spokes from a centre well above the band, so the
                // grain sweeps across it the way a brushed dial is finished.
                const float cx = rc.right * 0.5f, cy = -static_cast<float>(band.bottom) * 2.2f;
                p.Sunburst(cx, cy, band.bottom * 4.0f, 240, RGB(0xC9, 0xCC, 0xD0), 0.045f);
                p.Radial(band, RGB(255, 255, 255), 0.07f, RGB(0, 0, 0), 0.0f);
            }
            else if (amber::skin::Meter())
                p.Gradient(band, RGB(255, 255, 255), 0.14f, RGB(0, 0, 0), 0.06f);
            else if (amber::skin::Glaze())
                p.Radial(band, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.40f);
            else if (amber::skin::Stitch())
                p.Radial(band, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.30f);
            else if (amber::skin::Ornament())
                p.Radial(band, RGB(0xFF, 0xD8, 0x9A), 0.06f, RGB(0, 0, 0), 0.22f);
            else if (amber::skin::Impression() || amber::skin::Traces() || ch.pills)
            {
                // Paper, mask and LCARS stay flat by design.
            }
            else if (ch.glow)
            {
                // Neon skins: a bloom behind the wordmark.
                p.Glow(static_cast<float>(pad + Dpi(70, m_dpi)), static_cast<float>(pad + Dpi(22, m_dpi)),
                       static_cast<float>(Dpi(120, m_dpi)),
                       amber::SrgbRef(ch.neonA), 0.16f);
            }
        }
    }
}

// Owner-drawn check box / radio button in the skin's shape language: a
// chamfered box (check) or a hexagon (radio) with a neon core when set.
static void DrawCheckRadio(const DRAWITEMSTRUCT& dis, int kind, bool checked,
                           HFONT font, UINT dpi, HBRUSH bgBrush)
{
    HDC dc = dis.hDC;
    RECT rc = dis.rcItem;
    FillRect(dc, &rc, bgBrush);
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool hover = GetPropW(dis.hwndItem, kHoverProp) != nullptr;
    const bool focus = (dis.itemState & ODS_FOCUS) != 0;
    const int box = Dpi(14, dpi);
    const int cy = (rc.top + rc.bottom) / 2;
    RECT b = { rc.left + 1, cy - box / 2, rc.left + 1 + box, cy + box / 2 };

    COLORREF line = disabled ? Dim(kBorder, 0.6f) : (hover || focus || checked) ? kBorderHot : kBorder;
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    if (Pills())
    {
        // LCARS toggles: a pill for a check box, a round pip for a radio;
        // filled orange when set, outlined in lavender when clear.
        RECT pb = b;
        if (kind == 1)
            pb.right = pb.left + box * 2;
        COLORREF fillC = checked ? (disabled ? Dim(kBorderHot, 0.5f) : kBorderHot) : kBg;
        HBRUSH fb = CreateSolidBrush(fillC);
        HGDIOBJ ob2 = SelectObject(dc, fb);
        int rr = pb.bottom - pb.top;
        RoundRect(dc, pb.left, pb.top, pb.right, pb.bottom, rr, rr);
        SelectObject(dc, ob2);
        DeleteObject(fb);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        wchar_t text2[256] = L"";
        GetWindowTextW(dis.hwndItem, text2, 256);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? kTextDis : (hover ? kBorderHot : kText));
        HGDIOBJ of2 = SelectObject(dc, font);
        RECT tr2 = rc;
        tr2.left = pb.right + Dpi(8, dpi);
        DrawTextW(dc, text2, -1, &tr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of2);
        return;
    }
    if (Hairline())
    {
        // Blueprint: drafted symbols. A square with a cross through it for a
        // check, a circle with a solid centre for a radio — both in ink, both
        // unfilled, the way a legend marks a state on a drawing.
        if (kind == 2)
        {
            Ellipse(dc, b.left, b.top, b.right, b.bottom);
            if (checked)
            {
                HBRUSH core = CreateSolidBrush(disabled ? Dim(kBorderHot, 0.5f)
                                                        : kBorderHot);
                HGDIOBJ ob2 = SelectObject(dc, core);
                int in = Dpi(4, dpi);
                Ellipse(dc, b.left + in, b.top + in, b.right - in, b.bottom - in);
                SelectObject(dc, ob2);
                DeleteObject(core);
            }
        }
        else
        {
            Rectangle(dc, b.left, b.top, b.right, b.bottom);
            if (checked)
            {
                MoveToEx(dc, b.left + 2, b.top + 2, nullptr);
                LineTo(dc, b.right - 2, b.bottom - 2);
                MoveToEx(dc, b.right - 3, b.top + 2, nullptr);
                LineTo(dc, b.left + 1, b.bottom - 2);
            }
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        wchar_t text3[256] = L"";
        GetWindowTextW(dis.hwndItem, text3, 256);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? kTextDis : (hover ? kBorderHot : kText));
        HGDIOBJ of3 = SelectObject(dc, font);
        RECT tr3 = rc;
        tr3.left = b.right + Dpi(8, dpi);
        DrawTextW(dc, text3, -1, &tr3,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of3);
        return;
    }
    if (kind == 2 && amber::Chrome().chamfer <= 0.0f)
    {
        // A skin with no chamfer has no business drawing a cut hexagon: the
        // radio is a plain circle with a solid centre when set.
        Ellipse(dc, b.left, b.top, b.right, b.bottom);
        if (checked)
        {
            HBRUSH core = CreateSolidBrush(disabled ? Dim(kBorderHot, 0.5f)
                                                    : kBorderHot);
            HGDIOBJ ob2 = SelectObject(dc, core);
            int in = Dpi(4, dpi);
            Ellipse(dc, b.left + in, b.top + in, b.right - in, b.bottom - in);
            SelectObject(dc, ob2);
            DeleteObject(core);
        }
    }
    else if (kind == 2)
    {
        // Hexagon.
        int w = b.right - b.left, hh = b.bottom - b.top;
        POINT p[6] = {
            { b.left + w / 4, b.top }, { b.right - w / 4, b.top }, { b.right, b.top + hh / 2 },
            { b.right - w / 4, b.bottom }, { b.left + w / 4, b.bottom }, { b.left, b.top + hh / 2 },
        };
        Polygon(dc, p, 6);
        if (checked)
        {
            HBRUSH core = CreateSolidBrush(disabled ? Dim(kBorderHot, 0.5f) : kBorderHot);
            HGDIOBJ ob2 = SelectObject(dc, core);
            int in = Dpi(4, dpi);
            POINT q[6] = {
                { b.left + w / 4 + in / 2, b.top + in }, { b.right - w / 4 - in / 2, b.top + in },
                { b.right - in, b.top + hh / 2 }, { b.right - w / 4 - in / 2, b.bottom - in },
                { b.left + w / 4 + in / 2, b.bottom - in }, { b.left + in, b.top + hh / 2 },
            };
            Polygon(dc, q, 6);
            SelectObject(dc, ob2);
            DeleteObject(core);
        }
    }
    else
    {
        SkinShape(dc, b, Dpi(4, dpi), 0);
        if (checked)
        {
            // Neon core: a diamond, with a dimmer halo square behind it.
            HBRUSH halo = CreateSolidBrush(Dim(kBorderHot, 0.30f));
            RECT hr = b;
            InflateRect(&hr, -2, -2);
            FillRect(dc, &hr, halo);
            DeleteObject(halo);
            HBRUSH core = CreateSolidBrush(disabled ? Dim(kBorderHot, 0.5f) : kBorderHot);
            HGDIOBJ ob2 = SelectObject(dc, core);
            int cx = (b.left + b.right) / 2, m = Dpi(4, dpi);
            POINT d[4] = { { cx, cy - m }, { cx + m, cy }, { cx, cy + m }, { cx - m, cy } };
            Polygon(dc, d, 4);
            SelectObject(dc, ob2);
            DeleteObject(core);
        }
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    // ---- the Direct2D finish: a well when clear, a bloom when set ----------
    if (Skinned() && !Hairline() && !Pills())
    {
        amber::finish::Pass p(dc, b);
        if (p.ok())
        {
            const float fx = (b.left + b.right) * 0.5f, fy = (b.top + b.bottom) * 0.5f;
            if (checked)
                p.Glow(fx, fy, (b.right - b.left) * 0.85f, kBorderHot, disabled ? 0.15f : 0.38f);
            else
            {
                p.InnerShadow(b, Dpi(3, dpi), 0.32f);
                p.InnerLight(b, Dpi(2, dpi), 0.18f);
            }
        }
    }
    wchar_t text[256] = L"";
    GetWindowTextW(dis.hwndItem, text, 256);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? kTextDis : (hover ? kSelText : kText));
    HGDIOBJ of = SelectObject(dc, font);
    RECT tr = rc;
    tr.left = b.right + Dpi(8, dpi);
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

void ConnectionDialog::DrawThemedButton(const DRAWITEMSTRUCT& dis)
{
    if (int kind = (int)(INT_PTR)GetPropW(dis.hwndItem, kKindProp))
    {
        DrawCheckRadio(dis, kind, GetPropW(dis.hwndItem, kCheckProp) != nullptr,
                       m_font, m_dpi, m_bgBrush);
        return;
    }
    HDC dc = dis.hDC;
    RECT rc = dis.rcItem;
    const bool accent = dis.CtlID == IdOpen;
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool hover = GetPropW(dis.hwndItem, kHoverProp) != nullptr;

    FillRect(dc, &rc, m_bgBrush);   // rounded corners show dialog ground

    COLORREF fill = accent ? (pressed ? RGB(206, 124, 22)
                              : hover ? RGB(255, 190, 82)
                                      : kAccent)
                           : (pressed ? RGB(54, 38, 17)
                              : hover ? RGB(45, 32, 15)
                                      : kField);
    COLORREF line = accent ? RGB(255, 216, 150)
                    : (hover || (dis.itemState & ODS_FOCUS)) ? kBorderHot
                                                             : kBorder;
    COLORREF txt = accent   ? kAccentTx
                   : disabled ? kTextDis
                   : hover    ? kSelText
                              : kText;
    if (amber::Chrome().darkText)
    {
        // LCARS: solid coloured segments with black lettering.
        const amber::ChromeSpec& ch = amber::Chrome();
        fill = accent ? (hover ? amber::SrgbRef(ch.neonB) : kAccent)
                      : (hover ? amber::SrgbRef(ch.bars[1]) : amber::SrgbRef(ch.bars[0]));
        if (disabled)
            fill = Dim(fill, 0.45f);
        line = fill;
        // Ink chosen by the fill's own luma, not assumed black: a skin whose
        // bar palette includes near-black (Swiss) would otherwise print black
        // lettering on a black button.
        txt = amber::InkOn((GetRValue(fill) << 16) | (GetGValue(fill) << 8) |
                           GetBValue(fill));
    }
    if (Hairline())
    {
        // Blueprint: buttons are drafted outlines. The primary one is inked in
        // white and the rest in cyan; nothing is filled, so the sheet shows
        // through every control.
        const amber::ChromeSpec& ch = amber::Chrome();
        fill = kBg;
        line = disabled  ? kTextDis
               : accent  ? amber::SrgbRef(ch.neonA)
               : hover   ? amber::SrgbRef(ch.neonA)
                         : amber::SrgbRef(ch.neonB);
        txt = line;
    }

    if (amber::skin::Impression())
    {
        // Letterpress: the primary is foil-stamped; everything else is a
        // blind deboss — no ink, only the impression — lettered in the ink.
        // Both are finished after the shape is drawn.
        fill = kBg;
        line = kBg;
        txt = disabled ? kTextDis : kText;
    }
    else if (amber::skin::Stitch())
    {
        // Atelier: second-cut hide, brass for the primary with dark ink. The
        // stitch and the painted edge are added after the shape.
        const amber::ChromeSpec& ch = amber::Chrome();
        fill = accent ? kAccent
               : amber::SrgbRef(hover ? ch.bars[1] : ch.bars[0]);
        line = kBorder;
        txt = accent ? kAccentTx : (disabled ? kTextDis : kText);
    }
    else if (amber::skin::Glaze())
    {
        // Tenmoku: a pooled tile; the primary is raw stoneware.
        const amber::ChromeSpec& ch = amber::Chrome();
        fill = accent ? kAccent
               : amber::SrgbRef(hover ? ch.bars[1] : ch.bars[0]);
        line = amber::skin::Dim(fill, 0.55f);
        txt = accent ? kAccentTx : (disabled ? kTextDis : kText);
    }
    const int cut = Skinned() ? Dpi((int)amber::Chrome().chamfer, m_dpi) : 0;
    // Hazard skins stripe the destructive control instead of colouring it.
    const bool hazardBtn = Skinned() && amber::Chrome().hazard &&
                           dis.CtlID == IdDelete && !disabled;
    if (hazardBtn)
    {
        RECT hz = rc;
        InflateRect(&hz, -Dpi(1, m_dpi), -Dpi(1, m_dpi));
        COLORREF warn = amber::SrgbRef(amber::Chrome().neonB);
        HazardFill(dc, hz, warn, m_dpi);
        // Clear band through the middle: striping behind lettering is
        // unreadable, so the label gets a solid plate the way a real hazard
        // marking leaves its text panel clear.
        HBRUSH plate = CreateSolidBrush(warn);
        RECT band = { hz.left, (hz.top + hz.bottom) / 2 - Dpi(9, m_dpi),
                      hz.right, (hz.top + hz.bottom) / 2 + Dpi(9, m_dpi) };
        FillRect(dc, &band, plate);
        DeleteObject(plate);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        HGDIOBJ ofh = SelectObject(dc, m_font);
        wchar_t th[128] = L"";
        GetWindowTextW(dis.hwndItem, th, 128);
        // No DT_NOPREFIX: "&Delete" must render as an accelerator, not with a
        // literal ampersand.
        DrawTextW(dc, th, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, ofh);
        return;
    }
    if (Skinned() && (hover || accent) && !disabled)
    {
        // Neon glow: a dimmer outline just outside the button.
        RECT g = rc;
        InflateRect(&g, -1, -1);
        HPEN gp = CreatePen(PS_SOLID, 1, Dim(accent ? kAccent : kBorderHot, hover ? 0.6f : 0.4f));
        HGDIOBJ op0 = SelectObject(dc, gp);
        HGDIOBJ ob0 = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        SkinShape(dc, rc, cut + 1, Dpi(8, m_dpi));
        SelectObject(dc, ob0);
        SelectObject(dc, op0);
        DeleteObject(gp);
        rc = g;
        InflateRect(&rc, -1, -1);
    }
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID,
                         Hairline()
                             ? (std::max)(1, Dpi((int)amber::Chrome().outline, m_dpi))
                             : 1,
                         line);
    HGDIOBJ ob = SelectObject(dc, fillBrush);
    HGDIOBJ op = SelectObject(dc, pen);
    SkinShape(dc, rc, cut, Dpi(8, m_dpi));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fillBrush);
    DeleteObject(pen);
    if (amber::skin::Impression())
    {
        if (accent)
        {
            RECT fr = rc;
            InflateRect(&fr, -1, -1);
            amber::skin::Foil(dc, fr);
        }
        else
            amber::skin::Impress(dc, rc, kBorder, RGB(255, 255, 255));
    }
    else if (amber::skin::Stitch())
    {
        amber::skin::EdgePaint(dc, rc, m_dpi, kBorder);
        RECT in = rc;
        InflateRect(&in, -Dpi(3, m_dpi), -Dpi(3, m_dpi));
        amber::skin::StitchRect(dc, in, 0, m_dpi, accent ? RGB(0x5C, 0x45, 0x26) : kText);
    }
    else if (amber::skin::Meter())
        amber::skin::AppliedIndex(dc, rc, RGB(0xE6, 0xE8, 0xEB), RGB(0x7E, 0x82, 0x87));
    else if (amber::skin::Glaze())
        amber::skin::Pool(dc, rc, fill, amber::skin::kRust);
    // ---- the Direct2D finish over the plate: what GDI cannot do ------------
    if (Skinned() && !Hairline() && !disabled)
    {
        amber::finish::Pass p(dc, rc);
        if (p.ok())
        {
            const float cx = (rc.left + rc.right) * 0.5f, cy = (rc.top + rc.bottom) * 0.5f;
            const float hw = (rc.right - rc.left) * 0.5f;
            if (amber::skin::Impression())
            {
                if (accent)
                {
                    // Foil: a metallic three-stop sweep with a specular band.
                    p.Metal(rc, RGB(0x8E, 0x5A, 0x2B), RGB(0xF2, 0xC4, 0x94), 0.38f, 0.55f);
                    p.Sheen(rc, 0.30f, 0.45f);
                }
                else
                {
                    // Blind deboss: pressed into the stock, so the shade is soft
                    // and the light rises from the lower edges.
                    p.InnerShadow(rc, Dpi(4, m_dpi), 0.18f);
                    p.InnerLight(rc, Dpi(3, m_dpi), 0.7f);
                }
            }
            else if (amber::skin::Glaze())
            {
                // Pooling: the glaze thickens toward the edges.
                p.Radial(rc, RGB(255, 255, 255), 0.07f, RGB(0, 0, 0), 0.50f);
                p.InnerShadow(rc, Dpi(3, m_dpi), 0.35f);
            }
            else if (amber::skin::Meter())
            {
                // A machined pushbutton: lit from above, shaded below.
                p.Gradient(rc, RGB(255, 255, 255), 0.26f, RGB(0, 0, 0), 0.14f);
                if (accent)
                    p.Glow(cx, cy, hw * 0.9f, kAccent, 0.22f);
            }
            else if (amber::skin::Stitch())
            {
                // Leather: a soft sheen where the hide catches light.
                p.Radial(rc, RGB(255, 255, 255), 0.07f, RGB(0, 0, 0), 0.28f);
            }
            else if (amber::skin::Rehaut())
            {
                // Rhodium and lume both take a hard top light.
                p.Sheen(rc, accent ? 0.28f : 0.12f, 0.5f);
            }
            else if (amber::skin::Ornament())
            {
                // Brass: a warm metallic sweep.
                p.Metal(rc, RGB(0x6B, 0x4E, 0x25), RGB(0xE8, 0xC8, 0x8A), 0.42f, accent ? 0.45f : 0.22f);
            }
            else if (amber::skin::Traces())
            {
                // ENIG gold has a faint satin sheen; the mask does not.
                if (accent)
                    p.Sheen(rc, 0.22f, 0.5f);
            }
            else if (amber::Chrome().pills)
            {
                // LCARS / Brass segments: a soft top light only.
                p.Sheen(rc, 0.12f, 0.5f);
            }
            else
            {
                // Cyberpunk, Nostromo, Neo-Tokyo, Swiss: a sheen, and on the
                // glow skins a bloom around the primary action.
                p.Sheen(rc, accent ? 0.16f : 0.07f, 0.5f);
                if (accent && amber::Chrome().glow)
                    p.Glow(cx, cy, hw * 0.8f, kAccent, 0.20f);
            }
            if (hover && !accent)
                p.Sheen(rc, 0.08f, 1.0f);
        }
    }
    if (Skinned() && amber::Chrome().hudBrackets && !accent)
    {
        // Corner ticks in the accent, a little inside the shape.
        HBRUSH tb = CreateSolidBrush(Dim(line, hover ? 1.0f : 0.8f));
        RECT t;
        int L = Dpi(5, m_dpi);
        t = { rc.right - 1 - L, rc.top + 2, rc.right - 1, rc.top + 3 }; FillRect(dc, &t, tb);
        t = { rc.left + 2, rc.bottom - 3, rc.left + 2 + L, rc.bottom - 2 }; FillRect(dc, &t, tb);
        DeleteObject(tb);
    }

    wchar_t text[128] = L"";
    GetWindowTextW(dis.hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt);
    HGDIOBJ of = SelectObject(dc, m_font);
    RECT tr = rc;
    if (pressed)
        OffsetRect(&tr, 1, 1);
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

void ConnectionDialog::DrawSessionRow(const DRAWITEMSTRUCT& dis)
{
    HDC dc = dis.hDC;
    RECT rc = dis.rcItem;
    const bool sel = (dis.itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(sel ? kSelBg : kField);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if ((int)dis.itemID < 0)
        return;

    if (sel)
    {
        RECT bar = rc;
        bar.right = bar.left + Dpi(3, m_dpi);
        HBRUSH ab = CreateSolidBrush(kAccent);
        FillRect(dc, &bar, ab);
        DeleteObject(ab);
    }

    int len = (int)SendMessageW(dis.hwndItem, LB_GETTEXTLEN, dis.itemID, 0);
    std::wstring text((size_t)std::max(len, 0), L'\0');
    if (len > 0)
        SendMessageW(dis.hwndItem, LB_GETTEXT, dis.itemID, (LPARAM)text.data());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, sel ? kSelText : kText);
    HGDIOBJ of = SelectObject(dc, m_monoFont);
    RECT tr = rc;
    tr.left += Dpi(10, m_dpi);
    DrawTextW(dc, text.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

void ConnectionDialog::SetStatus(const std::wstring& text)
{
    if (m_status)
        SetText(m_status, text);
}

} // namespace amber
