// desktop_common.hlsli — the VNC particle desktop's GPU contract.
//
// DesktopCB is mirrored in src/render/desktop.h and its scalar count is
// checked by ShaderContractTests, the same way FrameCB is: a field added on
// one side and forgotten on the other produces silently wrong rendering,
// not a compile error. It binds at b1 so that amber_common.hlsli's FrameCB
// (b0) can be included for its helpers without a register collision; the
// desktop shaders never read FrameCB.
//
// A desktop particle is sixteen bytes: at 8.3 million particles anything
// wider costs more memory than the framebuffer it is drawing. Velocity and
// the two scalars are half precision; position is not, because a home that
// drifts by a quarter pixel is exactly the jitter faithful mode forbids.
#ifndef DESKTOP_COMMON_HLSLI
#define DESKTOP_COMMON_HLSLI

// the hash, simplex noise and curl only — never amber_common.hlsli itself,
// whose FrameCB would put a second `time` into this translation unit
#include "amber_noise.hlsli"

cbuffer DesktopCB : register(b1)
{
    float time, dt, effectSpeed, solidity;               // solidity 0 swarm .. 1 faithful
    float springK, damping, curlAmp, curlScale;
    float particleSize, glowSize, disturbance, energyDecay;   // energyDecay: per-second retention
    float dstX, dstY, scale, jitter;                     // framebuffer px -> screen px; home jitter at solidity 0
    float screenW, screenH, mouseX, mouseY;
    float mouseRadius, mouseForce, shockX, shockY;
    float shockTime, lightMode, hdrBoost, audioWind;
    uint fbW, fbH, density, stride;                      // stride > 1: the sampled fallback
    uint particleCount, animStyle, reducedMotion, faithful;   // faithful: solidity 1 at native scale
    uint resetFlag, sampledW, sampledH, cursorCount;     // cursorCount: particles reserved for the pointer cluster
    float brightness, dragAmt, cursorX, cursorY;         // cursorX/Y: screen px of the local pointer
    float cursorScale, cursorW, cursorH, motion;         // cursorW/H: the shape's size inside gCursor; motion: tempo of the swarm and effects, 1 = as designed
    // The effects (docs/vnc.md, "Effects"), each 0 = off .. 1 = full. Any of
    // them on means the desktop is no longer pixel-exact: faithful is 0.
    float fxShock, fxEdge, fxHeat, fxMaterialise;
    // bornTime: when this particle buffer was (re)born, for materialise;
    // shockAmp: +1 pushes outward (a click), negative pulls in (right click);
    // edgeGain: gradient-to-glow scale for fxEdge
    // vivid: saturation and contrast, 1 = the decoded colours (forced to 1 in faithful mode)
    float bornTime, shockAmp, edgeGain, vivid;
};

// materialise runs this long after bornTime: the flight home, then exact
static const float kMaterialiseSeconds = 1.4;

struct DeskParticle
{
    float2 pos;         // screen px
    uint   velPacked;   // f16x2
    uint   extra;       // f16 seed, f16 spare
};

float2 UnpackVel(uint v)
{
    return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16));
}

uint PackVel(float2 v)
{
    return (f32tof16(v.x) & 0xFFFFu) | (f32tof16(v.y) << 16);
}

float UnpackSeed(uint e)
{
    return f16tof32(e & 0xFFFFu);
}

// Which framebuffer pixel a particle is fixed to, and where that pixel's
// centre falls on screen. Particle i belongs to sampled pixel i / density;
// the density sub-index picks a quadrant offset so four particles per pixel
// tile it rather than stacking. The framebuffer pixel is the sampled pixel
// times the stride (1 unless the desktop exceeded the budget).
void HomeOf(uint i, out uint2 srcPixel, out float2 home, out uint sub)
{
    const uint s = i / max(density, 1u);
    sub = i - s * density;
    const uint sx = s % max(sampledW, 1u);
    const uint sy = s / max(sampledW, 1u);
    srcPixel = uint2(sx * stride, sy * stride);
    float2 q = float2(0.0, 0.0);
    if (density == 2)      q = float2(sub == 0 ? -0.25 : 0.25, 0.0);
    else if (density >= 3) q = float2((sub & 1u) ? 0.25 : -0.25, (sub & 2u) ? 0.25 : -0.25);
    const float2 fbPos = float2(srcPixel) + 0.5 + q * (density > 1 ? 1.0 : 0.0);
    home = float2(dstX, dstY) + fbPos * scale;
}

// sRGB byte values to linear light, the exact curve (not the gamma-2.2
// approximation): faithful mode is judged against the decoded bytes.
float3 SrgbToLinearExact(float3 c)
{
    const float3 lo = c / 12.92;
    const float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r,
                  c.g <= 0.04045 ? lo.g : hi.g,
                  c.b <= 0.04045 ? lo.b : hi.b);
}

#endif
