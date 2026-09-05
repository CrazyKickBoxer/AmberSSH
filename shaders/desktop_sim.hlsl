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
RWTexture2D<unorm float>         gInject    : register(u2);   // the energy pass's, unused here
RWTexture2D<float>               gStamp     : register(u3);   // when each pixel last changed

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
        // The pointer: a grid laid over the server's cursor shape, one
        // particle per shape pixel, placed by the shape's own hotspot — so
        // the cluster IS the cursor, not a blob near it. Past the grid, a
        // slowly turning ring that stays visible on any background.
        const uint k = i - particleCount;
        const uint g = uint(max(cursorGrid, 1.0));
        const float2 at = float2(cursorX, cursorY);
        if (k < g * g)
        {
            const float2 cell = float2(float(k % g), float(k / g));
            if (cursorW >= 1.0)
                home = at + cell * float2(cursorW, cursorH) / float(g) - float2(cursorHotX, cursorHotY);
            else
                home = at + cell;   // no server shape: the built-in arrow, drawn from the same cell
        }
        else
        {
            const uint j = k - g * g;
            const float count = max(float(cursorCount - g * g), 1.0);
            const float a = float(j) / count * 6.2831853 + time * 1.1;
            const float rr = max(max(cursorW, cursorH), 16.0) * 0.5 + 7.0;
            home = at + float2(cos(a), sin(a)) * rr;
        }
        src = uint2(0, 0);
        sub = 0;
    }
    else
        HomeOf(i, src, home, sub);

    const float2 pos0 = p.pos;   // where it was, for the velocity a flight implies
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

    // Light speed (redraw style 5): this pixel changed, so its particle
    // flies in from far out along a curved path and lands on it. The
    // particles themselves move; nothing resolves in place.
    float warpT = -1.0;
    if (transition > 4.5 && !isCursor && !materialising && !resetFlag)
    {
        const float changeAge = time - gStamp[src / max(stride, 1u)];
        if (changeAge >= 0.0 && changeAge < transitionSecs)
            warpT = changeAge / transitionSecs;
    }
    if (warpT >= 0.0)
    {
        // Out along the ray from the screen's centre, turned by the
        // particle's own seed so the swarm arrives on curves rather than
        // spokes, and decelerating hard into its pixel.
        const float e = 1.0 - pow(1.0 - warpT, 3.0);
        const float2 centre = float2(screenW, screenH) * 0.5;
        const float2 ray = home - centre;
        const float2 dir = normalize(ray + float2(1e-3, 1e-3));
        const float spin = (1.0 - e) * (2.4 * seed - 1.2) + (1.0 - e) * (1.0 - e) * 1.1;
        const float ca = cos(spin), sa = sin(spin);
        const float2 turned = float2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
        const float far = (700.0 + 900.0 * HashU(i * 13u + 29u)) * (1.0 - e);
        const float2 target = home + turned * far;
        const float step = max(dt, 1.0 / 240.0);
        p.pos = target;
        p.velPacked = PackVel((target - pos0) / step);   // the draw streaks along it
        gParticles[i] = p;
        return;
    }

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

        // The click's shockwave is a refraction of the picture, drawn in
        // desktop_draw.hlsl (ShockRefraction): the particles stay on their
        // pixels and read their colour from displaced ones. Below solidity
        // 1 the swarm still feels a soft push from it, so the two agree.
        if (shockTime >= 0.0 && loose > 0.0)
        {
            const float age = time - shockTime;
            const float2 ds = pos - float2(shockX, shockY);
            const float sd = max(length(ds), 1.0);
            shock = ds / sd * 3000.0 * shockAmp * exp(-sd * sd / 80000.0) * exp(-age * 8.0);
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
        a += (field * loose + shock * loose + burst * max(loose, fxHeat * 0.35)) * motion;

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
