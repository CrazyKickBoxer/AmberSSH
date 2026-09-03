// palette.h — terminal color model: packed per-cell colors that preserve the
// distinction between "default", ANSI palette indices, xterm-256 indices, and
// explicit 24-bit RGB, plus the configurable 16-color palettes.
//
// A CellColor packs into 32 bits: the top byte selects the mode, the low
// 24 bits carry the payload. Resolution to RGB happens at render time, so a
// palette switch recolors scrollback retroactively — exactly what a real
// terminal does.
#pragma once

#include <algorithm>
#include <cstdint>

namespace amber
{

using CellColor = uint32_t;

enum : uint32_t
{
    kColModeDefault = 0x00u,
    kColModePalette = 0x01u,   // low byte = index 0-255 (16 ANSI + 240 xterm)
    kColModeRgb     = 0x02u,   // low 24 bits = RRGGBB
};

constexpr CellColor kColorDefault = 0;

constexpr CellColor ColPalette(uint32_t index)
{
    return (kColModePalette << 24) | (index & 0xFFu);
}

constexpr CellColor ColRgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (kColModeRgb << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) |
           (b & 0xFFu);
}

constexpr uint32_t ColMode(CellColor c) { return c >> 24; }
constexpr bool ColIsDefault(CellColor c) { return ColMode(c) == kColModeDefault; }

// ---------------------------------------------------------------- palettes
struct Palette16
{
    const char* name;
    uint32_t c[16];   // 0xRRGGBB
};

// High-contrast house palette: neon accents that read against pure black
// while the default foreground stays amber.
inline const Palette16& PaletteAmberMiami()
{
    static const Palette16 p = {
        "Amber Miami",
        {
            0x0A0A0D, 0xFF4D6D, 0x63E6A6, 0xFFD166,
            0x5AA9FF, 0xFF4FD8, 0x2DE2E6, 0xE8E8EC,
            0x676773, 0xFF718A, 0x8AF0BD, 0xFFE08A,
            0x82C1FF, 0xFF7BE3, 0x73F1F3, 0xFFFFFF,
        }
    };
    return p;
}

inline const Palette16& PaletteClassicXterm()
{
    static const Palette16 p = {
        "Classic xterm",
        {
            0x000000, 0xCD0000, 0x00CD00, 0xCDCD00,
            0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
            0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
            0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
        }
    };
    return p;
}

// Canonical xterm-256: 16 palette entries, a 6x6x6 color cube, then the
// 24-step grayscale ramp.
inline uint32_t Resolve256(uint32_t index, const Palette16& pal)
{
    index &= 0xFFu;
    if (index < 16)
        return pal.c[index];
    if (index < 232)
    {
        static const uint32_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        uint32_t i = index - 16;
        return (lv[i / 36] << 16) | (lv[(i / 6) % 6] << 8) | lv[i % 6];
    }
    uint32_t g = 8 + (index - 232) * 10;
    return (g << 16) | (g << 8) | g;
}

// Resolves a packed CellColor to 0xRRGGBB; `defaultRgb` supplies the value for
// mode-default colors (amber for foreground, black for background).
inline uint32_t ResolveCellColor(CellColor c, const Palette16& pal,
                                 uint32_t defaultRgb)
{
    switch (ColMode(c))
    {
    case kColModePalette: return Resolve256(c & 0xFFu, pal);
    case kColModeRgb:     return c & 0xFFFFFFu;
    default:              return defaultRgb;
    }
}

inline float SrgbLumOfRgb(uint32_t rgb)
{
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace amber
