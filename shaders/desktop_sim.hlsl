// desktop_sim.hlsl — one thread per desktop particle.
//
// A spring toward a home that is a framebuffer pixel's centre on screen,
// plus everything the swarm can do to it — curl-noise drift, the selected
// motion style as a force field, the pointer's push, a shockwave, and the
// disturbance energy of its own source pixel, which throws it outward when
// that pixel changes — all of which is multiplied by (1 - solidity).
//
// At solidity 1 there is no integration at all: the particle is placed on
// its home and its velocity is zero. Not "a strong spring", which would
// still leave it a fraction of a pixel out for a frame: placed. That is
// what makes the faithful desktop reproduce the framebuffer's pixels rather
// than approximate them.
//
// The last cursorCount particles are the local pointer's cluster; their home
// is the pointer's position, not a pixel, and their colour is the cursor's.
#include "desktop_common.hlsli"
#include "motion_fields.hlsli"

RWStructuredBuffer<DeskParticle> gParticles : register(u0);
RWTexture2D<float>               gEnergy    : register(u1);

// The burst direction: away from home when the particle is already off it,
// otherwise the seed's own direction, so a resting particle still leaves.
float2 fromHomeOrRandom(float2 v, float2 fallback)
{
    return dot(v, v) > 0.25 ? v : fallback;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint i = dtid.x;
    if (i >= particleCount + cursorCount)
        return;

    DeskParticle p = gParticles[i];
    const bool isCursor = i >= particleCount;
    float seed = UnpackSeed(p.extra);
    if (resetFlag)
    {
        // a fresh buffer: the seed comes from the index, the particle from home
        seed = HashU(i * 2654435761u + 7u);
        p.extra = (f32tof16(seed) & 0xFFFFu);
    }

    uint2 src;
    float2 home;
    uint sub;
    if (isCursor)
    {
        // a small cluster around the hotspot, each particle with its own spot
        const uint k = i - particleCount;
        const float a = seed * 6.2831853;
        const float rr = (0.5 + 0.5 * HashU(k * 977u + 11u)) * cursorScale;
        home = float2(cursorX, cursorY) + float2(cos(a), sin(a)) * rr;
        src = uint2(0, 0);
        sub = 0;
    }
    else
        HomeOf(i, src, home, sub);

    // A jittered home at low solidity: the swarm settles near, not on, its pixel.
    const float loose = 1.0 - solidity;
    if (!isCursor && jitter > 0.0)
    {
        const float2 j = float2(HashU(i * 3u + 1u), HashU(i * 5u + 2u)) - 0.5;
        home += j * jitter * loose;
    }

    // Materialise: for kMaterialiseSeconds after the buffer is born the
    // particles fly home from wherever they were scattered, on a soft
    // spring and nothing else; then the ordinary rules (faithful included)
    // take over and place them exactly.
    const float bornAge = time - bornTime;
    const bool materialising = fxMaterialise > 0.0 && !isCursor && bornAge >= 0.0 && bornAge < kMaterialiseSeconds;

    if (resetFlag)
    {
        p.pos = home;
        if (materialising)
            p.pos = float2(HashU(i * 7u + 3u) * screenW, HashU(i * 11u + 5u) * screenH);
        p.velPacked = PackVel(float2(0.0, 0.0));
        gParticles[i] = p;
        return;
    }
    if (faithful && !isCursor && !materialising)
    {
        p.pos = home;
        p.velPacked = PackVel(float2(0.0, 0.0));
        gParticles[i] = p;
        return;
    }

    float2 vel = UnpackVel(p.velPacked);
    float2 pos = p.pos;
    const float ts = time * effectSpeed * motion;   // the motion setting is tempo: the fields' clock and their push

    // --- forces ------------------------------------------------------------
    // At solidity 1 the spring is stiffer and better damped: an effect
    // kicks the particle out and it is back on its pixel within a few
    // frames, with no overshoot to read as a wobble.
    const float stiff = solidity >= 0.999 ? 2.0 : 1.0;
    float2 a = (home - pos) * springK * stiff - vel * damping * sqrt(stiff);

    float2 field = float2(0.0, 0.0);   // the swarm's forces, scaled by (1 - solidity)
    float2 shock = float2(0.0, 0.0);   // the click shockwave: also at solidity 1 when fxShock
    float2 burst = float2(0.0, 0.0);   // the changed-pixel burst: also at solidity 1 when fxHeat
    if (!isCursor && !materialising)
    {
        field += Curl2(float3(pos * curlScale, ts * 0.3)) * curlAmp * (60.0 + 200.0 * audioWind);
        if (!reducedMotion)
            field += StyleField(animStyle, pos, home, ts, seed, float2(screenW, screenH));

        // the pointer's push
        const float2 dm = pos - float2(mouseX, mouseY);
        const float md = length(dm);
        if (mouseForce > 0.0 && md < mouseRadius)
            field += dm / max(md, 1.0) * mouseForce * (1.0 - md / mouseRadius);

        // a shockwave: a ring leaving the click at 900 px/s, pushing (or,
        // with a negative shockAmp, pulling) what it passes, fading in 1 s
        if (shockTime >= 0.0)
        {
            const float age = time - shockTime;
            const float2 ds = pos - float2(shockX, shockY);
            const float sd = max(length(ds), 1.0);
            const float2 away = ds / sd;
            // Every style is an impulse: a hard kick over a few frames, then
            // the spring brings the particles straight back. Nothing keeps
            // rippling — a pattern that lingers reads as LCD ghosting.
            const int style = int(shockStyle + 0.5);
            const float kick = exp(-age * 9.0);   // gone in about a third of a second
            if (style == 1)
            {
                // water drop: the plop — a dimple's worth of particles thrown
                // outward from the point, hardest at the centre
                shock = away * 16000.0 * shockAmp * exp(-sd / 90.0) * kick;
            }
            else if (style == 2)
            {
                // splash: the same, leaning upward
                const float2 lean = normalize(away + float2(0.0, -0.8));
                shock = lean * 16000.0 * shockAmp * exp(-sd / 140.0) * kick;
            }
            else if (style == 3)
            {
                // vortex: one brief twist around the point
                const float2 tangent = float2(-away.y, away.x);
                shock = (tangent * 14000.0 - away * 3000.0) * shockAmp * exp(-sd / 160.0) * kick;
            }
            else
            {
                // ring: one fast wave out from the point, 2400 px/s, thin
                const float ring = age * 2400.0;
                shock = away * 14000.0 * shockAmp * exp(-(sd - ring) * (sd - ring) / 1600.0) * exp(-age * 6.0);
            }
        }

        // disturbance: this pixel changed — burst outward, with a lean the
        // seed chooses so a block of change does not explode symmetrically
        const float e = gEnergy[src / max(stride, 1u)];
        if (e > 0.001)
        {
            const float ang = seed * 6.2831853;
            const float2 dir = normalize(fromHomeOrRandom(pos - home, float2(cos(ang), sin(ang))));
            burst = dir * e * disturbance * 2500.0;
        }
    }

    if (materialising)
        a = (home - pos) * 30.0 - vel * 11.0;   // critically damped, about a second's flight
    else
        a += (field * loose + shock * max(loose, fxShock) + burst * max(loose, fxHeat * 0.35)) * motion;

    // --- integrate ------------------------------------------------------------
    const float h = min(dt, 1.0 / 30.0);
    vel += a * h;
    if (!materialising)
        vel *= lerp(0.985, 0.90, saturate(dragAmt));
    pos += vel * h;

    // stay on screen, softly
    const float2 lo = float2(-8.0, -8.0), hi = float2(screenW + 8.0, screenH + 8.0);
    pos = clamp(pos, lo, hi);

    // At solidity 1 with an effect on, a particle that has all but come home
    // is placed: the picture at rest is the picture, not a blur of
    // sub-pixel remainders.
    if (solidity >= 0.999 && !materialising)
    {
        const float2 rem = home - pos;
        if (dot(rem, rem) < 0.0025 && dot(vel, vel) < 4.0)
        {
            pos = home;
            vel = float2(0.0, 0.0);
        }
    }

    p.pos = pos;
    p.velPacked = PackVel(vel);
    gParticles[i] = p;
}
