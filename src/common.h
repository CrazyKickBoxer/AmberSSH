// common.h — shared types, HRESULT handling, PIX markers, small utilities.
#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

constexpr uint32_t kFramesInFlight   = 3;
constexpr uint32_t kParticlesPerCell = 128;    // glyph template stride (points per glyph)
// Live-particle ceiling. Above the 128-slot template stride, extra particles
// repeat slots with static sub-pixel jitter and energy-conserving weights —
// density past 128 buys plusher, rounder strokes rather than brightness.
constexpr uint32_t kMaxParticlesPerCell = 256;
constexpr DXGI_FORMAT kSceneFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;

struct Float2 { float x = 0, y = 0; };

// One glyph-template slot: a cell-local position on the 8x16 pixel grid plus
// the antialiased coverage weight of that pixel (0 = empty, never rendered).
// Mirrors float4 in the sim shader's gPoints buffer.
struct GlyphPoint
{
    float x = 0.5f, y = 0.5f;
    float w = 0.0f;
    float pad = 0.0f;
};

inline void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

inline uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// ------------------------------------------------------------------ PIX events
inline void PixBegin(ID3D12GraphicsCommandList* cl, const wchar_t* name)
{
    cl->BeginEvent(0 /*PIX_EVENT_UNICODE_VERSION*/, name,
                   static_cast<UINT>((wcslen(name) + 1) * sizeof(wchar_t)));
}
inline void PixEnd(ID3D12GraphicsCommandList* cl) { cl->EndEvent(); }

struct PixScope
{
    ID3D12GraphicsCommandList* cl;
    PixScope(ID3D12GraphicsCommandList* c, const wchar_t* name) : cl(c) { PixBegin(cl, name); }
    ~PixScope() { PixEnd(cl); }
};

