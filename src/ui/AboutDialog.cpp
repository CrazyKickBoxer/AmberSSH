#include "AboutDialog.h"

#include "SkinDraw.h"
#include "SkinFinish.h"
#include "Theme.h"
#include "resource.h"

#include <algorithm>
#include <string>

namespace amber
{
namespace
{

constexpr wchar_t kClassName[] = L"AmberSSHAbout";

// Client size in logical pixels. The mark is square and generous, and the text
// column is wide enough that the description sits on two comfortable lines.
constexpr int kClientW = 660;
constexpr int kClientH = 408;
constexpr int kLogoPx = 176;

struct AboutState
{
    DialogPalette pal{};
    HICON logo = nullptr;
    HFONT wordFont = nullptr;    // "Amber SSH"
    HFONT tagFont = nullptr;     // "GPU PARTICLE TERMINAL"
    HFONT bodyFont = nullptr;    // version + description
    HFONT smallFont = nullptr;   // copyright
    UINT dpi = 96;
};

int Px(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// The face this skin letters in, falling back to the system UI font.
const wchar_t* UiFace()
{
    if (ChromeSkinned() && ChromeFace())
        return ChromeFace();
    return L"Segoe UI";
}

HFONT MakeFont(UINT dpi, int px, int weight, const wchar_t* face)
{
    return CreateFontW(-Px(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

// Draw one run of text and return the pen position after it.
int Run(HDC dc, int x, int y, const wchar_t* s, COLORREF c)
{
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s, static_cast<int>(wcslen(s)));
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
    return x + sz.cx;
}

void PaintAbout(HWND hwnd, AboutState& st)
{
    PAINTSTRUCT ps;
    HDC front = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Double buffered: the mark's glow is alpha-blended and would flicker if
    // it were composited straight onto the window DC.
    HDC dc = CreateCompatibleDC(front);
    HBITMAP bmp = CreateCompatibleBitmap(front, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    const ChromeSpec& ch = Chrome();
    const UINT dpi = st.dpi;
    HBRUSH ground = CreateSolidBrush(st.pal.bg);
    FillRect(dc, &rc, ground);
    DeleteObject(ground);

    // Faint scanlines on the skins that use them elsewhere, so the box belongs
    // to the same surface as the terminal strip and the connection manager.
    if (ChromeSkinned() && ch.scanlines)
    {
        HBRUSH sl = CreateSolidBrush(ScaleSrgb(ch.neonA, 0.10f));
        for (int y = rc.top; y < rc.bottom; y += Px(4, dpi))
        {
            RECT l = { rc.left, y, rc.right, y + 1 };
            FillRect(dc, &l, sl);
        }
        DeleteObject(sl);
    }

    // Horologe: the whole panel is engine-turned. The About box paints its
    // own type over a transparent ground, so the pattern can run under it
    // without the solid patches a static control would leave.
    if (ChromeSkinned() && skin::Rehaut())
        skin::Guilloche(dc, rc, dpi, st.pal.bg);

    // Reference: brushed plate. Tenmoku: oil spots and the rust rim. Atelier:
    // the maker's label — painted edge, stitch, a snap at each corner. All
    // drawn before the mark and the type, so content sits on top.
    if (ChromeSkinned() && skin::Meter())
        skin::Brushed(dc, rc, dpi, st.pal.bg);
    if (ChromeSkinned() && skin::Glaze())
    {
        skin::OilSpot(dc, rc, dpi, skin::kOilSpot);
        RECT rim = { 0, 0, rc.right, Px(2, dpi) };
        HBRUSH rb = CreateSolidBrush(skin::kRust);
        FillRect(dc, &rim, rb);
        DeleteObject(rb);
    }
    if (ChromeSkinned() && skin::Stitch())
    {
        skin::EdgePaint(dc, rc, dpi, st.pal.border);
        RECT in = { Px(4, dpi), Px(4, dpi), rc.right - Px(4, dpi), rc.bottom - Px(4, dpi) };
        skin::StitchRect(dc, in, 0, dpi, st.pal.text);
        const int s = Px(14, dpi);
        const POINT snaps[4] = { { s, s }, { rc.right - s, s },
                                 { s, rc.bottom - s }, { rc.right - s, rc.bottom - s } };
        for (const POINT& p : snaps)
            skin::Rivet(dc, p.x, p.y, Px(4, dpi), st.pal.accent, RGB(0x5C, 0x45, 0x26));
    }

    // Steampunk: the panel is framed by brass filigree at all four corners.
    // Drawn before the mark and the type so the content always sits on top of
    // the ornament, never the other way round.
    if (ChromeSkinned() && skin::Ornament())
    {
        const int fs = Px(64, dpi), fm = Px(8, dpi);
        skin::CornerFiligree(dc, fm, fm, fs, 0, st.pal, st.pal.bg);
        skin::CornerFiligree(dc, rc.right - fm - fs, fm, fs, 1, st.pal, st.pal.bg);
        skin::CornerFiligree(dc, rc.right - fm - fs, rc.bottom - fm - fs, fs, 2,
                             st.pal, st.pal.bg);
        skin::CornerFiligree(dc, fm, rc.bottom - fm - fs, fs, 3, st.pal, st.pal.bg);
    }

    // LCARS reserves a column at the left for the frame's side bar, so the
    // mark and the type shift right rather than sitting on top of it.
    const bool lcars = ChromeSkinned() && ch.elbow;
    const int lcarsV = Px(26, dpi);
    const int pad = Px(34, dpi);
    const int logo = Px(kLogoPx, dpi);
    const int logoX = pad + (lcars ? lcarsV + Px(10, dpi) : 0);
    const int logoY = (rc.bottom - logo) / 2 - Px(10, dpi);

    // LCARS: the panel is framed the way the connection manager is — elbow at
    // the top left, the block column continuing its leg on the same constant
    // gutter, a foot bar, and an end cap on each bar.
    if (lcars)
    {
        const int m = Px(14, dpi);
        const int H = Px(12, dpi);
        const int G = Px(5, dpi);
        const int right = rc.right - m;
        const int footY = rc.bottom - m - H;
        const int end = skin::Elbow(dc, m, m, right, H, lcarsV,
                                    H + H + Px(30, dpi), SrgbRef(ch.bars[0]));
        skin::BlockColumn(dc, m, end + G, lcarsV, footY - G, G, ch.bars, 1, dpi);
        HBRUSH foot = CreateSolidBrush(SrgbRef(ch.bars[2]));
        RECT fr = { m, footY, right, footY + H };
        FillRect(dc, &fr, foot);
        DeleteObject(foot);
        skin::EndCap(dc, right, m, Px(90, dpi), H, G, SrgbRef(ch.bars[1]),
                     st.pal.bg);
        skin::EndCap(dc, right, footY, Px(64, dpi), H, G, SrgbRef(ch.bars[3]),
                     st.pal.bg);
    }
    // The mark is an additive glow drawn for a dark ground: on paper it washes
    // out to a pale haze. A light skin therefore gets the dark tile the brand
    // sheet uses for the desktop icon, and the mark reads correctly on it.
    if (ChromeSkinned() && ch.lightGround)
    {
        const int m = Px(10, dpi);
        RECT tile = { logoX - m, logoY - m, logoX + logo + m, logoY + logo + m };
        HBRUSH plate = CreateSolidBrush(RGB(18, 18, 20));
        HPEN edge = CreatePen(PS_SOLID, 1, RGB(48, 48, 52));
        HGDIOBJ ob = SelectObject(dc, plate);
        HGDIOBJ op = SelectObject(dc, edge);
        int r = Px(22, dpi);
        RoundRect(dc, tile.left, tile.top, tile.right, tile.bottom, r, r);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(edge);
        DeleteObject(plate);
    }
    if (ChromeSkinned() && skin::Glaze())
    {
        // Tenmoku: the mark sits on a glazed tile with its own foot ring.
        const int m = Px(10, dpi);
        RECT tile = { logoX - m, logoY - m, logoX + logo + m, logoY + logo + m };
        skin::Pool(dc, tile, st.pal.field, skin::kRust);
        skin::Foot(dc, tile, dpi, skin::kClay);
    }
    if (st.logo)
        DrawIconEx(dc, logoX, logoY, st.logo, logo, logo, 0, nullptr, DI_NORMAL);

    const int tx = logoX + logo + Px(38, dpi);
    SetBkMode(dc, TRANSPARENT);

    // ---- wordmark: the product name in the accent, the protocol in body ink
    int y = logoY + Px(6, dpi);
    HGDIOBJ oldFont = SelectObject(dc, st.wordFont);
    int penX = Run(dc, tx, y, L"Amber", st.pal.banner);
    Run(dc, penX + Px(10, dpi), y, L"SSH", st.pal.text);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    y += tm.tmHeight + Px(2, dpi);

    // ---- tagline, letterspaced small caps
    SelectObject(dc, st.tagFont);
    SetTextCharacterExtra(dc, Px(4, dpi));
    Run(dc, tx + Px(2, dpi), y, kProductTagline, st.pal.banner);
    SetTextCharacterExtra(dc, 0);
    GetTextMetricsW(dc, &tm);
    y += tm.tmHeight + Px(24, dpi);

    // ---- version
    SelectObject(dc, st.bodyFont);
    std::wstring ver = std::wstring(L"Version ") + kProductVersion;
    Run(dc, tx, y, ver.c_str(), st.pal.text);
    GetTextMetricsW(dc, &tm);
    y += tm.tmHeight + Px(14, dpi);

    // ---- description
    RECT dr = { tx, y, rc.right - pad, y + Px(42, dpi) };
    SetTextColor(dc, st.pal.textDim);
    DrawTextW(dc,
              L"A next-generation SSH terminal where everything is rendered "
              L"by particles.",
              -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    y += Px(46, dpi);

    // ---- credits. Attribution is a licence obligation for most of what
    // AmberSSH links, and the About box is where a user looks for it. The
    // full terms are in THIRD-PARTY-NOTICES.md next to the executable.
    SelectObject(dc, st.smallFont);
    SetTextColor(dc, st.pal.textDis);
    RECT cr2 = { tx, y, rc.right - pad, y + Px(84, dpi) };
    DrawTextW(dc, kProductCredits, -1, &cr2,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    // ---- copyright, on the baseline of the panel
    SelectObject(dc, st.smallFont);
    SetTextColor(dc, st.pal.textDis);
    RECT cr = { tx, rc.bottom - pad - Px(20, dpi), rc.right - pad,
                rc.bottom - pad };
    DrawTextW(dc, L"\x00A9 2026 Amber SSH. All rights reserved.", -1, &cr,
              DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    // ---- the skin's own finishing mark along the bottom edge
    if (ChromeSkinned())
    {
        HBRUSH rail = CreateSolidBrush(SrgbRef(ch.neonA));
        if (ch.outline > 0.0f)
        {
            // Blueprint: a drafted border around the sheet instead of a rail.
            DeleteObject(rail);
            HPEN ink = CreatePen(PS_SOLID, (std::max)(1, Px((int)ch.outline, dpi)),
                                 SrgbRef(ch.neonB));
            HGDIOBJ op = SelectObject(dc, ink);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            int m = Px(8, dpi);
            Rectangle(dc, m, m, rc.right - m, rc.bottom - m);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(ink);
        }
        else if (ChromeSkinned() && skin::Impression())
        {
            // Letterpress: a colophon — thick-thin rule and one fleuron.
            skin::ThickThin(dc, pad, rc.bottom - Px(14, dpi), rc.right - pad * 2,
                            st.pal.borderHot, dpi);
            skin::Fleuron(dc, rc.right / 2, Px(22, dpi), Px(10, dpi), st.pal.borderHot);
        }
        else if (ChromeSkinned() && skin::Stitch())
        {
            // The maker's label is the stitched frame drawn under the mark.
        }
        else if (ChromeSkinned() && skin::Meter())
        {
            // Reference: the model plate engraved at the foot, and a small
            // VU at rest at the top right.
            RECT plate = { pad, rc.bottom - Px(30, dpi), rc.right / 2, rc.bottom - Px(12, dpi) };
            skin::Engrave(dc, plate, L"MODEL 1.0.0  ·  SERIAL 000001", st.smallFont,
                          st.pal.textDim, RGB(0xE6, 0xE8, 0xEB),
                          DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            RECT vu = { rc.right - pad - Px(96, dpi), Px(12, dpi), rc.right - pad,
                        Px(12, dpi) + Px(56, dpi) };
            skin::VuMeter(dc, vu, 0.0, dpi, SrgbRef(ch.neonB), RGB(0xEA, 0xF0, 0xFF),
                          SrgbRef(ch.danger), SrgbRef(ch.danger), L"VU", st.smallFont);
        }
        else if (ChromeSkinned() && skin::Glaze())
        {
            // Tenmoku: the panel stands on its foot; the version is a firing
            // mark; the studio's chop sits top right.
            skin::Foot(dc, rc, dpi, skin::kClay);
            RECT fm = { rc.right / 2, rc.bottom - Px(30, dpi), rc.right - pad, rc.bottom - Px(12, dpi) };
            skin::Designator(dc, fm, L"CONE 10  ·  1.0.0", st.smallFont, st.pal.textDim);
            skin::Chop(dc, rc.right - pad - Px(16, dpi), Px(14, dpi), Px(16, dpi),
                       SrgbRef(ch.danger), st.pal.bg);
        }
        else if (ChromeSkinned() && skin::Rehaut())
        {
            // Horologe: the rehaut runs along the top edge (numbered) and the
            // foot (plain — the copyright is the six-o'clock text), and the
            // one blued screw sits at the top right.
            const int m = Px(12, dpi);
            skin::MinuteTrack(dc, m, m, rc.right - m * 2 - Px(22, dpi), dpi,
                              st.pal, st.smallFont, false);
            skin::MinuteTrack(dc, m, rc.bottom - m, rc.right - m * 2, dpi,
                              st.pal, nullptr, true);
            skin::BluedScrew(dc, rc.right - m - Px(6, dpi), m + Px(4, dpi),
                             Px(5, dpi), SrgbRef(ch.neonB), st.pal.bg);
        }
        else if (ChromeSkinned() && skin::Traces())
        {
            // Solder Mask: the panel is a board. A copper run enters at the
            // top left and terminates in a gold via; a gold run leaves along
            // the foot. Both keep clear of the mark and the type, and the
            // panel carries its own silkscreen reference designator.
            const int tw = skin::TraceW(dpi);
            skin::Trace(dc, 0, Px(26, dpi), rc.right - Px(46, dpi),
                        Px(14, dpi), tw, skin::Copper());
            skin::Via(dc, rc.right - Px(46, dpi), Px(40, dpi), tw,
                      st.pal.accent, st.pal.bg);
            skin::Trace(dc, pad, rc.bottom - Px(20, dpi),
                        rc.right - pad * 2, -Px(10, dpi), tw, st.pal.borderHot);
            skin::Via(dc, pad, rc.bottom - Px(20, dpi), tw,
                      st.pal.accent, st.pal.bg);
            skin::Via(dc, rc.right - pad, rc.bottom - Px(30, dpi), tw,
                      st.pal.accent, st.pal.bg);
            RECT ref = { rc.right / 2, Px(46, dpi), rc.right - Px(24, dpi),
                         Px(64, dpi) };
            skin::Designator(dc, ref, L"U1  AMBER SSH  REV 1.0", st.smallFont,
                             st.pal.textDis);
        }
        else if (lcars)
        {
            // The LCARS frame above already terminates the panel.
        }
        else if (ch.pills)
        {
            // LCARS / Brass: a rounded segment sweeping the foot of the panel.
            HGDIOBJ ob = SelectObject(dc, rail);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            int h = Px(10, dpi);
            RoundRect(dc, pad, rc.bottom - pad + Px(4, dpi),
                      rc.right - pad, rc.bottom - pad + Px(4, dpi) + h, h, h);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(rail);
        }
        else
        {
            RECT r = { 0, rc.bottom - Px(3, dpi), rc.right, rc.bottom };
            FillRect(dc, &r, rail);
            DeleteObject(rail);
        }
    }

    // ---- the Direct2D finish: the material's light over the whole panel ----
    if (ChromeSkinned())
    {
        finish::Pass p(dc, rc);
        if (p.ok())
        {
            const float lx = logoX + logo * 0.5f, ly = logoY + logo * 0.5f;
            if (skin::Rehaut())
            {
                // The dial: a sunburst from the centre of the mark.
                p.Sunburst(lx, ly, static_cast<float>(rc.right), 360, RGB(0xC9, 0xCC, 0xD0), 0.05f);
                p.Radial(rc, RGB(255, 255, 255), 0.06f, RGB(0, 0, 0), 0.18f);
            }
            else if (skin::Meter())
                p.Gradient(rc, RGB(255, 255, 255), 0.16f, RGB(0, 0, 0), 0.08f);
            else if (skin::Glaze())
                p.Radial(rc, RGB(255, 255, 255), 0.04f, RGB(0, 0, 0), 0.45f);
            else if (skin::Stitch())
                p.Radial(rc, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.32f);
            else if (skin::Ornament())
                p.Radial(rc, RGB(0xFF, 0xD8, 0x9A), 0.07f, RGB(0, 0, 0), 0.26f);
            else if (skin::Impression())
            {
                // Cotton stock: a whisper of a vignette, the way a sheet lies
                // under a lamp.
                p.Radial(rc, RGB(255, 255, 255), 0.0f, RGB(0x6E, 0x1B, 0x2A), 0.05f);
            }
            else if (skin::Traces())
            {
                // Mask: flat.
            }
            else if (ch.glow)
                p.Glow(lx, ly, logo * 0.9f, SrgbRef(ch.neonA), 0.14f);
        }
    }
    BitBlt(front, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<AboutState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (st)
            PaintAbout(hwnd, *st);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE)
            DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void ShowAboutDialog(HWND owner)
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        wc.hIconSm = static_cast<HICON>(
            LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!RegisterClassExW(&wc))
            return;
        registered = true;
    }

    AboutState st;
    st.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    st.pal = MakeDialogPalette();
    const wchar_t* face = UiFace();
    // The wordmark is set light and large; the rest steps down from it.
    st.wordFont = MakeFont(st.dpi, 44, FW_LIGHT, face);
    st.tagFont = MakeFont(st.dpi, 13, FW_SEMIBOLD, face);
    st.bodyFont = MakeFont(st.dpi, 16, FW_NORMAL, face);
    st.smallFont = MakeFont(st.dpi, 14, FW_NORMAL, face);
    st.logo = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                   Px(kLogoPx, st.dpi), Px(kLogoPx, st.dpi), 0));

    RECT wr = { 0, 0, Px(kClientW, st.dpi), Px(kClientH, st.dpi) };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, st.dpi);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    // Centred on the owner, or on the work area when there is none.
    RECT ow{};
    if (owner)
        GetWindowRect(owner, &ow);
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ow, 0);
    int x = ow.left + ((ow.right - ow.left) - w) / 2;
    int y = ow.top + ((ow.bottom - ow.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName,
                                L"About Amber SSH", style, x, y, w, h, owner,
                                nullptr, inst, nullptr);
    if (!hwnd)
        return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    ApplyWindowChrome(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Modal: the owner is disabled for the life of the box, and the loop ends
    // on the WM_QUIT that WM_DESTROY posts.
    if (owner)
        EnableWindow(owner, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }

    if (st.logo)
        DestroyIcon(st.logo);
    DeleteObject(st.wordFont);
    DeleteObject(st.tagFont);
    DeleteObject(st.bodyFont);
    DeleteObject(st.smallFont);
}

} // namespace amber
