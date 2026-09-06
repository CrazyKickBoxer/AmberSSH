// Contrast.h — WCAG relative luminance and contrast, and the one thing the
// renderer needs from it: how far it may dim text before it stops being
// readable.
//
// Faint text (SGR 2) is drawn as the theme's foreground at a fraction of its
// brightness. At 0.55 that is comfortable on most of the shipped themes and
// below WCAG AA on three of them, which is a legibility bug rather than a
// style choice. The floor here is computed per theme, so a theme that already
// passes is not touched and only the ones that were failing move.
//
// Pure: no Windows, no renderer. Colours are sRGB 0xRRGGBB.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace amber
{

// WCAG 2.x relative luminance.
inline double RelativeLuminance(uint32_t srgb)
{
    auto chan = [](uint32_t v) {
        const double x = static_cast<double>(v) / 255.0;
        return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * chan((srgb >> 16) & 0xFF) +
           0.7152 * chan((srgb >> 8) & 0xFF) +
           0.0722 * chan(srgb & 0xFF);
}

// WCAG contrast ratio, always >= 1, order-independent.
inline double ContrastRatio(uint32_t a, uint32_t b)
{
    const double la = RelativeLuminance(a), lb = RelativeLuminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// The smallest brightness factor that keeps `fg` at or above `target` contrast
// against `bg`, clamped to [0, 1].
//
// The renderer scales luminance, so this solves for the factor directly rather
// than searching. When the foreground does not reach `target` even at full
// brightness the answer is 1: nothing this function can return would fix it,
// and returning less would quietly pretend otherwise.
inline float MinBrightnessForContrast(uint32_t fg, uint32_t bg, double target = 4.5)
{
    const double lb = RelativeLuminance(bg);
    const double lf = RelativeLuminance(fg);
    if (lf <= lb)
        return 1.0f;                       // dark text on a light ground: not this path
    const double needed = target * (lb + 0.05) - 0.05;
    if (needed <= 0.0)
        return 0.0f;
    if (lf <= 0.0 || needed >= lf)
        return 1.0f;
    float r = static_cast<float>(std::clamp(needed / lf, 0.0, 1.0));
    // Rounding the answer to a float can land a hair under the target, which
    // would make this function's own guarantee false by a millionth. Step up
    // to the next representable value until it holds.
    while (r < 1.0f && static_cast<double>(r) * lf < needed)
        r = std::nextafter(r, 1.0f);
    return r;
}

// What faint text is actually drawn at: the style's 0.55, raised to whatever
// the theme needs to stay legible. Never darker than 0.55, never lighter than
// full — a floor, not an override.
inline float DimFactorFor(uint32_t fg, uint32_t bg, float preferred = 0.55f,
                          double target = 4.5)
{
    return std::max(preferred, MinBrightnessForContrast(fg, bg, target));
}

} // namespace amber
