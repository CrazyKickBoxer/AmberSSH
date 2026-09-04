#include "SafetyDialog.h"

#include "SkinDraw.h"
#include "SkinFinish.h"
#include "Theme.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace amber
{
namespace
{

constexpr wchar_t kClassName[] = L"AmberSSHSafety";

// Control ids. Owner-drawn buttons, so these are only ever compared.
enum : int
{
    IdYes = 1001,
    IdNo = 1002,
    IdConfirmEdit = 1003,
};

std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

int Px(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

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

// The two dialogs share a window class and a paint routine for the ground, so
// a skin only has to be taught the surface once.
enum class Kind { HostKey, Risk, TrustedX11 };

struct SafetyState
{
    Kind kind = Kind::HostKey;
    DialogPalette pal{};
    UINT dpi = 96;
    HFONT headFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT monoFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT btnFont = nullptr;
    HBRUSH ground = nullptr;

    // host key
    Sigil sigil;
    std::wstring label;
    std::wstring fingerprint;
    std::wstring mnemonic;
    std::wstring describe;
    bool changed = false;

    // risk
    const RiskReport* report = nullptr;
    ConfirmStyle style = ConfirmStyle::YesNo;
    std::wstring hostname;
    // trusted X11
    bool sessionOnly = false;
    HWND edit = nullptr;
    HWND yes = nullptr;
    HWND no = nullptr;

    bool accepted = false;
    int hover = 0;          // control id under the pointer, 0 for none
};

// ---------------------------------------------------------------- the sigil
// Drawn from the normalised 0..1 geometry, so the same figure appears at
// dialog size, tab size and status-bar size.
void PaintSigil(HDC dc, const RECT& rc, const Sigil& s, UINT dpi, bool alarm,
                const DialogPalette& pal)
{
    const ChromeSpec& ch = Chrome();
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int side = (std::min)(w, h);
    const int ox = rc.left + (w - side) / 2;
    const int oy = rc.top + (h - side) / 2;
    auto X = [&](float u) { return ox + static_cast<int>(u * side + 0.5f); };
    auto Y = [&](float v) { return oy + static_cast<int>(v * side + 0.5f); };

    // The line colour follows the skin, and an alarm overrides it entirely.
    // Hue is never the only signal: the border rhythm, the node shapes and the
    // chords carry the identity, so this reads the same in monochrome.
    COLORREF ink = pal.banner;
    if (ChromeSkinned())
        ink = SrgbRef(ch.neonA);
    if (alarm)
        ink = ChromeSkinned() ? SrgbRef(ch.danger) : RGB(0xE0, 0x40, 0x30);

    const int lw = (std::max)(1, Px(2, dpi));
    HPEN pen = CreatePen(PS_SOLID, lw, ink);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

    // ---- border rhythm: the frame is a dashed rule whose dash lengths come
    // from the key. Two hosts with the same node count still have different
    // frames, which is what makes a glance enough.
    if (!s.rhythm.empty())
    {
        float total = 0.0f;
        for (float t : s.rhythm)
            total += t;
        const int per = 4;                     // four edges
        int x = ox, y = oy;
        int at = 0;
        for (int edge = 0; edge < per; ++edge)
        {
            float run = 0.0f;
            for (size_t i = 0; i < s.rhythm.size(); ++i)
            {
                const float len = s.rhythm[(at + i) % s.rhythm.size()] / total;
                const int px = static_cast<int>(len * side + 0.5f);
                const bool on = ((at + i) & 1) == 0;
                int x0 = x, y0 = y, x1 = x, y1 = y;
                if (edge == 0) { x1 = x + px; }
                else if (edge == 1) { y1 = y + px; }
                else if (edge == 2) { x1 = x - px; }
                else { y1 = y - px; }
                if (on)
                {
                    MoveToEx(dc, x0, y0, nullptr);
                    LineTo(dc, x1, y1);
                }
                x = x1;
                y = y1;
                run += len;
                if (run >= 1.0f)
                    break;
            }
            at += 3;
            // Snap to the corner so a rounding error cannot walk the frame.
            if (edge == 0) { x = ox + side; y = oy; }
            else if (edge == 1) { x = ox + side; y = oy + side; }
            else if (edge == 2) { x = ox; y = oy + side; }
        }
    }

    // ---- corner cuts
    for (int i = 0; i < 4; ++i)
    {
        if (s.corners[i] <= 0.0f)
            continue;
        const int c = static_cast<int>(s.corners[i] * side * 0.5f);
        int x0 = ox, y0 = oy, x1 = ox, y1 = oy;
        if (i == 0) { x0 = ox; y0 = oy + c; x1 = ox + c; y1 = oy; }
        else if (i == 1) { x0 = ox + side - c; y0 = oy; x1 = ox + side; y1 = oy + c; }
        else if (i == 2) { x0 = ox + side; y0 = oy + side - c; x1 = ox + side - c; y1 = oy + side; }
        else { x0 = ox + c; y0 = oy + side; x1 = ox; y1 = oy + side - c; }
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x1, y1);
    }

    // ---- edges, then nodes on top
    for (const SigilEdge& e : s.edges)
    {
        if (e.a < 0 || e.b < 0 || e.a >= static_cast<int>(s.nodes.size()) ||
            e.b >= static_cast<int>(s.nodes.size()))
            continue;
        const SigilNode& a = s.nodes[static_cast<size_t>(e.a)];
        const SigilNode& b = s.nodes[static_cast<size_t>(e.b)];
        if (e.doubled)
        {
            // A doubled chord is drawn as two parallel rules, offset along the
            // normal — a shape difference, not a weight difference, so it
            // survives a low-resolution capture.
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float nx = len > 0.0f ? -dy / len : 0.0f;
            const float ny = len > 0.0f ? dx / len : 0.0f;
            const float off = 0.012f;
            for (int k = -1; k <= 1; k += 2)
            {
                MoveToEx(dc, X(a.x + nx * off * k), Y(a.y + ny * off * k), nullptr);
                LineTo(dc, X(b.x + nx * off * k), Y(b.y + ny * off * k));
            }
        }
        else
        {
            MoveToEx(dc, X(a.x), Y(a.y), nullptr);
            LineTo(dc, X(b.x), Y(b.y));
        }
    }

    HBRUSH fill = CreateSolidBrush(ink);
    for (const SigilNode& n : s.nodes)
    {
        const int r = (std::max)(2, static_cast<int>(n.r * side + 0.5f));
        const int cx = X(n.x), cy = Y(n.y);
        SelectObject(dc, n.filled ? fill : GetStockObject(HOLLOW_BRUSH));
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    }
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);
}

// ------------------------------------------------------------- shared ground
void PaintGround(HDC dc, const RECT& rc, const SafetyState& st)
{
    const ChromeSpec& ch = Chrome();
    const UINT dpi = st.dpi;
    HBRUSH ground = CreateSolidBrush(st.pal.bg);
    FillRect(dc, &rc, ground);
    DeleteObject(ground);

    if (ChromeSkinned() && ch.scanlines)
        skin::Scanlines(dc, rc, dpi);
    if (ChromeSkinned() && skin::Rehaut())
        skin::Guilloche(dc, rc, dpi, st.pal.bg);
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
        RECT in = { Px(4, dpi), Px(4, dpi), rc.right - Px(4, dpi),
                    rc.bottom - Px(4, dpi) };
        skin::StitchRect(dc, in, 0, dpi, st.pal.text);
    }
    if (ChromeSkinned() && skin::Ornament())
    {
        const int fs = Px(52, dpi), fm = Px(6, dpi);
        skin::CornerFiligree(dc, fm, fm, fs, 0, st.pal, st.pal.bg);
        skin::CornerFiligree(dc, rc.right - fm - fs, fm, fs, 1, st.pal, st.pal.bg);
        skin::CornerFiligree(dc, rc.right - fm - fs, rc.bottom - fm - fs, fs, 2,
                             st.pal, st.pal.bg);
        skin::CornerFiligree(dc, fm, rc.bottom - fm - fs, fs, 3, st.pal, st.pal.bg);
    }
    if (ChromeSkinned() && skin::Traces())
        skin::TraceBus(dc, Px(12, dpi), rc.bottom - Px(16, dpi),
                       rc.right - Px(24, dpi), dpi, st.pal, st.pal.bg);

    // LCARS frames its panels rather than decorating them: an elbow at the top
    // left, the block column continuing its leg on a constant gutter, a foot
    // bar and an end cap on each. Same geometry rule as the connection manager
    // and the About box (Ro = H + V, Ri = H).
    if (ChromeSkinned() && ch.elbow)
    {
        const int m = Px(12, dpi);
        const int H = Px(11, dpi);
        const int G = Px(5, dpi);
        const int V = Px(24, dpi);
        const int right = rc.right - m;
        const int footY = rc.bottom - m - H;
        const int end = skin::Elbow(dc, m, m, right, H, V, H + H + Px(26, dpi),
                                    SrgbRef(ch.bars[0]));
        skin::BlockColumn(dc, m, end + G, V, footY - G, G, ch.bars, 1, dpi);
        HBRUSH foot = CreateSolidBrush(SrgbRef(ch.bars[2]));
        RECT fr = { m, footY, right, footY + H };
        FillRect(dc, &fr, foot);
        DeleteObject(foot);
        skin::EndCap(dc, right, m, Px(80, dpi), H, G, SrgbRef(ch.bars[1]), st.pal.bg);
        skin::EndCap(dc, right, footY, Px(56, dpi), H, G, SrgbRef(ch.bars[3]),
                     st.pal.bg);
    }
}

