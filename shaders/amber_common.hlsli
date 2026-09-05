// amber_common.hlsli — shared constants, structs, amber ramp, simplex/curl noise.
#ifndef AMBER_COMMON_HLSLI
#define AMBER_COMMON_HLSLI

// Mirrors FrameCB in src/common.h — do not reorder.
cbuffer FrameCB : register(b0)
{
    float time, dt, curlAmp, springK;
    float damping, transitionDur, cellW, cellH;
    float originX, originY, screenW, screenH;
    uint  cols, rows, particleCount, cursorIndex;
    float cursorBright, glowSize, breatheAmp, hdrBoost;
    uint  resetFlag, framePad0;
    float noiseSpeed, noiseScale;
    // --- particle model (four-layer / force-field extensions) ---
    uint  particlesPerCell, animStyle;
    float spreadRadius, shimmerSpeed;
    float twinkleAmp, flickerAmp;
    float mouseX, mouseY;
    float mouseRadius, mouseForce;
    float shockX, shockY;
    float shockTime, effectSpeed;
    float dragAmt, scatterSuppress;
    float trailScale, cursorPhase;
    float cursorActivity, framePad6;
    // Active theme's intensity ramp (5 linear-light stops, w unused).
    float4 ramp0, ramp1, ramp2, ramp3, ramp4;
    // Light ("paper") mode: lightMode 1 = dark-ink particles; inkColor is the
    // linear dark ink used for default-foreground particles.
    float lightMode, heatTau, phosphorDecay, cursorShape;
    float4 inkColor;
    // Eye-candy set (see FrameCB in common.h).
    float heatAmp, ghostAmp, audioWind, rainMode;
    float cursorBlink;                 // Appearance: cursor blink on/off
    // Panel repulsion: an opaque overlay's rectangle, which the field is
    // pushed out of and gathers around. panelW <= 0 disables it.
    float panelX, panelY, panelW;
    float panelH, panelMargin, departStyle, warmup;
    float tiltX, tiltY, sloshX, sloshY;
    float fxPad0, fxPad1, fxPad2, fxPad3;
};

// Glyph particle templates are always stored at this stride so the density
// setting can change without rebuilding the template table.
static const uint kMaxPPC = 128;

struct Particle
{
    float2 pos;
    float2 vel;
    float  bright;
    float  seed;
    float  tint;       // raw color bits (see particle_draw)
    float  lastBirth;  // cell birth stamp this particle has reacted to —
                       // drives the one-shot per-letter materialize scatter
};

struct CellGpu
{
    uint  glyph;
    uint  prevGlyph;
    float birth;
    float bright;
    uint  flags;      // 1 = selected, 2 = explicit color
    uint  fgRgb;      // resolved sRGB foreground when flag 2 is set
};

// Unpacks 0xRRGGBB sRGB into linear light.
float3 UnpackSrgbLinear(uint rgb)
{
    float3 c = float3((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) / 255.0;
    return pow(max(c, 0.0), 2.2);
}

// ------------------------------------------------------------------ amber ramp
// Intensity ramp of the ACTIVE THEME — 5 linear-light stops uploaded in the
// frame constants (defaults are the Amber Nixie phosphor:
// #3D2A00 -> #7A5500 -> #FFB000 -> #FFD54A -> #FFF3C4).
float3 AmberRamp(float t)
{
    t = saturate(t);
    if (t < 0.25) return lerp(ramp0.xyz, ramp1.xyz, t / 0.25);
    if (t < 0.55) return lerp(ramp1.xyz, ramp2.xyz, (t - 0.25) / 0.30);
    if (t < 0.80) return lerp(ramp2.xyz, ramp3.xyz, (t - 0.55) / 0.25);
    return lerp(ramp3.xyz, ramp4.xyz, (t - 0.80) / 0.20);
}

// ----------------------------------------------------- Miami Sunset selection
// #6C3BFF -> #D92BFF -> #FF2D95 -> #FF6A3D -> #FFD166, in linear light.
static const float3 kMiami0 = float3(0.1449, 0.0439, 1.0000);
static const float3 kMiami1 = float3(0.7305, 0.0265, 1.0000);
static const float3 kMiami2 = float3(1.0000, 0.0265, 0.2874);
static const float3 kMiami3 = float3(1.0000, 0.1516, 0.0508);
static const float3 kMiami4 = float3(1.0000, 0.6376, 0.1441);

float3 MiamiRamp(float t)
{
    t = saturate(t);
    if (t < 0.25) return lerp(kMiami0, kMiami1, t / 0.25);
    if (t < 0.50) return lerp(kMiami1, kMiami2, (t - 0.25) / 0.25);
    if (t < 0.75) return lerp(kMiami2, kMiami3, (t - 0.50) / 0.25);
    return lerp(kMiami3, kMiami4, (t - 0.75) / 0.25);
}

#include "amber_noise.hlsli"


#endif // AMBER_COMMON_HLSLI
