// motion_styles.h — the motion-style registry.
//
// This table is the single CPU-side definition of a style. It feeds the Motion
// menu, the status line, the command palette, the settings clamp, the
// crisp-core hold and the bulk reveal-wave stretch. Adding a style means
// adding one row here plus its behaviour in shaders/particle_sim.hlsl —
// nothing else on the CPU side needs touching.
//
// The header deliberately depends on nothing but <cstdint> so the tests can
// include it without pulling in Direct3D.
#pragma once

#include <cstdint>

struct MotionStyleInfo
{
    const wchar_t* menu;   // Motion-menu label, with its & accelerator
    const char* name;      // status line / command palette
    // Seconds (at 1x speed) that the sharp letterform is held back for. A
    // style whose entrance IS the letter — a tumble, a descent, a burn — must
    // not have a crisp copy of the finished glyph sitting still underneath it.
    float coreDelay;
    // Multiplier on the top-to-bottom reveal wave used for bulk repaints, so
    // slower choreography still reads when a whole screen arrives at once.
    float waveScale;
};

inline constexpr MotionStyleInfo kMotionStyles[] = {
    { L"&Direct (Normal)",   "Direct",             0.00f, 1.0f },
    { L"Nebula &Twist",      "Nebula Twist",       0.00f, 1.0f },
    { L"Digital &Rain",      "Digital Rain",       0.00f, 1.0f },
    { L"&Quantum Flux",      "Quantum Flux",       0.00f, 1.0f },
    { L"&Sonic Boom",        "Sonic Boom",         0.00f, 1.0f },
    { L"&Magnetic Assemble", "Magnetic Assemble",  0.00f, 1.0f },
    { L"&Cyclone",           "Cyclone",            0.00f, 1.0f },
    { L"&Fountain",          "Fountain",           0.00f, 1.0f },
    { L"&Glitch",            "Glitch",             0.00f, 1.0f },
    { L"S&lipstream",        "Slipstream",         0.00f, 1.0f },
    { L"T&umble",            "Tumble",             1.00f, 1.0f },
    { L"M&othership (ID4)",  "Mothership (ID4)",   1.10f, 2.0f },
    { L"St&arwake",          "Starwake",           0.52f, 1.4f },
    { L"Gunmetal &Iris",     "Gunmetal Iris",      0.72f, 1.6f },
    { L"Omi&nous Signal",    "Ominous Signal",     1.05f, 1.8f },
    { L"&Hunter Vision",     "Hunter Vision",      0.95f, 1.7f },
    { L"Film &Burn",         "Film Burn",          0.80f, 1.5f },
    { L"San&dfall",          "Sandfall",           0.80f, 1.5f },
    { L"&Weld",              "Weld",               0.55f, 1.3f },
    { L"M&urmuration",       "Murmuration",        0.95f, 1.7f },
    { L"Hamm&er",            "Hammer",             0.30f, 1.2f },
    { L"&Game of Life",      "Game of Life",       0.85f, 1.6f },
    { L"Sur&face",           "Surface",            0.70f, 1.4f },
};

inline constexpr int kMotionStyleCount =
    static_cast<int>(sizeof(kMotionStyles) / sizeof(kMotionStyles[0]));

// Clamped lookup: callers may hold a style index read from an older settings
// file, or from a build that had more styles than this one.
inline const MotionStyleInfo& MotionStyleAt(uint32_t style)
{
    return kMotionStyles[style < static_cast<uint32_t>(kMotionStyleCount)
                             ? style
                             : 0u];
}