// LCARS reserves a column at the left for the frame's side bar, so content
// shifts right rather than sitting on top of it. Every other skin returns 0.
int PadLeft(UINT dpi)
{
    if (ChromeSkinned() && Chrome().elbow)
        return Px(26, dpi) + Px(24, dpi) + Px(10, dpi);
    return Px(26, dpi);
}

// And the foot bar needs room, so the buttons rise on LCARS.
int PadBottom(UINT dpi)
{
    return ChromeSkinned() && Chrome().elbow ? Px(24, dpi) + Px(22, dpi)
                                             : Px(24, dpi);
}

// The alarm band: a full-width strip in the skin's danger colour, with the
// hazard striping on the skins that use it. Every skin gets a version of it —
// a warning that vanishes on one theme is a warning that does not exist.
void PaintAlarmBand(HDC dc, const RECT& band, const SafetyState& st,
                    const wchar_t* text)
{
    const ChromeSpec& ch = Chrome();
    const COLORREF warn = ChromeSkinned() ? SrgbRef(ch.danger)
                                          : RGB(0xC8, 0x30, 0x22);
    if (ChromeSkinned() && ch.hazard)
    {
        skin::HazardFill(dc, band, warn, st.dpi);
        HBRUSH plate = CreateSolidBrush(warn);
        RECT inner = band;
        InflateRect(&inner, -Px(28, st.dpi), -Px(5, st.dpi));
        FillRect(dc, &inner, plate);
        DeleteObject(plate);
    }
    else if (skin::Hairline())
    {
        HPEN pn = CreatePen(PS_SOLID, (std::max)(1, Px(2, st.dpi)), warn);
        HGDIOBJ op = SelectObject(dc, pn);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, band.left, band.top, band.right, band.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pn);
    }
    else
    {
        HBRUSH b = CreateSolidBrush(warn);
        if (skin::Pills())
        {
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            const int r = (band.bottom - band.top);
            RoundRect(dc, band.left, band.top, band.right, band.bottom, r, r);
            SelectObject(dc, op);
            SelectObject(dc, ob);
        }
        else
            FillRect(dc, &band, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    // Black or white, whichever reads on the danger colour — a warning that
    // cannot be read on one skin is a warning that skin does not have.
    const uint32_t warnSrgb = (static_cast<uint32_t>(GetRValue(warn)) << 16) |
                              (static_cast<uint32_t>(GetGValue(warn)) << 8) |
                              static_cast<uint32_t>(GetBValue(warn));
    SetTextColor(dc, skin::Hairline() ? warn : InkOn(warnSrgb));
    RECT t = band;
    DrawTextW(dc, text, -1, &t,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ------------------------------------------------------------------ host key
void PaintHostKey(HDC dc, const RECT& rc, SafetyState& st)
{
    const UINT dpi = st.dpi;
    PaintGround(dc, rc, st);
    SetBkMode(dc, TRANSPARENT);

    const int pad = PadLeft(dpi);
    const int padR = Px(26, dpi) + (PadLeft(dpi) > Px(26, dpi) ? Px(16, dpi) : 0);
    int y = Px(26, dpi);

    if (st.changed)
    {
        RECT band = { pad, y, rc.right - padR, y + Px(30, dpi) };
        HGDIOBJ of = SelectObject(dc, st.bodyFont);
        PaintAlarmBand(dc, band, st,
                       skin::Label(L"The host key for this server has CHANGED").c_str());
        SelectObject(dc, of);
        y += Px(42, dpi);
    }

    // ---- the sigil, in a well on the left
    const int fig = Px(150, dpi);
    RECT well = { pad, y, pad + fig, y + fig };
    skin::FrameWell(dc, well, dpi, false);
    RECT inner = well;
    InflateRect(&inner, -Px(12, dpi), -Px(12, dpi));
    PaintSigil(dc, inner, st.sigil, dpi, st.changed, st.pal);

    // ---- the mnemonic under it, large: the thing said out loud on a call
    HGDIOBJ oldFont = SelectObject(dc, st.headFont);
    SetTextColor(dc, st.changed && ChromeSkinned() ? SrgbRef(Chrome().danger)
                                                   : st.pal.banner);
    RECT mn = { pad, y + fig + Px(6, dpi), pad + fig, y + fig + Px(42, dpi) };
    DrawTextW(dc, st.mnemonic.c_str(), -1, &mn,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    // ---- the text column
    const int tx = pad + fig + Px(28, dpi);
    const int tw = rc.right - padR - tx;
    int ty = y;

    SelectObject(dc, st.headFont);
    SetTextColor(dc, st.pal.text);
    RECT hr = { tx, ty, tx + tw, ty + Px(34, dpi) };
    DrawTextW(dc,
              st.changed ? skin::Label(L"Verify this server again").c_str()
                         : skin::Label(L"An unrecognised server").c_str(),
              -1, &hr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ty += Px(38, dpi);

    SelectObject(dc, st.bodyFont);
    SetTextColor(dc, st.pal.textDim);
    RECT br = { tx, ty, tx + tw, ty + Px(120, dpi) };
    const std::wstring intro =
        st.changed
            ? L"A different key was presented for " + st.label +
                  L". This happens when a server is rebuilt — and it is also "
                  L"what an interception looks like. Check the fingerprint "
                  L"against a source that did not come over this connection."
            : L"AmberSSH has not seen " + st.label +
                  L" before. Accepting records this key; you will be warned if "
                  L"it ever changes.";
    DrawTextW(dc, intro.c_str(), -1, &br,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    // Measured, not assumed: the changed-key paragraph is twice as long as the
    // first-contact one, and a fixed step put the fingerprint on top of it.
    RECT mr = br;
    const int introH = DrawTextW(dc, intro.c_str(), -1, &mr,
                                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX |
                                     DT_CALCRECT);
    ty += (std::max)(Px(52, dpi), introH) + Px(14, dpi);

    // ---- the fingerprint itself, in a monospaced well
    RECT fw = { tx, ty, tx + tw, ty + Px(46, dpi) };
    skin::FrameWell(dc, fw, dpi, false);
    SelectObject(dc, st.monoFont);
    SetTextColor(dc, st.pal.text);
    RECT ft = fw;
    InflateRect(&ft, -Px(8, dpi), -Px(6, dpi));
    DrawTextW(dc, st.fingerprint.c_str(), -1, &ft,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    ty += Px(54, dpi);

    // ---- the shape described in words, for anyone who cannot see the figure
    SelectObject(dc, st.smallFont);
    SetTextColor(dc, st.pal.textDis);
    RECT dr = { tx, ty, tx + tw, ty + Px(34, dpi) };
    DrawTextW(dc, (skin::Label(L"Shape: ") + st.describe).c_str(), -1, &dr,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(dc, oldFont);
}

// --------------------------------------------------------------------- risk
void PaintRisk(HDC dc, const RECT& rc, SafetyState& st)
{
    const UINT dpi = st.dpi;
    PaintGround(dc, rc, st);
    SetBkMode(dc, TRANSPARENT);
    if (!st.report)
        return;
    const RiskReport& r = *st.report;

    const int pad = PadLeft(dpi);
    const int padR = Px(26, dpi) + (PadLeft(dpi) > Px(26, dpi) ? Px(16, dpi) : 0);
    int y = Px(26, dpi);

    RECT band = { pad, y, rc.right - padR, y + Px(32, dpi) };
    HGDIOBJ oldFont = SelectObject(dc, st.bodyFont);
    std::wstring level = skin::Label(Wide(RiskLevelName(r.level)));
    for (wchar_t& c : level)
        c = static_cast<wchar_t>(towupper(c));
    PaintAlarmBand(dc, band, st, (level + L"  \x2014  check before running").c_str());
    y += Px(44, dpi);

    // ---- the exact command, verbatim, in a well. Never reformatted: the
    // warning has to be about the thing that will actually be sent.
    RECT cw = { pad, y, rc.right - padR, y + Px(52, dpi) };
    skin::FrameWell(dc, cw, dpi, false);
    SelectObject(dc, st.monoFont);
    SetTextColor(dc, st.pal.text);
    RECT ct = cw;
    InflateRect(&ct, -Px(10, dpi), -Px(8, dpi));
    DrawTextW(dc, Wide(r.command).c_str(), -1, &ct,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    y += Px(62, dpi);

    // ---- the findings, one line each, worst first
    SelectObject(dc, st.bodyFont);
    std::vector<const RiskFinding*> sorted;
    for (const RiskFinding& f : r.findings)
        sorted.push_back(&f);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const RiskFinding* a, const RiskFinding* b)
                     { return static_cast<int>(a->level) > static_cast<int>(b->level); });
    int shown = 0;
    for (const RiskFinding* f : sorted)
    {
        if (shown >= 5)
            break;
        SetTextColor(dc, f->uncertain ? st.pal.textDim : st.pal.text);
        std::wstring line = L"\x2022  " + Wide(f->what);
        if (!f->detail.empty())
            line += L": " + Wide(f->detail);
        if (f->uncertain)
            line += L"   (unable to determine)";
        RECT lr = { pad + Px(4, dpi), y, rc.right - padR, y + Px(34, dpi) };
        DrawTextW(dc, line.c_str(), -1, &lr,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        const int used = DrawTextW(dc, line.c_str(), -1, &lr,
                                   DT_LEFT | DT_TOP | DT_WORDBREAK |
                                       DT_NOPREFIX | DT_CALCRECT);
        y += (std::max)(Px(20, dpi), used) + Px(6, dpi);
        ++shown;
    }

    // ---- what the parser did NOT read. Said plainly, because a warning that
    // hides its own limits is worse than no warning.
    if (r.incomplete)
    {
        SelectObject(dc, st.smallFont);
        SetTextColor(dc, st.pal.textDis);
        RECT ir = { pad, y + Px(4, dpi), rc.right - padR, y + Px(40, dpi) };
        DrawTextW(dc,
                  (L"AmberSSH could not read the whole command: " +
                   Wide(r.incompleteWhy) + L".")
                      .c_str(),
                  -1, &ir, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }

    // ---- the typed-confirmation prompt sits above the buttons
    if (st.style == ConfirmStyle::TypeHostname)
    {
        SelectObject(dc, st.bodyFont);
        SetTextColor(dc, st.pal.textDim);
        // Above the edit box, not on top of it: the edit is a child window and
        // would paint straight over this line.
        const int editTop = rc.bottom - PadBottom(dpi) - Px(34, dpi) - Px(38, dpi);
        RECT pr = { pad, editTop - Px(24, dpi), rc.right - padR, editTop - Px(2, dpi) };
        DrawTextW(dc, (L"Type " + st.hostname + L" to confirm:").c_str(), -1, &pr,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        // The edit control has no frame of its own, and on a dark skin an empty
        // one is invisible. The skin's well is what makes it a field.
        RECT ew = { pad - Px(4, dpi), editTop - Px(3, dpi),
                    pad + Px(264, dpi), editTop + Px(31, dpi) };
        skin::FrameWell(dc, ew, dpi, false);
    }
    SelectObject(dc, oldFont);
}

// The trusted-X11 opt-in. Same ground, band, wells and typed confirmation as
// the risk dialog, so every skin already knows how to draw it; the words are
// the part that is new, and they say what trusted X11 actually permits.
void PaintTrustedX11(HDC dc, const RECT& rc, SafetyState& st)
{
    const UINT dpi = st.dpi;
    PaintGround(dc, rc, st);
    SetBkMode(dc, TRANSPARENT);

    const int pad = PadLeft(dpi);
    const int padR = Px(26, dpi) + (PadLeft(dpi) > Px(26, dpi) ? Px(16, dpi) : 0);
    int y = Px(26, dpi);

    RECT band = { pad, y, rc.right - padR, y + Px(32, dpi) };
    HGDIOBJ oldFont = SelectObject(dc, st.bodyFont);
    std::wstring head = skin::Label(L"Trusted X11");
    for (wchar_t& c : head)
        c = static_cast<wchar_t>(towupper(c));
    PaintAlarmBand(dc, band, st, (head + L"  \x2014  full access for every program from " + st.hostname).c_str());
    y += Px(44, dpi);

    // ---- what it means, in the terms the SECURITY extension enforces
    static const wchar_t* lines[] = {
        L"\x2022  Any forwarded program can read the keystrokes going to any other forwarded window.",
        L"\x2022  Any forwarded program can read and change any other forwarded window's contents and properties.",
        L"\x2022  Any forwarded program can grab the keyboard and pointer.",
        L"\x2022  Restricted mode \x2014 the default \x2014 forbids all of that and still runs ordinary applications.",
        L"\x2022  Every window will carry an \x201cX11 TRUSTED\x201d strip while this is on.",
    };
    SelectObject(dc, st.bodyFont);
    for (const wchar_t* l : lines)
    {
        SetTextColor(dc, st.pal.text);
        RECT lr = { pad + Px(4, dpi), y, rc.right - padR, y + Px(40, dpi) };
        const int used = DrawTextW(dc, l, -1, &lr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, l, -1, &lr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        y += (std::max)(Px(20, dpi), used) + Px(6, dpi);
    }

    SelectObject(dc, st.smallFont);
    SetTextColor(dc, st.pal.textDim);
    RECT sr = { pad, y + Px(4, dpi), rc.right - padR, y + Px(44, dpi) };
    DrawTextW(dc,
              st.sessionOnly
                  ? L"For this session only: the choice is not saved and is gone when AmberSSH closes."
                  : L"Saved with the profile: every connection to this host will be trusted until you change it back.",
              -1, &sr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    // ---- the typed confirmation, exactly as the risk dialog lays it out
    SelectObject(dc, st.bodyFont);
    SetTextColor(dc, st.pal.textDim);
    const int editTop = rc.bottom - PadBottom(dpi) - Px(34, dpi) - Px(38, dpi);
    RECT pr = { pad, editTop - Px(24, dpi), rc.right - padR, editTop - Px(2, dpi) };
    DrawTextW(dc, (L"Type " + st.hostname + L" to confirm:").c_str(), -1, &pr,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    RECT ew = { pad - Px(4, dpi), editTop - Px(3, dpi), pad + Px(264, dpi), editTop + Px(31, dpi) };
    skin::FrameWell(dc, ew, dpi, false);
    SelectObject(dc, oldFont);
}

void Finish(HDC dc, const RECT& rc)
{
    if (!ChromeSkinned())
        return;
    finish::Pass p(dc, rc);
    if (!p.ok())
        return;
    const ChromeSpec& ch = Chrome();
    if (skin::Rehaut())
        p.Radial(rc, RGB(255, 255, 255), 0.06f, RGB(0, 0, 0), 0.18f);
    else if (skin::Meter())
        p.Gradient(rc, RGB(255, 255, 255), 0.16f, RGB(0, 0, 0), 0.08f);
    else if (skin::Glaze())
        p.Radial(rc, RGB(255, 255, 255), 0.04f, RGB(0, 0, 0), 0.45f);
    else if (skin::Stitch())
        p.Radial(rc, RGB(255, 255, 255), 0.05f, RGB(0, 0, 0), 0.32f);
    else if (skin::Ornament())
        p.Radial(rc, RGB(0xFF, 0xD8, 0x9A), 0.07f, RGB(0, 0, 0), 0.26f);
    else if (skin::Impression())
        p.Radial(rc, RGB(255, 255, 255), 0.0f, RGB(0x6E, 0x1B, 0x2A), 0.05f);
    else if (ch.glow)
        p.Glow(rc.right * 0.5f, rc.bottom * 0.5f, static_cast<float>(rc.right),
               SrgbRef(ch.neonA), 0.08f);
}

void Paint(HWND hwnd, SafetyState& st)
{
    PAINTSTRUCT ps;
    HDC front = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(front);
    HBITMAP bmp = CreateCompatibleBitmap(front, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    if (st.kind == Kind::HostKey)
        PaintHostKey(dc, rc, st);
    else if (st.kind == Kind::TrustedX11)
        PaintTrustedX11(dc, rc, st);
    else
        PaintRisk(dc, rc, st);
    Finish(dc, rc);

    BitBlt(front, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// The typed confirmation is only satisfied by an exact match, trimmed. A
// prefix or a near-miss is not a yes.
bool TypedOk(const SafetyState& st)
{
    if (st.style != ConfirmStyle::TypeHostname)
        return true;
    if (!st.edit)
        return false;
    wchar_t buf[256] = L"";
    GetWindowTextW(st.edit, buf, 256);
    std::wstring got = buf;
    while (!got.empty() && iswspace(got.front()))
        got.erase(got.begin());
    while (!got.empty() && iswspace(got.back()))
        got.pop_back();
    return !got.empty() && got == st.hostname;
}

LRESULT CALLBACK SafetyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<SafetyState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (st)
            Paint(hwnd, *st);
        return 0;

    case WM_CTLCOLOREDIT:
        if (st)
        {
            SetTextColor(reinterpret_cast<HDC>(wp), st->pal.text);
            SetBkColor(reinterpret_cast<HDC>(wp), st->pal.field);
            if (!st->ground)
                st->ground = CreateSolidBrush(st->pal.field);
            return reinterpret_cast<LRESULT>(st->ground);
        }
        break;

    case WM_DRAWITEM:
        if (st)
        {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            HBRUSH bg = CreateSolidBrush(st->pal.bg);
            const bool accent = dis->CtlID == IdNo;   // the safe answer leads
            const bool danger = dis->CtlID == IdYes;
            skin::DrawButton(*dis, st->btnFont, st->dpi, bg,
                             accent, danger, st->hover == static_cast<int>(dis->CtlID));
            DeleteObject(bg);
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        if (st)
        {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            HWND under = ChildWindowFromPoint(hwnd, p);
            const int id = under && under != hwnd
                               ? static_cast<int>(GetWindowLongPtrW(under, GWLP_ID))
                               : 0;
            if (id != st->hover)
            {
                st->hover = id;
                if (st->yes) InvalidateRect(st->yes, nullptr, TRUE);
                if (st->no) InvalidateRect(st->no, nullptr, TRUE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (st && st->hover)
        {
            st->hover = 0;
            if (st->yes) InvalidateRect(st->yes, nullptr, TRUE);
            if (st->no) InvalidateRect(st->no, nullptr, TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (st)
        {
            const int id = LOWORD(wp);
            if (id == IdConfirmEdit && HIWORD(wp) == EN_CHANGE)
            {
                if (st->yes)
                    EnableWindow(st->yes, TypedOk(*st));
                return 0;
            }
            if (id == IdYes)
            {
                if (!TypedOk(*st))
                    return 0;
                st->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IdNo)
            {
                st->accepted = false;
                DestroyWindow(hwnd);
                return 0;
            }
        }
        break;

    case WM_KEYDOWN:
        // Esc is No. There is deliberately no Enter-is-Yes: a decision that
        // can be made by a keystroke already in flight is not a decision.
        if (wp == VK_ESCAPE)
        {
            if (st)
                st->accepted = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (st)
            st->accepted = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureClass()
{
    static bool registered = false;
    if (registered)
        return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = SafetyProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (RegisterClassExW(&wc))
        registered = true;
}

// Runs the modal loop and returns what the user chose.
bool RunModal(HWND owner, SafetyState& st, int clientW, int clientH,
              const wchar_t* title, const wchar_t* yesText,
              const wchar_t* noText)
{
    EnsureClass();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* face = UiFace();
    st.headFont = MakeFont(st.dpi, 21, FW_SEMIBOLD, face);
    st.bodyFont = MakeFont(st.dpi, 15, FW_NORMAL, face);
    st.smallFont = MakeFont(st.dpi, 13, FW_NORMAL, face);
    st.btnFont = MakeFont(st.dpi, 15, FW_SEMIBOLD, face);
    st.monoFont = MakeFont(st.dpi, 14, FW_NORMAL, L"Consolas");

    RECT wr = { 0, 0, Px(clientW, st.dpi), Px(clientH, st.dpi) };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, st.dpi);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    RECT ow{};
    if (owner)
        GetWindowRect(owner, &ow);
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ow, 0);
    const int x = ow.left + ((ow.right - ow.left) - w) / 2;
    const int y = ow.top + ((ow.bottom - ow.top) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title, style,
                                x, y, w, h, owner, nullptr, inst, nullptr);
    if (!hwnd)
        return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    ApplyWindowChrome(hwnd);

    RECT cr;
    GetClientRect(hwnd, &cr);
    // Wide enough for the longest label in capitals: several skins letter
    // their chrome in small caps, which is noticeably wider than the mixed
    // case the width was first eyeballed against.
    const int bw = Px(176, st.dpi), bh = Px(34, st.dpi);
    const int bm = PadBottom(st.dpi);
    const int bmR = Px(24, st.dpi) + (PadLeft(st.dpi) > Px(26, st.dpi) ? Px(16, st.dpi) : 0);
    const int by = cr.bottom - bm - bh;
    st.no = CreateWindowExW(0, L"BUTTON", noText,
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            cr.right - bmR - bw, by, bw, bh, hwnd,
                            reinterpret_cast<HMENU>(IdNo), inst, nullptr);
    st.yes = CreateWindowExW(0, L"BUTTON", yesText,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             cr.right - bmR - Px(24, st.dpi) - bw * 2, by, bw, bh, hwnd,
                             reinterpret_cast<HMENU>(IdYes), inst, nullptr);
    if (st.style == ConfirmStyle::TypeHostname)
    {
        st.edit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  PadLeft(st.dpi), by - Px(38, st.dpi),
                                  Px(260, st.dpi), Px(28, st.dpi), hwnd,
                                  reinterpret_cast<HMENU>(IdConfirmEdit), inst,
                                  nullptr);
        SendMessageW(st.edit, WM_SETFONT, reinterpret_cast<WPARAM>(st.monoFont), TRUE);
        // Disabled until the hostname matches. The button cannot be reached
        // by muscle memory alone.
        EnableWindow(st.yes, FALSE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    // Focus lands on the safe button, never the destructive one.
    SetFocus(st.no);
    if (owner)
        EnableWindow(owner, FALSE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            st.accepted = false;
            DestroyWindow(hwnd);
            continue;
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
        SetActiveWindow(owner);
    }

    DeleteObject(st.headFont);
    DeleteObject(st.bodyFont);
    DeleteObject(st.smallFont);
    DeleteObject(st.btnFont);
    DeleteObject(st.monoFont);
    if (st.ground)
        DeleteObject(st.ground);
    return st.accepted;
}

} // namespace

void DrawSigil(HDC dc, const RECT& rc, const Sigil& s, UINT dpi, bool alarm)
{
    PaintSigil(dc, rc, s, dpi, alarm, MakeDialogPalette());
}

HostKeyChoice ShowHostKeyDialog(HWND owner, const std::string& label,
                                const std::string& fingerprint, bool changed)
{
    SafetyState st;
    st.kind = Kind::HostKey;
    st.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    st.pal = MakeDialogPalette();
    st.sigil = MakeSigil(fingerprint);
    st.label = Wide(label);
    st.fingerprint = Wide(fingerprint);
    st.mnemonic = Wide(SigilMnemonic(st.sigil));
    st.describe = Wide(SigilDescribe(st.sigil));
    st.changed = changed;
    st.style = ConfirmStyle::YesNo;

    int h = changed ? 420 : 330;
    if (ChromeSkinned() && Chrome().elbow)
        h += 26;               // room for the LCARS foot bar
    const bool ok = RunModal(owner, st, 700, h,
                             L"AmberSSH \x2014 verify host key",
                             changed ? L"Accept new key" : L"Accept key",
                             L"Do not connect");
    return ok ? HostKeyChoice::Accept : HostKeyChoice::Reject;
}

bool ShowTrustedX11Dialog(HWND owner, const std::string& hostname, bool sessionOnly)
{
    SafetyState st;
    st.kind = Kind::TrustedX11;
    st.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    st.pal = MakeDialogPalette();
    st.style = ConfirmStyle::TypeHostname;   // always: there is no low-friction trusted
    st.hostname = Wide(hostname.empty() ? std::string("trusted") : hostname);
    st.sessionOnly = sessionOnly;
    int h = 150 + 5 * 28 + 44 + 78 + 34 + 24;
    if (ChromeSkinned() && Chrome().elbow)
        h += 22;
    return RunModal(owner, st, 660, h, L"AmberSSH \x2014 trusted X11",
                    L"Enable trusted X11", L"Keep restricted");
}

bool ShowRiskDialog(HWND owner, const RiskReport& report, ConfirmStyle style,
                    const std::string& hostname)
{
    if (style == ConfirmStyle::None || style == ConfirmStyle::Notice)
        return true;
    SafetyState st;
    st.kind = Kind::Risk;
    st.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    st.pal = MakeDialogPalette();
    st.report = &report;
    st.style = style;
    st.hostname = Wide(hostname.empty() ? std::string("confirm") : hostname);

    // Sized to the content: the band, the command well, one line per finding,
    // and whatever the confirmation needs. A box with a lake of empty space in
    // it reads as an ordinary dialog, and this one should not.
    int h = 150;
    h += static_cast<int>((std::min)(report.findings.size(), size_t(5))) * 28;
    if (report.incomplete)
        h += 38;
    if (style == ConfirmStyle::TypeHostname)
        h += 78;
    h += 34 + 24;                        // the button row and its margin
    if (ChromeSkinned() && Chrome().elbow)
        h += 22;                         // LCARS puts a foot bar under it
    return RunModal(owner, st, 660, h, L"AmberSSH \x2014 confirm command",
                    L"Run it", L"Cancel");
}

} // namespace amber
