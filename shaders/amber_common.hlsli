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

float HashU(uint n)
{
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffff);
}

// -------------------------------------------------- simplex noise (3D, no LUT)
// Ashima Arts / Stefan Gustavson public-domain simplex noise, HLSL port.
float3 sn_mod289(float3 x) { return x - floor(x / 289.0) * 289.0; }
float4 sn_mod289(float4 x) { return x - floor(x / 289.0) * 289.0; }
float4 sn_permute(float4 x) { return sn_mod289(((x * 34.0) + 1.0) * x); }
float4 sn_taylorInvSqrt(float4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(float3 v)
{
    const float2 C = float2(1.0 / 6.0, 1.0 / 3.0);
    const float4 D = float4(0.0, 0.5, 1.0, 2.0);

    float3 i  = floor(v + dot(v, C.yyy));
    float3 x0 = v - i + dot(i, C.xxx);

    float3 g  = step(x0.yzx, x0.xyz);
    float3 l  = 1.0 - g;
    float3 i1 = min(g.xyz, l.zxy);
    float3 i2 = max(g.xyz, l.zxy);

    float3 x1 = x0 - i1 + C.xxx;
    float3 x2 = x0 - i2 + C.yyy;
    float3 x3 = x0 - D.yyy;

    i = sn_mod289(i);
    float4 p = sn_permute(sn_permute(sn_permute(
                  i.z + float4(0.0, i1.z, i2.z, 1.0))
                + i.y + float4(0.0, i1.y, i2.y, 1.0))
                + i.x + float4(0.0, i1.x, i2.x, 1.0));

    float  n_ = 0.142857142857;      // 1/7
    float3 ns = n_ * D.wyz - D.xzx;

    float4 j  = p - 49.0 * floor(p * ns.z * ns.z);

    float4 x_ = floor(j * ns.z);
    float4 y_ = floor(j - 7.0 * x_);

    float4 x  = x_ * ns.x + ns.yyyy;
    float4 y  = y_ * ns.x + ns.yyyy;
    float4 h  = 1.0 - abs(x) - abs(y);

    float4 b0 = float4(x.xy, y.xy);
    float4 b1 = float4(x.zw, y.zw);

    float4 s0 = floor(b0) * 2.0 + 1.0;
    float4 s1 = floor(b1) * 2.0 + 1.0;
    float4 sh = -step(h, float4(0, 0, 0, 0));

    float4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    float4 a1 = b1.xzyw + s1.xzyw * sh.zzww;

    float3 p0 = float3(a0.xy, h.x);
    float3 p1 = float3(a0.zw, h.y);
    float3 p2 = float3(a1.xy, h.z);
    float3 p3 = float3(a1.zw, h.w);

    float4 norm = sn_taylorInvSqrt(float4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;

    float4 m = max(0.6 - float4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m * m, float4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}

// 2D curl of a scalar simplex potential — divergence-free gas drift.
float2 Curl2(float3 p)
{
    const float e = 0.35;
    float dy = snoise(p + float3(0, e, 0)) - snoise(p - float3(0, e, 0));
    float dx = snoise(p + float3(e, 0, 0)) - snoise(p - float3(e, 0, 0));
    return float2(dy, -dx) / (2.0 * e);
}

#endif // AMBER_COMMON_HLSLI