// -------------------------------------------------------------- string helpers
inline std::wstring WideFromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string Utf8FromWide(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline void AppendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

inline std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

// -------------------------------------------- GPU-visible cell / frame structs
// Mirrors FrameCB in shaders/amber_common.hlsli — do not reorder.
struct FrameCB
{
    float time, dt, curlAmp, springK;
    float damping, transitionDur, cellW, cellH;
    float originX, originY, screenW, screenH;
    uint32_t cols, rows, particleCount, cursorIndex;
    float cursorBright, glowSize, breatheAmp, hdrBoost;
    uint32_t resetFlag, framePad0;
    float noiseSpeed, noiseScale;
    // --- particle model (four-layer / force-field extensions) ---
    uint32_t particlesPerCell = 32, animStyle = 0;
    float spreadRadius = 0.22f, shimmerSpeed = 4.0f;
    float twinkleAmp = 0.10f, flickerAmp = 0.04f;
    float mouseX = -1e6f, mouseY = -1e6f;
    float mouseRadius = 120.0f, mouseForce = 0.0f;
    float shockX = 0.0f, shockY = 0.0f;
    float shockTime = -1.0f, effectSpeed = 1.0f;
    // dragAmt: 0 = flowy (x0.96/step) .. 1 = sticky (x0.60), per the reference
    // multiplicative-drag integrator used by the motion styles.
    // scatterSuppress: 1 when most of the screen changed this frame (scroll,
    // clear) — per-letter materialize bursts are skipped so a scroll does not
    // detonate the whole grid at once.
    float dragAmt = 0.25f, scatterSuppress = 0.0f;
    // trailScale: seconds of velocity smeared into a fading streak behind
    // fast-moving particles (0 disables). Settled particles have ~zero
    // velocity, so trails can never blur resting text.
    float trailScale = 0.035f;
    // cursorPhase: CPU-accumulated cursor-orbit angle. It only advances while
    // the cursor is travelling, so a parked cursor's swirl glides to a stop.
    float cursorPhase = 0.0f;
    // cursorActivity: 1 while the cursor is travelling (plus a short grace),
    // easing to 0 when it parks — lets style cursors react to motion.
    float cursorActivity = 0.0f;
    float framePad6 = 0.0f;
    // Theme ramp: 5 linear-light stops (w unused) replacing the hardcoded
    // amber constants, so the particle field follows the active theme.
    float ramp0[4] = { 0.0430f, 0.0187f, 0.0f, 0.0f };
    float ramp1[4] = { 0.1960f, 0.0889f, 0.0f, 0.0f };
    float ramp2[4] = { 1.0f, 0.4423f, 0.0f, 0.0f };
    float ramp3[4] = { 1.0f, 0.6730f, 0.0660f, 0.0f };
    float ramp4[4] = { 1.0f, 0.8990f, 0.5610f, 0.0f };
    // Light mode: 1 = paper (dark ink particles via the alpha-blend PSO).
    // inkColor is the linear dark ink used for default-fg particles.
    float lightMode = 0.0f;
    // heatTau: seconds for the heat-map boost to decay (short in full-screen
    // apps so only the cells that actually changed flash — "diff glow").
    // phosphorDecay: >0 = time constant (s) for particle light to fade when
    // a cell goes darker (0 = instant, the default behaviour).
    // cursorShape: 0 block (orbiting rect), 1 underline, 2 vertical bar
    // (Appearance page).
    float heatTau = 2.5f, phosphorDecay = 0.0f, cursorShape = 0.0f;
    float inkColor[4] = { 0.06f, 0.05f, 0.04f, 0.0f };
    // Eye-candy set: heat-map boost for freshly changed cells, latency ghost
    // trail length (0..1 from RTT), audio-reactive wind (0..1), and the
    // idle screensaver's digital-rain blend (0..1).
    float heatAmp = 0.0f, ghostAmp = 0.0f, audioWind = 0.0f, rainMode = 0.0f;
    // Appearance page: cursor blink on (1) / off (0).
    float cursorBlink = 1.0f;
    // Panel repulsion. An opaque overlay (the command palette, the journal,
    // the paste guard) evicts the particle field from its rectangle, so the
    // text is pushed out and gathers along the panel's edge instead of
    // showing through it. panelW <= 0 disables the whole thing.
    float panelX = 0.0f, panelY = 0.0f, panelW = 0.0f;
    float panelH = 0.0f;
    // How far outside the rectangle the evicted particles pile up.
    float panelMargin = 0.0f;
    // How an erased letter LEAVES: 0 fade, 1 ash, 2 smoke, 3 sand, 4 shatter.
    // Arrivals and departures are chosen separately.
    float departStyle = 0.0f;
    // Phosphor warm-up: 0 = stone cold (dim, blue-shifted), 1 = at
    // temperature. Ramps over the first seconds of a session.
    float warmup = 1.0f;
    // Depth parallax: the field tilts with the pointer, and rows further up
    // (and the scrollback behind them) sit deeper, so they move less.
    float tiltX = 0.0f, tiltY = 0.0f;
    // Window slosh: an impulse applied when the window is dragged or resized,
    // so the field lags behind the frame and catches up.
    float sloshX = 0.0f, sloshY = 0.0f;
    // Night shift is applied on the CPU instead: it warms the theme stops, so
    // the particle ramp, the crisp cores and the dialogs all shift together
    // from one place. Nothing for the shader to do.
    float fxPad0 = 0.0f, fxPad1 = 0.0f, fxPad2 = 0.0f, fxPad3 = 0.0f;
};
static_assert(sizeof(FrameCB) == 368, "FrameCB must match HLSL layout");

// Mirrors CellGpu in shaders/amber_common.hlsli.
struct CellGpu
{
    uint32_t glyph = 0;
    uint32_t prevGlyph = 0;
    float birth = -100.0f;
    float bright = 0.0f;
    uint32_t flags = 0;
    uint32_t fgRgb = 0;      // resolved sRGB foreground when kCellFlagColor set
};
static_assert(sizeof(CellGpu) == 24, "CellGpu must match HLSL layout");

// CPU-side per-cell desired appearance for the current frame.
struct CellVisual
{
    uint32_t glyph = 0;
    float bright = 0.0f;
    uint32_t rgb = 0;        // resolved 0xRRGGBB when kVisColor is set
    uint32_t flags = 0;      // kVisSelected | kVisColor
};

constexpr uint32_t kVisSelected = 1u;
constexpr uint32_t kVisColor = 2u;
// Quiet cell: set for alternate-screen panes (htop/top/vim), where a refresh
// churns many cells a second and a full scatter per digit reads as chaos.
// Unless the frame is a bulk repaint (the app opening, a page change — those
// keep the motion style and its reveal wave), a change on a quiet cell morphs
// in place via the Direct path: no scatter, no style force, no twirl.
constexpr uint32_t kVisQuiet = 4u;

// CellGpu::flags bits (bit 0 selected; bit 1 explicit color — the particle
// shader uses the packed RGB instead of the amber ramp when set; bit 2 quiet
// transition — the shader runs the Direct morph for this cell's current
// glyph generation regardless of the global motion style).
constexpr uint32_t kCellFlagSelected = 1u;
constexpr uint32_t kCellFlagColor = 2u;
constexpr uint32_t kCellFlagQuiet = 4u;

constexpr uint32_t kNoCursor = 0xFFFFFFFFu;

// ------------------------------------------------------------- color helpers
inline void SrgbToLinear(uint32_t rgb, float out[3])
{
    auto conv = [](uint32_t v) {
        float f = v / 255.0f;
        return f <= 0.04045f ? f / 12.92f
                             : std::pow((f + 0.055f) / 1.055f, 2.4f);
    };
    out[0] = conv((rgb >> 16) & 0xFF);
    out[1] = conv((rgb >> 8) & 0xFF);
    out[2] = conv(rgb & 0xFF);
}

// Active theme ramp: 5 linear-light stops, defaulting to the Amber Nixie
// phosphor. App::ApplyTheme overwrites these; every intensity-colored surface
// (particles via FrameCB, UI chrome via this CPU mirror) themes at once.
inline float gThemeStops[5][3] = {
    { 0.0430f, 0.0187f, 0.0f }, { 0.1960f, 0.0889f, 0.0f },
    { 1.0f, 0.4423f, 0.0f },    { 1.0f, 0.6730f, 0.0660f },
    { 1.0f, 0.8990f, 0.5610f },
};

// sRGB (0xRRGGBB) mirror of the active theme's 5 stops, so native Win32
// dialogs can derive themed COLORREFs without a linear→sRGB conversion.
// App::ApplyTheme fills this; defaults to Amber Nixie.
inline uint32_t gThemeSrgb[5] = {
    0x3D2A00, 0x7A5500, 0xFFB000, 0xFFD54A, 0xFFF3C4,
};

// CPU mirror of AmberRamp in amber_common.hlsli, evaluated against the
// active theme's stops.
inline void AmberRampCpu(float t, float out[3])
{
    static const float pos[5] = { 0.0f, 0.25f, 0.55f, 0.80f, 1.0f };
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    int seg = 0;
    while (seg < 3 && t > pos[seg + 1])
        ++seg;
    float f = (t - pos[seg]) / (pos[seg + 1] - pos[seg]);
    for (int i = 0; i < 3; ++i)
        out[i] = gThemeStops[seg][i] +
                 (gThemeStops[seg + 1][i] - gThemeStops[seg][i]) * f;
}
