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

    // Shear plates (redraw style 6): the pixels of an eight-by-eight block
    // move as one rigid plate — a slide and a small turn about the block's
    // centre that settles — so the eye reads whole surfaces moving rather
    // than a cloud of dots. Every particle works its block's transform out
    // for itself from the block's own coordinates, so nothing has to be
    // shared between them.
    if (transition > 5.5 && transition < 6.5 && !isCursor && !materialising && !resetFlag)
    {
        const uint2 blk = src / 8u;
        const uint2 mid = blk * 8u + 4u;
        const float plateAge = time - gStamp[mid / max(stride, 1u)];
        if (plateAge >= 0.0 && plateAge < transitionSecs)
        {
            const float k = 1.0 - plateAge / transitionSecs;
            const float k2 = k * k;
            const uint h = blk.x * 73856093u ^ blk.y * 19349663u;
            const float ang = (HashU(h) - 0.5) * 0.9 * k2;
            const float2 slide = (float2(HashU(h + 7u), HashU(h + 13u)) - 0.5) * 34.0 * k2;
            const float2 centre = float2(dstX, dstY) + (float2(mid) + 0.5) * scale;
            const float2 off = home - centre;
            const float ca = cos(ang), sa = sin(ang);
            const float2 turned = float2(off.x * ca - off.y * sa, off.x * sa + off.y * ca);
            const float2 target = centre + turned * (1.0 + 0.12 * k2) + slide;
            const float step = max(dt, 1.0 / 240.0);
            p.pos = target;
            p.velPacked = PackVel((target - pos0) / step);
            gParticles[i] = p;
            return;
        }
    }

    // The region styles (7 iris, 8 sonic boom, 9 shatter): these move
    // particles with respect to the changed region as a whole, whose centre
    // and reach the CPU measured from the damage itself.
    if (transition > 6.5 && transition < 9.5 && !isCursor && !materialising && !resetFlag)
    {
        const float regionAge = time - gStamp[src / max(stride, 1u)];
        if (regionAge >= 0.0 && regionAge < transitionSecs)
        {
            const float t = regionAge / transitionSecs;
            const int style = int(transition + 0.5);
            const float2 hub = float2(dstX, dstY) + (float2(irisX, irisY) + 0.5) * scale;
            const float2 fromHub = home - hub;
            const float reach = max(length(fromHub), 1.0);
            const float2 away = fromHub / reach;
            float2 target = home;
            if (style == 7)
            {
                // Iris: the picture does not move except at the rim, where
                // the expanding front throws the particles it passes
                // outward. That is what gives the wipe a physical edge.
                const float front = (time - irisTime) * irisSpeed * scale;
                const float band = 7.0 * scale;
                const float onRim = exp(-(reach - front) * (reach - front) / (band * band));
                target = home + away * onRim * 9.0 * scale;
            }
            else if (style == 8)
            {
                // Sonic boom: every particle in the region leaves at once on
                // one expanding shell, then comes back on a curve that
                // accelerates into the landing, so they all arrive together.
                const float k = t < 0.28 ? smoothstep(0.0, 1.0, t / 0.28)
                                         : pow(1.0 - (t - 0.28) / 0.72, 2.2);
                target = home + away * k * (48.0 + 26.0 * seed) * scale;
            }
            else
            {
                // Shatter and reform: sixteen-pixel shards, each turning
                // about its own centroid, thrown out from the region's
                // centre and falling, each starting a moment after the last
                // so the break is ragged rather than a single pulse.
                const uint2 cell = src / 16u;
                const uint h = cell.x * 73856093u ^ cell.y * 19349663u;
                const float lead = HashU(h) * 0.3;
                const float u = saturate((t - lead) / max(1.0 - lead, 0.05));
                const float k = 1.0 - u;
                const float k2 = k * k;
                const float2 mid = float2(dstX, dstY) + (float2(cell * 16u + 8u) + 0.5) * scale;
                const float2 off = home - mid;
                const float ang = (HashU(h + 5u) - 0.5) * 2.4 * k2;
                const float ca = cos(ang), sa = sin(ang);
                const float2 turned = float2(off.x * ca - off.y * sa, off.x * sa + off.y * ca);
                const float2 spread = normalize(mid - hub + float2(0.001, 0.001)) * 46.0 * k2 * scale;
                target = mid + turned * (1.0 + 0.12 * k2) + spread + float2(0.0, 30.0 * k2 * k * scale);
            }
            const float step = max(dt, 1.0 / 240.0);
            p.pos = target;
            p.velPacked = PackVel((target - pos0) / step);
            gParticles[i] = p;
            return;
        }
    }

    // Light speed (redraw style 5): this pixel changed, so its particle
    // flies in from far out along a curved path and lands on it. The
    // particles themselves move; nothing resolves in place.
    float warpT = -1.0;
    if (transition > 4.5 && transition < 5.5 && !isCursor && !materialising && !resetFlag)
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
        const float spin = (1.0 - e) * (1.0 * seed - 0.5) + (1.0 - e) * (1.0 - e) * 0.45;
        const float ca = cos(spin), sa = sin(spin);
        const float2 turned = float2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
        // a short launch: the region snaps back into place rather than
        // travelling across the screen
        const float far = (110.0 + 170.0 * HashU(i * 13u + 29u)) * (1.0 - e);
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
