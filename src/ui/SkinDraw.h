// SkinDraw.h — the shared drawing kit every Win32 surface uses to obey the
// active interface skin.
//
// The rule this exists to serve: a new feature is not finished until it looks
// deliberate in EVERY skin. Doing that by hand in each window is how the
// black-on-black buttons and the invisible-on-paper text happened, so the
// decisions that are easy to get wrong live here once:
//
//   * ink chosen by luma, never assumed black or white
//   * the skin's shape language (pill / chamfer / hairline / square)
//   * the skin's typeface and letter case, measured as it will be DRAWN
//   * light grounds, which need different borders and no additive tricks
//
// Header-only and inline so any window can use it without a link dependency.
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "Chrome.h"
#include "Theme.h"

#pragma comment(lib, "msimg32.lib")   // GradientFill, for the foil

namespace amber
{
namespace skin
{

inline bool Skinned() { return ChromeSkinned(); }
inline bool Pills() { return Skinned() && Chrome().pills; }
inline bool Hairline() { return Skinned() && Chrome().outline > 0.0f; }
inline bool UpCase() { return Skinned() && Chrome().uppercase; }
inline bool LightGround() { return Skinned() && Chrome().lightGround; }

inline int Px(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Scale a COLORREF towards black (f < 1) or towards white (f > 1).
inline COLORREF Dim(COLORREF c, float f)
{
    auto ch = [&](int v) { return (std::min)(255, static_cast<int>(v * f + 0.5f)); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

inline COLORREF Ref(uint32_t srgb) { return SrgbRef(srgb); }

// Black or white, whichever reads on this ground. The single most common
// source of unreadable controls is guessing this.
inline COLORREF InkOnRef(COLORREF ground)
{
    const uint32_t packed = (static_cast<uint32_t>(GetRValue(ground)) << 16) |
                            (static_cast<uint32_t>(GetGValue(ground)) << 8) |
                            GetBValue(ground);
    return InkOn(packed);
}

// The face this skin letters in.
inline const wchar_t* UiFace()
{
    if (Skinned() && ChromeFace())
        return ChromeFace();
    return L"Segoe UI";
}

inline HFONT MakeUiFont(UINT dpi, int px, int weight = FW_NORMAL)
{
    return CreateFontW(-Px(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, UiFace());
}

// Label text in the skin's case.
inline std::wstring Label(std::wstring s)
{
    if (UpCase())
        for (wchar_t& c : s)
            c = static_cast<wchar_t>(::towupper(c));
    return s;
}

// Width of a label as it will actually be drawn — uppercase is wider than the
// source string, and measuring the source clips the result.
inline int LabelWidth(HDC dc, const std::wstring& s)
{
    std::wstring shown = Label(s);
    SIZE sz{};
    GetTextExtentPoint32W(dc, shown.c_str(), static_cast<int>(shown.size()), &sz);
    return sz.cx;
}

// The skin's silhouette for a panel, button or well. The caller selects the
// pen and brush; this only decides the outline.
inline void Shape(HDC dc, const RECT& rc, int cut, int radius, UINT dpi)
{
    if (Hairline())
    {
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        return;
    }
    if (Pills())
    {
        int r = (std::min)(static_cast<int>(rc.bottom - rc.top), Px(26, dpi));
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
        return;
    }
    if (cut > 0)
    {
        POINT p[6] = {
            { rc.left + cut, rc.top },          { rc.right - 1, rc.top },
            { rc.right - 1, rc.bottom - 1 - cut }, { rc.right - 1 - cut, rc.bottom - 1 },
            { rc.left, rc.bottom - 1 },         { rc.left, rc.top + cut },
        };
        Polygon(dc, p, 6);
        return;
    }
    if (radius > 0)
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    else
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
}

// Corner ticks, the HUD detail Cyberpunk and Blueprint use on panels.
inline void Ticks(HDC dc, const RECT& r, int len, COLORREF col)
{
    HBRUSH b = CreateSolidBrush(col);
    RECT t;
    auto fill = [&](int x, int y, int w, int h) {
        t = { x, y, x + w, y + h };
        FillRect(dc, &t, b);
    };
    fill(r.left - 2, r.top - 2, len, 1);           fill(r.left - 2, r.top - 2, 1, len);
    fill(r.right + 1 - len, r.top - 2, len, 1);    fill(r.right + 1, r.top - 2, 1, len);
    fill(r.left - 2, r.bottom + 1, len, 1);        fill(r.left - 2, r.bottom + 1 - len, 1, len);
    fill(r.right + 1 - len, r.bottom + 1, len, 1); fill(r.right + 1, r.bottom + 1 - len, 1, len);
    DeleteObject(b);
}

// Diagonal hazard striping, for the skins that mark destructive controls.
inline void HazardFill(HDC dc, const RECT& rc, COLORREF warn, UINT dpi)
{
    HBRUSH wb = CreateSolidBrush(warn);
    FillRect(dc, &rc, wb);
    DeleteObject(wb);
    const int h = rc.bottom - rc.top;
    HBRUSH bb = CreateSolidBrush(RGB(0, 0, 0));
    HGDIOBJ ob = SelectObject(dc, bb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HRGN clip = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
    SelectClipRgn(dc, clip);
    for (int x = rc.left - h; x < rc.right; x += Px(12, dpi))
    {
        POINT p[4] = { { x, rc.bottom }, { x + h, rc.top },
                       { x + h + Px(5, dpi), rc.top }, { x + Px(5, dpi), rc.bottom } };
        Polygon(dc, p, 4);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(bb);
}

// A push button in the active skin. `accent` marks the primary action;
// `danger` gets hazard striping on the skins that use it.
inline void DrawButton(const DRAWITEMSTRUCT& dis, HFONT font, UINT dpi,
                       HBRUSH groundBrush, bool accent, bool danger, bool hover)
{
    const DialogPalette pal = MakeDialogPalette();
    const ChromeSpec& ch = Chrome();
    HDC dc = dis.hDC;
    RECT rc = dis.rcItem;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;

    FillRect(dc, &rc, groundBrush);

    wchar_t raw[256] = L"";
    GetWindowTextW(dis.hwndItem, raw, 256);
    const std::wstring text = Label(raw);

    if (danger && Skinned() && ch.hazard && !disabled)
    {
        RECT hz = rc;
        InflateRect(&hz, -Px(1, dpi), -Px(1, dpi));
        const COLORREF warn = Ref(ch.neonB);
        HazardFill(dc, hz, warn, dpi);
        // A clear plate through the middle: striping behind lettering cannot
        // be read.
        HBRUSH plate = CreateSolidBrush(warn);
        RECT band = { hz.left, (hz.top + hz.bottom) / 2 - Px(9, dpi), hz.right,
                      (hz.top + hz.bottom) / 2 + Px(9, dpi) };
        FillRect(dc, &band, plate);
        DeleteObject(plate);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        HGDIOBJ of = SelectObject(dc, font);
        DrawTextW(dc, text.c_str(), -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        return;
    }

    COLORREF fill = accent ? pal.accent : pal.field;
    COLORREF line = (hover || accent) ? pal.borderHot : pal.border;
    COLORREF txt = disabled ? pal.textDis : (accent ? pal.accentText : pal.text);
    if (Skinned() && ch.darkText)
    {
        fill = accent ? (hover ? Ref(ch.neonB) : pal.accent)
                      : (hover ? Ref(ch.bars[1]) : Ref(ch.bars[0]));
        line = fill;
        txt = InkOnRef(fill);
    }
    else if (hover && !accent)
        fill = LightGround() ? Dim(pal.field, 0.94f) : Dim(pal.field, 1.35f);
    if (Hairline())
    {
        fill = pal.bg;                       // Blueprint fills nothing
        line = disabled ? pal.textDis : (accent || hover) ? Ref(ch.neonA)
                                                          : Ref(ch.neonB);
        txt = line;
    }
    if (pressed)
        fill = Dim(fill, LightGround() ? 0.90f : 1.25f);
    if (disabled)
        fill = Dim(fill, LightGround() ? 1.04f : 0.55f);

    const int cut = Skinned() ? Px(static_cast<int>(ch.chamfer), dpi) : 0;
    const int pen = Hairline()
                        ? (std::max)(1, Px(static_cast<int>(ch.outline), dpi))
                        : 1;
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN pn = CreatePen(PS_SOLID, pen, line);
    HGDIOBJ ob = SelectObject(dc, fb);
    HGDIOBJ op = SelectObject(dc, pn);
    Shape(dc, rc, cut, Px(6, dpi), dpi);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fb);
    DeleteObject(pn);
    if (Skinned() && ch.hudBrackets && hover)
        Ticks(dc, rc, Px(9, dpi), pal.borderHot);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt);
    HGDIOBJ of = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

// A field well or list pane: the outline the skin would draw around content.
inline void FrameWell(HDC dc, const RECT& rc, UINT dpi, bool focused)
{
    const DialogPalette pal = MakeDialogPalette();
    const ChromeSpec& ch = Chrome();
    const int pen = Hairline()
                        ? (std::max)(1, Px(static_cast<int>(ch.outline), dpi))
                        : 1;
    HPEN pn = CreatePen(PS_SOLID, pen, focused ? pal.borderHot : pal.border);
    HGDIOBJ op = SelectObject(dc, pn);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Shape(dc, rc, Skinned() ? Px(static_cast<int>(ch.chamfer), dpi) : 0,
          Px(4, dpi), dpi);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pn);
    if (Skinned() && ch.hudBrackets)
        Ticks(dc, rc, Px(12, dpi), pal.borderHot);
}

// The Explorer visual style to hand a list view. Getting this wrong is what
// leaves white-on-white rows on a light skin.
inline const wchar_t* ListTheme()
{
    return LightGround() ? L"Explorer" : L"DarkMode_Explorer";
}
inline const wchar_t* HeaderTheme()
{
    return LightGround() ? L"ItemsView" : L"DarkMode_ItemsView";
}

// Faint scanlines across a panel, for the skins that use them.
// ------------------------------------------------------------------- LCARS
// The Okudagram frame, drawn to rule (see the Figma LCARS Reference board):
// a top bar of height H turning down into a side bar of width V through an
// OUTER radius of H + V and an INNER radius of H, with a constant gutter
// between every segment that never scales with the block it separates.
//
// One closed figure, so the two curves meet the straight runs exactly rather
// than being carved back out of a rounded rectangle in the ground colour.
inline int Elbow(HDC dc, int x, int top, int right, int H, int V, int legLen,
                 COLORREF c)
{
    const int Ro = H + V, Ri = H;
    std::vector<POINT> p;
    auto arc = [&](double cx, double cy, double r, double a0, double a1) {
        const int steps = 16;
        for (int i = 0; i <= steps; ++i)
        {
            const double a = (a0 + (a1 - a0) * i / steps) * 3.14159265358979 / 180.0;
            p.push_back({ static_cast<LONG>(cx + std::cos(a) * r + 0.5),
                          static_cast<LONG>(cy + std::sin(a) * r + 0.5) });
        }
    };
    arc(x + Ro, top + Ro, Ro, 180.0, 270.0);              // outer corner
    p.push_back({ right, top });
    p.push_back({ right, top + H });
    arc(x + V + Ri, top + H + Ri, Ri, 270.0, 180.0);      // inner corner
    p.push_back({ x + V, top + legLen });
    p.push_back({ x, top + legLen });

    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p.data(), static_cast<int>(p.size()));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    return top + legLen;
}

// The block column that continues a leg on the same width and gutter. Heights
// are the irregular LCARS rhythm rather than a uniform stack, which is what
// stops the column reading as a list.
inline void BlockColumn(HDC dc, int x, int y, int V, int bottom, int gutter,
                        const uint32_t* bars, int first, UINT dpi)
{
    const int heights[5] = { 70, 34, 110, 48, 90 };
    int blk = 0;
    while (y < bottom)
    {
        const int h = (std::min)(Px(heights[blk % 5], dpi), bottom - y);
        if (h <= 0)
            break;
        HBRUSH cb = CreateSolidBrush(SrgbRef(bars[(blk + first) % 4]));
        RECT r = { x, y, x + V, y + h };
        FillRect(dc, &r, cb);
        DeleteObject(cb);
        y += h + gutter;
        ++blk;
    }
}

// The end cap: half-round on the OUTER end only, radius H/2, held off the bar
// it terminates by one gutter. The gutter is punched back to the ground.
inline void EndCap(HDC dc, int right, int top, int w, int H, int gutter,
                   COLORREF c, COLORREF ground)
{
    RECT cut = { right - w - gutter, top, right, top + H };
    HBRUSH gb = CreateSolidBrush(ground);
    FillRect(dc, &cut, gb);
    DeleteObject(gb);
    HBRUSH cb = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, cb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, right - w, top, right, top + H, H, H);
    RECT sq = { right - w, top, right - w + H, top + H };
    FillRect(dc, &sq, cb);        // square off the inner end
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(cb);
}

// A geometric pen with round ends, so a drawn run — a copper trace, a length
// of brass rod — reads as drawn rather than as a rectangle with square ends.
inline HPEN RoundPen(int width, COLORREF c)
{
    LOGBRUSH lb;
    lb.lbStyle = BS_SOLID;
    lb.lbColor = c;
    lb.lbHatch = 0;
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                        static_cast<DWORD>((std::max)(1, width)), &lb, 0, nullptr);
}

// ------------------------------------------------------------------- rehaut
// Haute horlogerie, from the Figma Horologe board. The rehaut is the minute
// track on the inner rim of a dial: a tick every T, a long tick every fifth,
// a numeral every fifteenth. It runs along every long edge of the chrome.
// Controls are APPLIED INDICES — raised by a rhodium line above and a shade
// below, never filled — and the header band is engine-turned.

inline bool Rehaut() { return Skinned() && Chrome().rehaut; }

// Tick pitch, fixed in logical px like the rail repeat: a track keeps its
// rhythm whatever it happens to be measuring.
inline int TickPitch(UINT dpi) { return (std::max)(3, Px(6, dpi)); }

// The minute track: baseline at y across w, ticks rising from it when `up`,
// hanging from it otherwise. `numerals` is the small face for the numbers;
// nullptr skips them where there is no room.
inline void MinuteTrack(HDC dc, int x, int y, int w, UINT dpi,
                        const DialogPalette& pal, HFONT numerals, bool up)
{
    const int T = TickPitch(dpi);
    const int shortT = Px(4, dpi), longT = Px(8, dpi);
    HBRUSH dimB = CreateSolidBrush(pal.textDim);
    HBRUSH hotB = CreateSolidBrush(pal.text);
    HBRUSH shade = CreateSolidBrush(Dim(pal.bg, 0.45f));
    RECT base = { x, y, x + w, y + 1 };
    FillRect(dc, &base, shade);
    HGDIOBJ of = numerals ? SelectObject(dc, numerals) : nullptr;
    const int obk = SetBkMode(dc, TRANSPARENT);
    const COLORREF oc = SetTextColor(dc, pal.textDim);
    for (int i = 0; i * T <= w; ++i)
    {
        const bool lng = (i % 5) == 0;
        const int h = lng ? longT : shortT;
        const int tx = x + i * T;
        RECT t = up ? RECT{ tx, y - h, tx + 1, y }
                    : RECT{ tx, y + 1, tx + 1, y + 1 + h };
        FillRect(dc, &t, lng ? hotB : dimB);
        if (numerals && i > 0 && (i % 15) == 0 && i * T + Px(12, dpi) <= w)
        {
            wchar_t n[8];
            swprintf(n, 8, L"%d", (i / 15) * 5);
            const int nh = Px(13, dpi), nw = Px(12, dpi);
            RECT nr = up ? RECT{ tx - nw, y - longT - nh, tx + nw, y - longT }
                         : RECT{ tx - nw, y + 1 + longT, tx + nw, y + 1 + longT + nh };
            DrawTextW(dc, n, -1, &nr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    SetTextColor(dc, oc);
    SetBkMode(dc, obk);
    if (of)
        SelectObject(dc, of);
    DeleteObject(dimB);
    DeleteObject(hotB);
    DeleteObject(shade);
}

// Clous de Paris: two hairline grids at ±45°, six percent over the ground.
// The x-step is 6 so the perpendicular pitch is ~4, the board's figure.
// Header band and About panel only — never under a label, never in a field.
inline void Guilloche(HDC dc, const RECT& rc, UINT dpi, COLORREF ground)
{
    const int step = (std::max)(4, Px(6, dpi));
    const COLORREF line = RGB((std::min)(255, GetRValue(ground) + 15),
                              (std::min)(255, GetGValue(ground) + 15),
                              (std::min)(255, GetBValue(ground) + 15));
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ op = SelectObject(dc, pen);
    // Each diagonal is clipped to the rect arithmetically rather than with a
    // clip region: this runs inside another surface's paint, and leaving any
    // DC state behind — a clip, a saved level — is that surface's bug to find.
    const int W = rc.right - rc.left, Hh = rc.bottom - rc.top;
    for (int k = -Hh; k <= W + Hh; k += step)
    {
        // "\" : x = left + k + (y - top)
        int y0 = (std::max)(rc.top, rc.top - k);
        int y1 = (std::min)(rc.bottom, rc.top + (W - k));
        if (y0 < y1)
        {
            MoveToEx(dc, rc.left + k + (y0 - rc.top), y0, nullptr);
            LineTo(dc, rc.left + k + (y1 - rc.top), y1);
        }
        // "/" : x = left + k - (y - top)
        y0 = (std::max)(rc.top, rc.top + (k - W));
        y1 = (std::min)(rc.bottom, rc.top + k);
        if (y0 < y1)
        {
            MoveToEx(dc, rc.left + k - (y0 - rc.top), y0, nullptr);
            LineTo(dc, rc.left + k - (y1 - rc.top), y1);
        }
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

// An applied index: RAISED, never filled. A rhodium line along the top edge
// and a shade along the bottom, on whatever the piece is already painted.
inline void AppliedIndex(HDC dc, const RECT& rc, COLORREF light, COLORREF shade)
{
    RECT t = { rc.left, rc.top, rc.right, rc.top + 1 };
    RECT b = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
    HBRUSH lb = CreateSolidBrush(light), sb = CreateSolidBrush(shade);
    FillRect(dc, &t, lb);
    FillRect(dc, &b, sb);
    DeleteObject(lb);
    DeleteObject(sb);
}

// The blued screw: a disc with a slot. Exactly one per surface at rest.
inline void BluedScrew(HDC dc, int cx, int cy, int r, COLORREF blue, COLORREF slot)
{
    HBRUSH b = CreateSolidBrush(blue);
    HPEN p = CreatePen(PS_SOLID, 1, Dim(blue, 0.6f));
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    DeleteObject(p);
    const int hw = (std::max)(1, r / 4), in = (r * 2) / 5;
    RECT s = { cx - r + in, cy - hw, cx + r - in + 1, cy + hw };
    HBRUSH sb = CreateSolidBrush(slot);
    FillRect(dc, &s, sb);
    DeleteObject(sb);
}

// ------------------------------------------------------------------- traces
// Copper routing, from the Figma Solder Mask board. The whole grammar is that
// a trace NEVER turns a right angle: every bend is a 45 degree mitre, runs at
// least 2W before bending, and terminates in a via. A right angle here reads
// as wrong the way a proportional font reads as wrong in a terminal.

inline bool Traces() { return Skinned() && Chrome().traces; }

// Trace width for this DPI. Everything else in the kit is derived from it, so
// a board drawn at one width stays in proportion at any scale.
inline int TraceW(UINT dpi) { return (std::max)(2, Px(4, dpi)); }

// Bare copper. bars[3] is the board's copper slot — the dialog palette has no
// copper of its own, because no other skin needs one.
inline COLORREF Copper() { return SrgbRef(Chrome().bars[3]); }

// A run from (x, y) to (x + dx, y + dy) routed the way a board is: straight,
// then a single 45 degree mitre, then straight. `dy` is usually small — this
// is trim, not a schematic.
inline void Trace(HDC dc, int x, int y, int dx, int dy, int w, COLORREF c)
{
    const int drop = (dy < 0 ? -dy : dy);
    const int run = (dx < 0 ? -dx : dx);
    const int sx = (dx < 0 ? -1 : 1);
    // The mitre eats `drop` of the run, and it needs 2W of straight either
    // side; if the run cannot afford that, the trace stays flat rather than
    // bending too tightly, which is what a router would do.
    const int lead = (std::max)(2 * w, (run - drop) / 2);
    POINT p[4];
    p[0] = { x, y };
    if (run - drop >= 4 * w)
    {
        p[1] = { x + sx * lead, y };
        p[2] = { x + sx * (lead + drop), y + dy };
        p[3] = { x + dx, y + dy };
    }
    else
    {
        p[1] = p[0];
        p[2] = { x + dx, y };
        p[3] = { x + dx, y };
    }
    HPEN pen = RoundPen(w, c);
    HGDIOBJ op = SelectObject(dc, pen);
    Polyline(dc, p, 4);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A via: an annular ring of 2.2W with a 0.9W bore punched back to the mask.
inline void Via(HDC dc, int x, int y, int w, COLORREF ring, COLORREF ground)
{
    const int r = (std::max)(2, (w * 11) / 10);
    const int b = (std::max)(1, (w * 9) / 20);
    HBRUSH rb = CreateSolidBrush(ring);
    HBRUSH gb = CreateSolidBrush(ground);
    HGDIOBJ ob = SelectObject(dc, rb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - r, y - r, x + r, y + r);
    SelectObject(dc, gb);
    Ellipse(dc, x - b, y - b, x + b, y + b);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(rb);
    DeleteObject(gb);
}

// A silkscreen reference designator, set small and dim. The PCB dialect of the
// LCARS numeric tag: it fills the empty end of a control with something the
// board would actually have printed there.
inline void Designator(HDC dc, const RECT& r, const wchar_t* ref, HFONT f,
                       COLORREF c)
{
    HGDIOBJ of = SelectObject(dc, f);
    const int obk = SetBkMode(dc, TRANSPARENT);
    const COLORREF oc = SetTextColor(dc, c);
    RECT t = r;
    DrawTextW(dc, ref, -1, &t,
              DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, oc);
    SetBkMode(dc, obk);
    SelectObject(dc, of);
}

// The divider a routed board uses in place of a rule: a copper bus dropping
// one step, with a gold via at each end.
inline void TraceBus(HDC dc, int x, int y, int w, UINT dpi,
                     const DialogPalette& pal, COLORREF ground)
{
    const int tw = TraceW(dpi);
    const int drop = Px(9, dpi);
    Trace(dc, x, y, w, drop, tw, Copper());
    Trace(dc, x, y - Px(7, dpi), (w * 3) / 4, drop, tw, pal.borderHot);
    Via(dc, x, y, tw, pal.accent, ground);
    Via(dc, x + w, y + drop, tw, pal.accent, ground);
    Via(dc, x + (w * 3) / 4, y - Px(7, dpi) + drop, tw, pal.selText, ground);
}

// ---------------------------------------------------------------- ornament
// Brass filigree, gears and rails, ported from the Figma ornament kit as GDI
// geometry rather than shipped as bitmaps: it scales with DPI, recolours from
// the palette, and costs no assets.
//
// Ornament is drawn ONLY on chrome — dialog grounds and the window strips.
// Nothing here may be drawn over the terminal grid; the whole point of the
// terminal is that the text is the picture.

inline bool Ornament() { return Skinned() && Chrome().ornament; }


// An Archimedean volute, sampled as a polyline: r = a + b*theta.
inline void Volute(HDC dc, double cx, double cy, double a, double b,
                   double turns, int dir, double from, int width, COLORREF c)
{
    const int steps = (std::max)(8, static_cast<int>(turns * 22));
    std::vector<POINT> pts;
    pts.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i)
    {
        const double th = (static_cast<double>(i) / steps) * turns * 6.28318530718;
        const double r = a + b * th;
        const double ang = from + dir * th;
        pts.push_back({ static_cast<LONG>(cx + std::cos(ang) * r + 0.5),
                        static_cast<LONG>(cy + std::sin(ang) * r + 0.5) });
    }
    HPEN pen = RoundPen(width, c);
    HGDIOBJ op = SelectObject(dc, pen);
    Polyline(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A spur gear: trapezoidal teeth around a hub, with the bore punched back to
// the ground colour. Flat grounds only, which is every place it is used.
inline void Gear(HDC dc, int cx, int cy, int R, int teeth, COLORREF face,
                 COLORREF edge, COLORREF ground)
{
    const double root = R * 0.74;
    const double tip = (3.14159265358979 / teeth) * 0.55;
    const double rt  = (3.14159265358979 / teeth) * 0.95;
    std::vector<POINT> pts;
    pts.reserve(static_cast<size_t>(teeth) * 4);
    for (int i = 0; i < teeth; ++i)
    {
        const double a = (i * 6.28318530718) / teeth;
        const double ang[4] = { a - rt, a - tip, a + tip, a + rt };
        const double rad[4] = { root, static_cast<double>(R),
                                static_cast<double>(R), root };
        for (int k = 0; k < 4; ++k)
            pts.push_back({ static_cast<LONG>(cx + std::cos(ang[k]) * rad[k] + 0.5),
                            static_cast<LONG>(cy + std::sin(ang[k]) * rad[k] + 0.5) });
    }
    HBRUSH fb = CreateSolidBrush(face);
    HPEN pn = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, fb);
    HGDIOBJ op = SelectObject(dc, pn);
    Polygon(dc, pts.data(), static_cast<int>(pts.size()));
    HBRUSH gb = CreateSolidBrush(ground);
    SelectObject(dc, gb);
    const int bore = (std::max)(2, R / 4);
    Ellipse(dc, cx - bore, cy - bore, cx + bore, cy + bore);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fb);
    DeleteObject(gb);
    DeleteObject(pn);
}

// One corner of the frame. `corner` is 0 TL, 1 TR, 2 BR, 3 BL; the canonical
// design is drawn top-left and every other orientation is those same points
// through a rotation, so all four are identical by construction.
inline void CornerFiligree(HDC dc, int x, int y, int size, int corner,
                           const DialogPalette& pal, COLORREF ground)
{
    const double k = size / 210.0;         // the Figma kit was drawn at 210
    auto M = [&](double u, double v, POINT& out) {
        const double S = 210.0;
        double a = u, b = v;
        if (corner == 1) { a = S - v; b = u; }
        else if (corner == 2) { a = S - u; b = S - v; }
        else if (corner == 3) { a = v; b = S - u; }
        out.x = static_cast<LONG>(x + a * k + 0.5);
        out.y = static_cast<LONG>(y + b * k + 0.5);
    };
    auto W = [&](double w) { return (std::max)(1, static_cast<int>(w * k + 0.5)); };

    const COLORREF brass = pal.borderHot;
    const COLORREF polish = pal.accent;
    const COLORREF dark = Dim(pal.border, 0.55f);

    std::vector<POINT> bar;
    POINT p;
    M(22, 196, p); bar.push_back(p);
    M(22, 46, p);  bar.push_back(p);
    for (int i = 0; i <= 8; ++i)           // quarter-round elbow
    {
        const double t = i / 8.0 * 1.57079632679;
        M(22 + 24 * (1 - std::cos(t)), 46 - 24 * std::sin(t), p);
        bar.push_back(p);
    }
    M(196, 22, p); bar.push_back(p);

    HPEN under = RoundPen(W(13), dark);
    HGDIOBJ op = SelectObject(dc, under);
    Polyline(dc, bar.data(), static_cast<int>(bar.size()));
    SelectObject(dc, op);
    DeleteObject(under);

    HPEN top = RoundPen(W(9), brass);
    op = SelectObject(dc, top);
    Polyline(dc, bar.data(), static_cast<int>(bar.size()));
    SelectObject(dc, op);
    DeleteObject(top);

    // The scrolls curl into the panel on every corner, so the sweep direction
    // and start angle rotate with the corner rather than staying put.
    const int dir = (corner == 1 || corner == 3) ? -1 : 1;
    const double turn = (corner == 1) ? 1.5708 : (corner == 2) ? 3.1416
                      : (corner == 3) ? 4.7124 : 0.0;
    POINT v1, v2, v3;
    M(86, 62, v1); M(62, 118, v2); M(120, 104, v3);
    Volute(dc, v1.x, v1.y, 4 * k, 3.4 * k, 1.55, dir, -1.9 + turn, W(4.5), brass);
    Volute(dc, v2.x, v2.y, 4 * k, 3.2 * k, 1.45, -dir, 1.2 + turn, W(4.5), brass);
    Volute(dc, v3.x, v3.y, 3 * k, 2.4 * k, 1.25, dir, 0.4 + turn, W(2.6), polish);

    const double dots[7][3] = { {22,72,4.5},{22,150,4.5},{72,22,4.5},{150,22,4.5},
                                {86,62,3.2},{62,118,3.2},{120,104,2.4} };
    HPEN dp = CreatePen(PS_SOLID, 1, dark);
    HBRUSH db = CreateSolidBrush(polish);
    HGDIOBJ ob = SelectObject(dc, db);
    op = SelectObject(dc, dp);
    for (const auto& d : dots)
    {
        M(d[0], d[1], p);
        const int r = W(d[2]);
        Ellipse(dc, p.x - r, p.y - r, p.x + r, p.y + r);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(db);
    DeleteObject(dp);

    M(46, 46, p);
    Gear(dc, p.x, p.y, W(27), 12, brass, dark, ground);
}

// A small train of meshing gears, for the corner voids in a dialog's chrome
// where there is room for detail but not for a full filigree corner.
inline void GearCluster(HDC dc, int cx, int cy, int scale, const DialogPalette& pal,
                        COLORREF ground)
{
    const COLORREF brass = pal.borderHot;
    const COLORREF dark = Dim(pal.border, 0.55f);
    const int r0 = scale, r1 = (scale * 7) / 10, r2 = scale / 2;
    Gear(dc, cx, cy, r0, 16, brass, dark, ground);
    Gear(dc, cx + (r0 + r1) - scale / 6, cy - (r0 / 2), r1, 12, brass, dark, ground);
    Gear(dc, cx + (r0 + r1) - scale / 6 + (r1 + r2) - scale / 8,
         cy - (r0 / 2) + (r1 / 2), r2, 9, brass, dark, ground);
}

// A rail that tiles: the motif repeats every 120 design units, so butting
// copies leaves no seam and corners plus rails frame a panel at any width.
// `band` is the rail height in logical px and the whole motif scales with it,
// so a header divider can be slim where a panel foot is generous.
inline void EdgeRail(HDC dc, int x, int y, int w, UINT dpi,
                     const DialogPalette& pal, int band = 46)
{
    const double k = Px(band, dpi) / 46.0;
    const int mid = y + Px(band, dpi) / 2;
    const COLORREF brass = pal.borderHot;
    const COLORREF polish = pal.accent;
    const COLORREF dark = Dim(pal.border, 0.55f);
    auto line = [&](int width, COLORREF c, int off) {
        HPEN pen = RoundPen(width, c);
        HGDIOBJ o = SelectObject(dc, pen);
        MoveToEx(dc, x, mid + off, nullptr);
        LineTo(dc, x + w, mid + off);
        SelectObject(dc, o);
        DeleteObject(pen);
    };
    line((std::max)(1, static_cast<int>(11 * k)), dark, 0);
    line((std::max)(1, static_cast<int>(7 * k)), brass, 0);
    line(1, polish, -static_cast<int>(5 * k));

    // The repeat is fixed in logical px rather than scaled with the band: a
    // slim rail wants its studs spaced the same, not crowded together.
    const int rep = (std::max)(8, Px(120, dpi));
    HPEN dp = CreatePen(PS_SOLID, 1, dark);
    HBRUSH bb = CreateSolidBrush(brass);
    HBRUSH pb = CreateSolidBrush(polish);
    HGDIOBJ op = SelectObject(dc, dp);
    for (int o = 0; o + rep / 2 <= w; o += rep)
    {
        const int c = x + o + rep / 2;
        const int hh = static_cast<int>(14 * k), hw = static_cast<int>(15 * k);
        POINT loz[4] = { { c, mid - hh }, { c + hw, mid }, { c, mid + hh }, { c - hw, mid } };
        HGDIOBJ ob = SelectObject(dc, bb);
        Polygon(dc, loz, 4);
        SelectObject(dc, pb);
        const int r = (std::max)(2, static_cast<int>(3.6 * k));
        Ellipse(dc, c - r, mid - r, c + r, mid + r);
        SelectObject(dc, ob);
    }
    SelectObject(dc, op);
    DeleteObject(dp);
    DeleteObject(bb);
    DeleteObject(pb);
}

inline void Scanlines(HDC dc, const RECT& rc, UINT dpi)
{
    if (!Skinned() || !Chrome().scanlines)
        return;
    HBRUSH sl = CreateSolidBrush(ScaleSrgb(Chrome().neonA, 0.10f));
    for (int y = rc.top; y < rc.bottom; y += Px(4, dpi))
    {
        RECT l = { rc.left, y, rc.right, y + 1 };
        FillRect(dc, &l, sl);
    }
    DeleteObject(sl);
}

// -------------------------------------------------------------- letterpress
// A one-ink press on cotton stock (see the boutique pitch). Every shape is
// pressed INTO the paper: a shade along its top-left inner edge, a light
// along the bottom-right. An inactive control carries the impression and no
// ink at all — off is unprinted — and copper foil is reserved for the active
// piece and the primary action.

inline bool Impression() { return Skinned() && Chrome().impression; }

inline void Impress(HDC dc, const RECT& rc, COLORREF shade, COLORREF light)
{
    HBRUSH sb = CreateSolidBrush(shade), lb = CreateSolidBrush(light);
    RECT t = { rc.left, rc.top, rc.right, rc.top + 1 };
    RECT l = { rc.left, rc.top, rc.left + 1, rc.bottom };
    RECT b = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
    RECT r = { rc.right - 1, rc.top, rc.right, rc.bottom };
    FillRect(dc, &t, sb);
    FillRect(dc, &l, sb);
    FillRect(dc, &b, lb);
    FillRect(dc, &r, lb);
    DeleteObject(sb);
    DeleteObject(lb);
}

// Copper foil: a vertical metallic gradient with a darker rule beneath.
inline void Foil(HDC dc, const RECT& rc)
{
    TRIVERTEX v[2] = {
        { rc.left, rc.top, 0xE000, 0xA000, 0x7000, 0 },
        { rc.right, rc.bottom - 1, 0x8E00, 0x5A00, 0x2B00, 0 },
    };
    GRADIENT_RECT g = { 0, 1 };
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
    RECT rule = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
    HBRUSH rb = CreateSolidBrush(RGB(0x6E, 0x43, 0x20));
    FillRect(dc, &rule, rb);
    DeleteObject(rb);
}

// A printer's rule: thick, a gap, thin — always in the ink.
inline void ThickThin(HDC dc, int x, int y, int w, COLORREF ink, UINT dpi)
{
    HBRUSH b = CreateSolidBrush(ink);
    RECT thick = { x, y, x + w, y + Px(3, dpi) };
    RECT thin = { x, y + Px(5, dpi), x + w, y + Px(6, dpi) };
    FillRect(dc, &thick, b);
    FillRect(dc, &thin, b);
    DeleteObject(b);
}

// One fleuron: two mirrored volutes and a dot. One per panel head, never in
// a list.
inline void Fleuron(HDC dc, int cx, int cy, int size, COLORREF ink)
{
    const double a = size * 0.12, b = size * 0.10;
    const int w = (std::max)(1, size / 6);
    Volute(dc, cx - size * 0.55, cy, a, b, 1.3, 1, 3.1416, w, ink);
    Volute(dc, cx + size * 0.55, cy, a, b, 1.3, -1, 0.0, w, ink);
    HBRUSH db = CreateSolidBrush(ink);
    HGDIOBJ ob = SelectObject(dc, db);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    const int r = (std::max)(2, size / 5);
    Ellipse(dc, cx - r, cy + size / 3 - r, cx + r + 1, cy + size / 3 + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(db);
}

// ------------------------------------------------------------------ atelier
// Full-grain leather. Outlines are a saddle stitch — 6px dashes, 4px gaps,
// each laid 12 degrees off the edge the way a saddle stitch actually lies —
// inside a painted edge. The active piece is fixed with a brass rivet, and
// fields are lined with a charcoal check. Brass is dots only: never a bar.

inline bool Stitch() { return Skinned() && Chrome().stitch; }

// A horizontal run of stitches starting at (x, y).
inline void StitchRun(HDC dc, int x, int y, int w, UINT dpi, COLORREF thread)
{
    const int L = Px(6, dpi), G = Px(4, dpi);
    const int dy = (std::max)(1, Px(1, dpi));           // the 12 degree lean
    HPEN pen = RoundPen((std::max)(1, Px(1, dpi) + Px(1, dpi) / 2), thread);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int sx = x; sx + L <= x + w; sx += L + G)
    {
        MoveToEx(dc, sx, y + dy, nullptr);
        LineTo(dc, sx + L, y - dy);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A vertical run of stitches starting at (x, y).
inline void StitchRunV(HDC dc, int x, int y, int h, UINT dpi, COLORREF thread)
{
    const int L = Px(6, dpi), G = Px(4, dpi);
    const int dx = (std::max)(1, Px(1, dpi));
    HPEN pen = RoundPen((std::max)(1, Px(1, dpi) + Px(1, dpi) / 2), thread);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int sy = y; sy + L <= y + h; sy += L + G)
    {
        MoveToEx(dc, x - dx, sy, nullptr);
        LineTo(dc, x + dx, sy + L);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

// The stitch around a rect, `inset` px inside its edge.
inline void StitchRect(HDC dc, const RECT& rc, int inset, UINT dpi, COLORREF thread)
{
    const int i = inset + Px(2, dpi);
    StitchRun(dc, rc.left + i, rc.top + i, rc.right - rc.left - 2 * i, dpi, thread);
    StitchRun(dc, rc.left + i, rc.bottom - 1 - i, rc.right - rc.left - 2 * i, dpi, thread);
    StitchRunV(dc, rc.left + i, rc.top + i, rc.bottom - rc.top - 2 * i, dpi, thread);
    StitchRunV(dc, rc.right - 1 - i, rc.top + i, rc.bottom - rc.top - 2 * i, dpi, thread);
}

// The painted edge: a 2px frame at the edge of the rect, outside the stitch.
inline void EdgePaint(HDC dc, const RECT& rc, UINT dpi, COLORREF paint)
{
    const int w = (std::max)(1, Px(2, dpi));
    HBRUSH b = CreateSolidBrush(paint);
    RECT t = { rc.left, rc.top, rc.right, rc.top + w };
    RECT l = { rc.left, rc.top, rc.left + w, rc.bottom };
    RECT bt = { rc.left, rc.bottom - w, rc.right, rc.bottom };
    RECT r = { rc.right - w, rc.top, rc.right, rc.bottom };
    FillRect(dc, &t, b);
    FillRect(dc, &l, b);
    FillRect(dc, &bt, b);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// A brass rivet: a disc with a dark ring.
inline void Rivet(HDC dc, int cx, int cy, int r, COLORREF brass, COLORREF ring)
{
    HBRUSH b = CreateSolidBrush(brass);
    HPEN p = CreatePen(PS_SOLID, 1, ring);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    DeleteObject(p);
}

// The check lining as a pattern brush, so every field is lined for free
// through WM_CTLCOLOREDIT. The caller owns the brush.
inline HBRUSH LiningBrush(UINT dpi, COLORREF ground, COLORREF line)
{
    const int P = (std::max)(4, Px(8, dpi));
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, P, P);
    HGDIOBJ ob = SelectObject(mdc, bm);
    RECT all = { 0, 0, P, P };
    HBRUSH gb = CreateSolidBrush(ground), lb = CreateSolidBrush(line);
    FillRect(mdc, &all, gb);
    RECT row = { 0, 0, P, 1 }, col = { 0, 0, 1, P };
    FillRect(mdc, &row, lb);
    FillRect(mdc, &col, lb);
    DeleteObject(gb);
    DeleteObject(lb);
    SelectObject(mdc, ob);
    HBRUSH pat = CreatePatternBrush(bm);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return pat;
}

// ---------------------------------------------------------------- reference
// Boutique hi-fi. A brushed aluminium plate with lettering ENGRAVED into it
// (ink with a highlight one pixel below), knurled knobs, an LED on anything
// that can be active, and a blue-backlit VU meter for anything that is a
// level.

inline bool Meter() { return Skinned() && Chrome().meter; }

// Horizontal brush hairlines every 2px, three percent over the plate.
inline void Brushed(HDC dc, const RECT& rc, UINT dpi, COLORREF ground)
{
    const COLORREF line = RGB((std::min)(255, GetRValue(ground) + 9),
                              (std::min)(255, GetGValue(ground) + 9),
                              (std::min)(255, GetBValue(ground) + 9));
    HBRUSH b = CreateSolidBrush(line);
    const int step = (std::max)(2, Px(2, dpi));
    for (int y = rc.top + 1; y < rc.bottom; y += step)
    {
        RECT l = { rc.left, y, rc.right, y + 1 };
        FillRect(dc, &l, b);
    }
    DeleteObject(b);
}

// Engraved lettering: the highlight copy one pixel down, then the ink.
inline void Engrave(HDC dc, const RECT& rc, const wchar_t* s, HFONT f,
                    COLORREF ink, COLORREF light, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    const int obk = SetBkMode(dc, TRANSPARENT);
    RECT lo = rc;
    OffsetRect(&lo, 0, 1);
    const COLORREF oc = SetTextColor(dc, light);
    DrawTextW(dc, s, -1, &lo, fmt);
    SetTextColor(dc, ink);
    RECT hi = rc;
    DrawTextW(dc, s, -1, &hi, fmt);
    SetTextColor(dc, oc);
    SetBkMode(dc, obk);
    SelectObject(dc, of);
}

// A knurled knob: a disc with 24 radial hairlines around its rim.
inline void Knurl(HDC dc, int cx, int cy, int r, COLORREF face, COLORREF dark,
                  COLORREF light)
{
    HBRUSH b = CreateSolidBrush(face);
    HPEN p = CreatePen(PS_SOLID, 1, dark);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    DeleteObject(b);
    HPEN lp = CreatePen(PS_SOLID, 1, light);
    for (int i = 0; i < 24; ++i)
    {
        const double a = i * 6.28318530718 / 24.0;
        SelectObject(dc, (i & 1) ? lp : p);
        MoveToEx(dc, cx + static_cast<int>(std::cos(a) * r * 0.62),
                 cy + static_cast<int>(std::sin(a) * r * 0.62), nullptr);
        LineTo(dc, cx + static_cast<int>(std::cos(a) * r),
               cy + static_cast<int>(std::sin(a) * r));
    }
    SelectObject(dc, op);
    DeleteObject(p);
    DeleteObject(lp);
}

// An LED: lit is the colour with a lighter ring; unlit is the colour dimmed.
inline void Led(HDC dc, int cx, int cy, int r, COLORREF c, bool lit)
{
    HBRUSH b = CreateSolidBrush(lit ? c : Dim(c, 0.35f));
    HPEN p = CreatePen(PS_SOLID, 1, lit ? RGB(255, 240, 200) : Dim(c, 0.25f));
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    DeleteObject(p);
}

// A VU meter: blue-lit face, arc scale -20..+3 with the red zone from 0, the
// needle at `frac` of full scale, pivot at the bottom centre.
inline void VuMeter(HDC dc, const RECT& rc, double frac, UINT dpi, COLORREF face,
                    COLORREF ink, COLORREF red, COLORREF needle,
                    const wchar_t* label, HFONT f)
{
    HBRUSH fb = CreateSolidBrush(face);
    HPEN fp = CreatePen(PS_SOLID, 1, RGB(0x7E, 0x82, 0x87));
    HGDIOBJ ob = SelectObject(dc, fb);
    HGDIOBJ op = SelectObject(dc, fp);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, Px(4, dpi), Px(4, dpi));
    SelectObject(dc, ob);
    DeleteObject(fb);
    const int cx = (rc.left + rc.right) / 2, cy = rc.bottom - Px(6, dpi);
    const int R = (std::min)((rc.right - rc.left) / 2 - Px(6, dpi),
                             rc.bottom - rc.top - Px(12, dpi));
    HPEN ip = CreatePen(PS_SOLID, 1, ink), rp = CreatePen(PS_SOLID, 2, red);
    const double a0 = 215.0, a1 = 325.0;
    auto pt = [&](double deg, double r, POINT& o) {
        const double a = deg * 3.14159265358979 / 180.0;
        o.x = cx + static_cast<LONG>(std::cos(a) * r);
        o.y = cy + static_cast<LONG>(std::sin(a) * r);
    };
    // The scale arc, sampled; ink to 80% of the sweep, red beyond.
    for (int k = 0; k < 44; ++k)
    {
        POINT p0, p1;
        pt(a0 + (a1 - a0) * k / 44.0, R, p0);
        pt(a0 + (a1 - a0) * (k + 1) / 44.0, R, p1);
        SelectObject(dc, k >= 35 ? rp : ip);
        MoveToEx(dc, p0.x, p0.y, nullptr);
        LineTo(dc, p1.x, p1.y);
    }
    // Ticks at -20, -10, -5, 0, +3.
    const double tk[5] = { 0.0, 0.4, 0.62, 0.8, 1.0 };
    for (double t : tk)
    {
        POINT p0, p1;
        pt(a0 + (a1 - a0) * t, R, p0);
        pt(a0 + (a1 - a0) * t, R - Px(5, dpi), p1);
        SelectObject(dc, t >= 0.8 ? rp : ip);
        MoveToEx(dc, p0.x, p0.y, nullptr);
        LineTo(dc, p1.x, p1.y);
    }
    // The needle.
    HPEN np = RoundPen((std::max)(1, Px(1, dpi)), needle);
    SelectObject(dc, np);
    POINT tip;
    pt(a0 + (a1 - a0) * (std::min)(1.0, (std::max)(0.0, frac)), R - Px(2, dpi), tip);
    MoveToEx(dc, cx, cy, nullptr);
    LineTo(dc, tip.x, tip.y);
    SelectObject(dc, op);
    DeleteObject(np);
    DeleteObject(ip);
    DeleteObject(rp);
    HBRUSH pb = CreateSolidBrush(RGB(0x0C, 0x0E, 0x12));
    HGDIOBJ ob2 = SelectObject(dc, pb);
    HGDIOBJ op2 = SelectObject(dc, fp);
    const int pr = Px(3, dpi);
    Ellipse(dc, cx - pr, cy - pr, cx + pr + 1, cy + pr + 1);
    SelectObject(dc, op2);
    SelectObject(dc, ob2);
    DeleteObject(pb);
    DeleteObject(fp);
    if (label && f)
    {
        RECT lr = { rc.left, cy - Px(16, dpi) - R / 2, rc.right, cy - R / 2 };
        HGDIOBJ of = SelectObject(dc, f);
        const int obk = SetBkMode(dc, TRANSPARENT);
        const COLORREF oc = SetTextColor(dc, ink);
        DrawTextW(dc, label, -1, &lr, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, oc);
        SetBkMode(dc, obk);
        SelectObject(dc, of);
    }
}

// ------------------------------------------------------------------ tenmoku
// Studio pottery. Glaze pools darker at every edge and thins to rust along
// the top; sparse silver-blue oil-spot flecks, seeded by position so they
// never crawl between repaints; a raw stoneware foot under every panel; and
// one cinnabar chop marking the active piece. Nothing else is red.

inline bool Glaze() { return Skinned() && Chrome().glaze; }

inline const COLORREF kRust = RGB(0x7A, 0x3B, 0x1E);
inline const COLORREF kOilSpot = RGB(0x7F, 0xA0, 0xB8);
inline const COLORREF kClay = RGB(0x8B, 0x73, 0x55);

// A pooled fill: the fill, two darker rings inset from the edge, the rim.
inline void Pool(HDC dc, const RECT& rc, COLORREF fill, COLORREF rim)
{
    HBRUSH fb = CreateSolidBrush(fill);
    FillRect(dc, &rc, fb);
    DeleteObject(fb);
    HPEN p1 = CreatePen(PS_SOLID, 1, Dim(fill, 0.5f));
    HPEN p2 = CreatePen(PS_SOLID, 1, Dim(fill, 0.75f));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    HGDIOBJ op = SelectObject(dc, p1);
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, p2);
    Rectangle(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p1);
    DeleteObject(p2);
    HBRUSH rb = CreateSolidBrush(rim);
    RECT r = { rc.left + 1, rc.top + 1, rc.right - 1, rc.top + 3 };
    FillRect(dc, &r, rb);
    DeleteObject(rb);
}

// The raw stoneware foot: a 3px band along the bottom of a panel.
inline void Foot(HDC dc, const RECT& rc, UINT dpi, COLORREF clay)
{
    HBRUSH b = CreateSolidBrush(clay);
    RECT f = { rc.left, rc.bottom - Px(3, dpi), rc.right, rc.bottom };
    FillRect(dc, &f, b);
    DeleteObject(b);
}

// Oil-spot flecks, at most one per 20px cell, placed by a hash of the cell.
inline void OilSpot(HDC dc, const RECT& rc, UINT dpi, COLORREF c)
{
    const int cell = (std::max)(8, Px(20, dpi));
    HBRUSH b = CreateSolidBrush(c);
    for (int cy = rc.top; cy < rc.bottom; cy += cell)
        for (int cx = rc.left; cx < rc.right; cx += cell)
        {
            const uint32_t h = static_cast<uint32_t>(cx / cell) * 2654435761u ^
                               static_cast<uint32_t>(cy / cell) * 40503u;
            if ((h >> 7) % 100 >= 22)
                continue;
            const int x = cx + static_cast<int>(h % cell);
            const int y = cy + static_cast<int>((h >> 11) % cell);
            const int s = ((h >> 19) & 1) ? 2 : 1;
            RECT f = { x, y, (std::min)(x + s, static_cast<int>(rc.right)),
                       (std::min)(y + s, static_cast<int>(rc.bottom)) };
            FillRect(dc, &f, b);
        }
    DeleteObject(b);
}

// The chop: a cinnabar seal with its centre knocked out to the ground.
inline void Chop(HDC dc, int x, int y, int size, COLORREF cinnabar, COLORREF ground)
{
    HBRUSH cb = CreateSolidBrush(cinnabar), gb = CreateSolidBrush(ground);
    HGDIOBJ ob = SelectObject(dc, cb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    const int r = (std::max)(2, size / 4);
    RoundRect(dc, x, y, x + size + 1, y + size + 1, r, r);
    SelectObject(dc, gb);
    const int in = (std::max)(1, size / 3);
    RoundRect(dc, x + in, y + in, x + size - in + 1, y + size - in + 1, 1, 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(cb);
    DeleteObject(gb);
}

} // namespace skin
} // namespace amber
