// Theme.h — shared native-dialog theming derived from the live app theme.
//
// Every Win32 surface (connection manager, SFTP browser, prompts) reads its
// palette from the active theme's sRGB stops (amber::gThemeSrgb) so the whole
// app recolours together, and gets the Termius-style dark, rounded-corner
// window chrome from ApplyWindowChrome.
#pragma once

#include <Windows.h>
#include <dwmapi.h>

#include <algorithm>

#include "../common.h"
#include "Chrome.h"

#pragma comment(lib, "dwmapi.lib")

namespace amber
{

// Undocumented-but-stable DWM attribute ids (Windows 11).
enum : DWORD
{
    kDwmImmersiveDark   = 20,   // DWMWA_USE_IMMERSIVE_DARK_MODE
    kDwmCornerPref      = 33,   // DWMWA_WINDOW_CORNER_PREFERENCE
    kDwmBorderColor     = 34,   // DWMWA_BORDER_COLOR
    kDwmCaptionColor    = 35,   // DWMWA_CAPTION_COLOR
    kDwmTextColor       = 36,   // DWMWA_TEXT_COLOR
};
enum : DWORD { kDwmCornerRound = 2 };   // DWMWCP_ROUND

inline COLORREF ScaleSrgb(uint32_t c, float f)
{
    auto ch = [&](int s) {
        return static_cast<int>(
            (std::min)(255.0f, ((c >> s) & 0xFF) * f) + 0.5f);
    };
    return RGB(ch(16), ch(8), ch(0));
}

// Blend b into a by t. Light skins tint away from white rather than scaling
// towards black, which on paper reads as a smudge instead of a highlight.
inline COLORREF MixSrgb(uint32_t a, uint32_t b, float t)
{
    auto ch = [&](int s) {
        float av = static_cast<float>((a >> s) & 0xFF);
        float bv = static_cast<float>((b >> s) & 0xFF);
        return static_cast<int>(av + (bv - av) * t + 0.5f);
    };
    return RGB(ch(16), ch(8), ch(0));
}

// Rec. 601 luma, used to decide whether text on a colour should be black.
inline float LumaSrgb(uint32_t c)
{
    return (0.299f * ((c >> 16) & 0xFF) + 0.587f * ((c >> 8) & 0xFF) +
            0.114f * (c & 0xFF)) / 255.0f;
}

// Black or white, whichever reads on the given ground.
inline COLORREF InkOn(uint32_t ground)
{
    return LumaSrgb(ground) > 0.55f ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// The full palette a themed dialog needs, derived from the 5 theme stops
// (0 = dim glow … 4 = hot highlight).
struct DialogPalette
{
    COLORREF bg;          // window ground
    COLORREF field;       // edit / list wells
    COLORREF text;        // primary text
    COLORREF textDim;     // labels
    COLORREF textDis;     // disabled
    COLORREF border;      // resting outline
    COLORREF borderHot;   // focus / hover outline
    COLORREF accent;      // primary-button fill / selection bar
    COLORREF accentText;  // text on accent
    COLORREF selBg;       // selection wash
    COLORREF selText;
    COLORREF banner;      // wordmark / heading
};

inline COLORREF SrgbRef(uint32_t c)
{
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

inline DialogPalette MakeDialogPalette()
{
    const ChromeSpec& ch = Chrome();
    if (!ch.useThemeAccent)
    {
        // A skin with its own palette (Cyberpunk): neon accents on a dark
        // ground, independent of the terminal theme.
        DialogPalette p;
        p.bg         = SrgbRef(ch.bg);
        p.field      = SrgbRef(ch.field);
        p.text       = SrgbRef(ch.text);
        p.textDim    = SrgbRef(ch.textDim);
        p.textDis    = ScaleSrgb(ch.textDim, 0.55f);
        p.border     = SrgbRef(ch.border);
        p.borderHot  = SrgbRef(ch.neonA);
        p.accent     = SrgbRef(ch.neonB);
        // Text sitting ON the accent fill, chosen by the accent's own luma so
        // a brass or paper skin does not end up with white on pale gold.
        p.accentText = InkOn(ch.neonB);
        // A light skin cannot wash its selection towards black — that reads as
        // a smudge on paper. Tint towards the accent from white instead.
        p.selBg      = ch.lightGround ? MixSrgb(0xFFFFFF, ch.neonA, 0.16f)
                                      : ScaleSrgb(ch.neonA, 0.22f);
        p.selText    = SrgbRef(ch.neonA);
        p.banner     = SrgbRef(ch.neonA);
        return p;
    }
    const uint32_t* s = gThemeSrgb;
    DialogPalette p;
    p.bg         = ScaleSrgb(s[0], 0.34f);
    p.field      = ScaleSrgb(s[0], 0.62f);
    p.text       = ScaleSrgb(s[3], 0.92f);
    p.textDim    = ScaleSrgb(s[2], 0.66f);
    p.textDis    = ScaleSrgb(s[1], 0.62f);
    p.border     = ScaleSrgb(s[2], 0.42f);
    p.borderHot  = s[3];
    p.accent     = s[2];
    p.accentText = ScaleSrgb(s[0], 0.55f);
    p.selBg      = ScaleSrgb(s[1], 0.62f);
    p.selText    = s[4];
    p.banner     = s[3];
    return p;
}

// Dark titlebar + themed caption/border + rounded corners, in one call.
inline void ApplyWindowChrome(HWND hwnd)
{
    DialogPalette p = MakeDialogPalette();
    // A light skin must leave immersive dark mode or Windows draws its own
    // caption glyphs in white on the pale caption we are about to set.
    BOOL dark = Chrome().lightGround ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, kDwmImmersiveDark, &dark, sizeof(dark));
    COLORREF cap = p.bg;
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &cap, sizeof(cap));
    COLORREF ct = p.text;
    DwmSetWindowAttribute(hwnd, kDwmTextColor, &ct, sizeof(ct));
    COLORREF bc = Chrome().useThemeAccent ? ScaleSrgb(gThemeSrgb[2], 0.55f)
                                          : ScaleSrgb(Chrome().neonA, 0.6f);
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &bc, sizeof(bc));
    DWORD round = kDwmCornerRound;
    DwmSetWindowAttribute(hwnd, kDwmCornerPref, &round, sizeof(round));
}

} // namespace amber
