// motion_fields.hlsli — the motion styles as force fields for the VNC
// particle desktop.
//
// particle_sim.hlsl applies the styles to glyph cells: each has a birth, a
// template and a cell to arrive at, and the style choreographs that arrival.
// A desktop particle has none of those — its home is a pixel that never
// changes — so the same styles are expressed here as what they are once the
// choreography is taken away: a field of acceleration over the screen, a
// function of where the particle is, where it belongs, the time, and its
// seed. The order and names are kMotionStyles in src/render/motion_styles.h.
//
// Units are screen px/s². The caller scales the whole field by (1 -
// solidity), so at solidity 1 no style moves a pixel — that is the faithful
// contract, kept in the sim, not here.
#ifndef MOTION_FIELDS_HLSLI
#define MOTION_FIELDS_HLSLI

#include "amber_noise.hlsli"

float2 Perp(float2 v) { return float2(-v.y, v.x); }

float2 StyleField(uint style, float2 pos, float2 home, float t, float seed, float2 screen)
{
    const float2 centre = screen * 0.5;
    const float2 rel = pos - centre;
    const float r = max(length(rel), 1.0);
    const float2 radial = rel / r;
    const float2 fromHome = pos - home;
    const float diag = max(length(screen), 1.0);
    const float3 np = float3(pos * 0.0025, t * 0.25);

    switch (style)
    {
    default:
    case 0:   // Direct: the spring alone
        return float2(0.0, 0.0);
    case 1:   // Nebula Twist: a slow vortex about the centre, breathing
        return Perp(radial) * (600.0 + 300.0 * sin(t * 0.6)) * saturate(r / (0.35 * diag))
             + Curl2(np) * 180.0;
    case 2:   // Digital Rain: everything falls, in columns with their own phase
    {
        const float col = floor(pos.x / 14.0);
        const float ph = HashU(uint(col) * 7919u + 3u);
        return float2(0.0, 900.0 + 500.0 * ph) + float2(0.0, 300.0 * sin(t * 3.0 + ph * 6.28));
    }
    case 3:   // Quantum Flux: incoherent jitter, everywhere
        return Curl2(float3(pos * 0.05, t * 3.0)) * 1400.0;
    case 4:   // Sonic Boom: a ring expands from the centre every few seconds
    {
        const float period = 4.0;
        const float ringR = frac(t / period) * diag * 0.6;
        const float d = abs(r - ringR);
        return radial * 5000.0 * exp(-d * d / 900.0);
    }
    case 5:   // Magnetic Assemble: pulled hard into place with an orbital lean
        return -fromHome * 40.0 + Perp(normalize(fromHome + 0.001)) * 400.0 * saturate(length(fromHome) / 40.0);
    case 6:   // Cyclone: inward spiral
        return Perp(radial) * 1200.0 - radial * 350.0;
    case 7:   // Fountain: thrown up from the bottom centre, gravity brings it back
    {
        const float2 nozzle = float2(centre.x, screen.y);
        const float2 dn = pos - nozzle;
        const float near = exp(-dot(dn, dn) / (0.02 * diag * diag));
        return float2(0.0, -6000.0) * near + float2(0.0, 900.0);
    }
    case 8:   // Glitch: horizontal slabs shove sideways for a frame at a time
    {
        const float slab = floor(pos.y / 24.0);
        const float k = HashU(uint(slab) * 131u + uint(floor(t * 8.0)) * 977u);
        return float2(k > 0.9 ? 8000.0 * (k > 0.95 ? 1.0 : -1.0) : 0.0, 0.0);
    }
    case 9:   // Slipstream: wind from the left with turbulence
        return float2(700.0, 0.0) + Curl2(np * 3.0) * 500.0;
    case 10:  // Tumble: each particle rolls about its own home
        return Perp(normalize(fromHome + float2(0.001, 0.0))) * 900.0 * saturate(length(fromHome) / 6.0)
             + Perp(float2(cos(seed * 6.28 + t * 4.0), sin(seed * 6.28 + t * 4.0))) * 200.0;
    case 11:  // Mothership (ID4): a shadow front sweeps down and presses
    {
        const float front = frac(t / 9.0) * (screen.y + 200.0) - 100.0;
        const float w = smoothstep(120.0, 0.0, abs(pos.y - front));
        return float2(0.0, 2500.0) * w + Curl2(np) * 120.0 * w;
    }
    case 12:  // Starwake: streaming outward from the centre, like a warp field
        return radial * (900.0 + 600.0 * HashU(uint(seed * 65535.0))) * saturate(r / (0.2 * diag));
    case 13:  // Gunmetal Iris: concentric rings contract and release
    {
        const float ph = sin(t * 1.2 + r * 0.02);
        return radial * ph * 1500.0;
    }
    case 14:  // Ominous Signal: slow broad waves crossing the screen
        return float2(sin(pos.y * 0.01 + t * 0.7), cos(pos.x * 0.01 - t * 0.5)) * 500.0;
    case 15:  // Hunter Vision: a scan band races across, lifting what it passes
    {
        const float band = frac(t / 2.5) * (screen.x + 200.0) - 100.0;
        const float w = smoothstep(60.0, 0.0, abs(pos.x - band));
        return float2(0.0, -1800.0) * w + Curl2(np * 2.0) * 300.0 * w;
    }
    case 16:  // Film Burn: hot spots wander and push outward
    {
        const float2 spot = centre + float2(sin(t * 0.37), cos(t * 0.29)) * diag * 0.25;
        const float2 ds = pos - spot;
        const float d2 = dot(ds, ds);
        return ds / max(sqrt(d2), 1.0) * 3000.0 * exp(-d2 / (0.01 * diag * diag));
    }
    case 17:  // Sandfall: grains settle, sliding on the way down
        return float2(0.0, 1100.0) + Curl2(np * 4.0) * 250.0;
    case 18:  // Weld: a moving point throws sparks
    {
        const float2 tip = float2(frac(t / 6.0) * screen.x, centre.y + sin(t * 2.0) * 0.2 * screen.y);
        const float2 ds = pos - tip;
        const float d2 = dot(ds, ds);
        return ds / max(sqrt(d2), 1.0) * 7000.0 * exp(-d2 / 2500.0);
    }
    case 19:  // Murmuration: flocking as coherent sinusoidal groups
    {
        const float g = floor((pos.x + pos.y) / 90.0);
        const float ph = HashU(uint(g) * 401u) * 6.28;
        return float2(cos(t * 1.5 + ph), sin(t * 1.1 + ph)) * 700.0 + Curl2(np) * 200.0;
    }
    case 20:  // Hammer: a periodic slam straight down, then a rebound
    {
        const float ph = frac(t / 1.5);
        return float2(0.0, ph < 0.15 ? 9000.0 : -1200.0 * (1.0 - ph));
    }
    case 21:  // Game of Life: cells of the screen pulse in and out of place
    {
        const float2 cell = floor(pos / 32.0);
        const float alive = step(0.5, HashU(uint(cell.x) * 73u + uint(cell.y) * 151u + uint(floor(t)) * 17u));
        return alive * -fromHome * 20.0 + (1.0 - alive) * Curl2(np * 3.0) * 900.0;
    }
    case 22:  // Surface: ripples spreading across a pool
    {
        const float wave = sin(r * 0.05 - t * 3.0);
        return radial * wave * 800.0;
    }
    }
}

#endif
