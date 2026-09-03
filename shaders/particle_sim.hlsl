// particle_sim.hlsl — per-particle motion.
//
// Baseline behaviour (glyph transitions, curl-noise gas drift, spring-damper
// toward home, breathing brightness, cursor orbit) is AmberSSH's own.
//
// Extended after amber-particle-ssh (MIT, CrazyKickBoxer) with:
//   * a four-layer particle model  (0 diffuse glow .. 3 HDR sparkle)
//   * a radial mouse force field
//   * a shockwave impulse
//   * selectable animation styles  (twist / rain / quantum / sonic / magnetic)
//   * runtime-selectable particle density
// The reference is GLSL/OpenGL; this is an independent HLSL implementation of
// the same ideas against our own particle and cell layout.
#include "amber_common.hlsli"

RWStructuredBuffer<Particle>  gParticles : register(u0);
StructuredBuffer<CellGpu>     gCells     : register(t0);
// xy = cell-local grid-pixel position, z = antialiased coverage weight.
StructuredBuffer<float4>      gPoints    : register(t1);

float EaseOutCubic(float t) { return 1.0 - pow(1.0 - t, 3.0); }

// Point on the inset border rectangle of a cell, u in [0,1) around perimeter.
float2 BorderPoint(float2 org, float u)
{
    float inset = 1.5;
    float w = max(cellW - 2.0 * inset, 1.0);
    float h = max(cellH - 2.0 * inset, 1.0);
    float per = 2.0 * (w + h);
    float d = frac(u) * per;
    float2 p0 = org + float2(inset, inset);
    if (d < w)              return p0 + float2(d, 0);
    d -= w;
    if (d < h)              return p0 + float2(w, d);
    d -= h;
    if (d < w)              return p0 + float2(w - d, h);
    d -= w;
    return p0 + float2(0, h - d);
}

// Screensaver blend: the idle screen dissolves into digital rain — every
// glyph's particles stream down their own column at a per-column speed,
// flickering, and flow back into letters when input returns (rainMode is
// the blend, ramped by the app). Shared by the quiet path and the styled
// path so both dissolve identically.
void RainBlend(uint col, float seed, float tpx, float2 cellOrg, float weight,
               float cellBright, inout float2 home, inout float targetB)
{
    if (rainMode <= 0.001 || weight <= 0.001 || cellBright <= 0.0)
        return;
    float colSeed = HashU(col * 7919u + 3u);
    float speed = 90.0 + 160.0 * colSeed;
    float span = max(screenH - originY, 1.0);   // below the title bar
    float drop = frac((time * speed + colSeed * 4000.0 + seed * 600.0) / span);
    float2 rainHome = float2(cellOrg.x + tpx * cellW, originY + drop * span);
    home = lerp(home, rainHome, rainMode);
    float flick = 0.35 + 0.65 * step(0.55, frac(drop * 6.0 + seed * 2.0));
    targetB *= lerp(1.0, flick * 1.4, rainMode);
}

// Resolved cell color packed for the particle: bit 24 = "explicit color",
// low 24 bits = sRGB. Zero means the amber ramp.
float TintBits(CellGpu cd)
{
    uint cbits = (cd.flags & 2u) ? (0x01000000u | (cd.fgRgb & 0x00FFFFFFu)) : 0u;
    return asfloat(cbits);
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint pid = dtid.x;
    if (pid >= particleCount)
        return;

    uint ppc  = max(particlesPerCell, 1u);
    uint cell = pid / ppc;
    uint sub  = pid - cell * ppc;
    CellGpu cd = gCells[cell];

    // Quiet cell (flag bit 2): this glyph generation changed in place inside
    // a stationary full-screen app (htop, vim, top). Quiet cells take the
    // self-contained QUIET CELL path further down and RETURN there, before
    // any motion-style code runs — so present styles and future ones alike
    // can never move them. Their transitions also run ~10x faster, so a
    // refreshed value is readable at once.
    const bool quietC = (cd.flags & 4u) != 0u;
    float quickK = quietC ? 10.0 : 1.0;

    uint col = cell % cols;
    uint row = cell / cols;
    float2 cellOrg  = float2(originX + col * cellW, originY + row * cellH);
    float2 cellSize = float2(cellW, cellH);

    // Layer assignment: sub-index is strided across four layers so every layer
    // samples the whole glyph rather than one region of it.
    //   0 = large diffuse background glow   2 = small sharp detail
    //   1 = medium character body           3 = tiny HDR sparkle
    uint layer = sub & 3u;

    // Home: fly from the previous glyph's point set to the current one. The
    // template table is always stored at kMaxPPC stride.
    float  tt   = saturate((time - cd.birth) * quickK / transitionDur);
    float  ease = EaseOutCubic(tt);
    uint   slot = sub % kMaxPPC;
    float4 tp   = gPoints[cd.glyph     * kMaxPPC + slot];
    float4 pp   = gPoints[cd.prevGlyph * kMaxPPC + slot];
    float2 tgt  = cellOrg + tp.xy * cellSize;
    float2 src  = cellOrg + pp.xy * cellSize;
    // Direct morphs the home from the old glyph to the new one. The motion
    // styles do NOT: their target snaps instantly (reference behaviour), so
    // the scatter burst below leaves real distance for the style force to
    // spiral/fall/flux the letter into existence.
    float2 home = (quietC || animStyle == 0u) ? lerp(src, tgt, ease) : tgt;

    // Densities above the template stride repeat slots: give the repeats a
    // static sub-pixel offset so they plush out the stroke instead of
    // stacking invisibly on the same spot.
    if (sub >= kMaxPPC)
    {
        float jx = HashU(pid * 613u + 11u) - 0.5;
        float jy = HashU(pid * 809u + 3u) - 0.5;
        home += float2(jx, jy) * (cellSize.y / 16.0) * 1.2;
    }
    // Coverage weight from the glyph raster: this is what makes strokes read
    // like a rendered font — empty grid pixels stay dark and cost nothing.
    float  weight = tp.z;

    float seed = HashU(pid * 747796405u + 2891336453u);

    // Text layers sit exactly on the glyph pixels; only the dim layer-0 glow
    // is allowed off the skeleton (bloom provides the rest of the halo).
    float spreadPx = spreadRadius * min(cellW, cellH);
    float2 offDir  = float2(cos(seed * 6.28318530), sin(seed * 6.28318530));
    float  layerSpread = (layer == 0) ? 1.00 : 0.0;
    home += offDir * spreadPx * layerSpread * (0.35 + 0.65 * seed);

    // ------------------------------------------------------- depth parallax
    // The field is not flat: rows further UP sit further back, and scrolled
    // history sits further back still. Tilting with the pointer moves the near
    // rows more than the far ones, which is what sells the depth. Applied to
    // the HOME, so it is a property of where a cell lives rather than a force
    // — the letters stay perfectly stable when the pointer is still.
    float depth01 = 0.0;
    if (tiltX != 0.0 || tiltY != 0.0)
    {
        depth01 = 1.0 - saturate(float(row) / max(float(rows - 1u), 1.0));
        home += float2(tiltX, tiltY) * depth01;
    }

    // Target brightness: cell brightness x idle breathing x birth fade-in.
    float breathe = (1.0 - breatheAmp) + breatheAmp *
                    sin(time * 0.4 + HashU(cell * 9781u) * 6.28318530);

    // Dual sine animation (pulse + faster flicker), per the reference model.
    float pulse   = sin(time * shimmerSpeed        + seed * 6.28318530);
    float flicker = sin(time * shimmerSpeed * 3.7  + seed * 11.13);
    float twinkle = 1.0 + twinkleAmp * pulse + flickerAmp * flicker;

    // Layer weighting: coverage carries the letterform now, so layers only
    // differentiate glow (dim, wide) from body, plus a rare hot sparkle.
    float layerGain = (layer == 0) ? 0.35 : 1.0;
    if (layer == 3)
        layerGain *= 1.0 + 0.6 * step(0.85, seed);

    // Energy conservation above the template stride: N-fold slot reuse means
    // each particle carries 1/N of the light, keeping total glyph brightness
    // constant while the grain gets denser.
    float dupScale = (particlesPerCell > kMaxPPC)
                         ? float(kMaxPPC) / float(particlesPerCell)
                         : 1.0;
    float targetB = cd.bright * weight * breathe * twinkle * layerGain *
                    dupScale * (0.35 + 0.65 * ease);
    // Distance haze: the rows sitting further back read dimmer, which is the
    // other half of the parallax and the part that survives a still pointer.
    if (depth01 > 0.0)
        targetB *= lerp(0.80, 1.0, depth01);
    // Phosphor warm-up: a cold tube is dim and takes a few seconds to come to
    // temperature. Pairs with the CRT power-down when a tab closes.
    if (warmup < 0.999)
        targetB *= 0.18 + 0.82 * warmup * warmup;

    // Ahead of a reveal wave the cell stays dark; it materializes when its
    // staggered birth arrives.
    if (time < cd.birth)
        targetB = 0.0;

    // Activity heat map: a cell that just changed runs hot (the ramp goes
    // white at the top, so "brighter" reads as "hotter") and cools over a
    // few seconds — churning log regions visibly glow.
    if (heatAmp > 0.0 && cell != cursorIndex && cd.bright > 0.0)
    {
        float ageH = max(time - cd.birth, 0.0);
        targetB *= 1.0 + heatAmp * exp(-ageH / max(heatTau, 0.05));
    }

    // =====================================================================
    // QUIET CELL — a stationary app (htop, vim, top) changed this cell in
    // place. This is a complete, self-contained path that RETURNS here, so
    // NOTHING below — no present motion style and no future one — can ever
    // touch a quiet cell. Keep it that way: add style behaviour below this
    // block, never above it. Only generic, style-free behaviour lives above
    // (home morph, brightness, heat map). The cursor cell is exempt so the
    // cursor costumes still run. ShaderContractTests pins this ordering.
    // =====================================================================
    if (quietC && cell != cursorIndex)
    {
        RainBlend(col, seed, tp.x, cellOrg, weight, cd.bright, home, targetB);
        Particle q = gParticles[pid];
        if (resetFlag != 0)
        {
            q.pos = home;
            q.vel = float2(0, 0);
            q.bright = (resetFlag == 2u) ? targetB : 0.0;
            q.seed = seed;
        }
        q.lastBirth = cd.birth;          // acknowledge the change: no burst
        float hq = min(dt, 0.05);
        // ~25 ms exponential approach onto the (10x) morph: no spring, no
        // overshoot, unconditionally stable at any frame rate.
        q.pos = lerp(q.pos, home, 1.0 - exp(-hq * 40.0));
        q.vel = float2(0, 0);
        if (length(home - q.pos) < 0.35)
            q.pos = home;
        // Brightness follows at ~10x the normal rate, no phosphor lag: the
        // old value clears exactly as fast as the new one appears.
        q.bright = lerp(q.bright, targetB, 1.0 - exp(-hq * 90.0));
        q.tint = TintBits(cd);
        gParticles[pid] = q;
        return;
    }

    // Quantum Flux transit character: brief per-slice particle dropout, then
    // an emissive flash right at lock-in.
    if (animStyle == 3u && time >= cd.birth)
    {
        float ageQ = time - cd.birth;
        float ttq = ageQ / max(transitionDur * 1.6, 0.001);
        if (ttq < 0.75)
        {
            uint sliceQ = (uint)(ageQ / 0.045);
            if (HashU(pid * 613u ^ sliceQ) < 0.12)
                targetB = 0.0;                   // dropped out this slice
        }
        targetB *= 1.0 + 1.1 * saturate(1.0 - abs(ttq - 0.95) * 12.0);
    }

    // Mothership: the letter is a dark colossal mass sliding in from high
    // above — nearly black in transit (a silhouette against the sky), with
    // running lights (rare sparkle particles stay lit), then a hard flash as
    // it slams onto the cell.
    if (animStyle == 11u && time >= cd.birth && cell != cursorIndex)
    {
        float spd11 = max(effectSpeed, 0.1);
        float T11   = 1.1 / spd11;
        float age11 = time - cd.birth;
        if (age11 < T11)
        {
            float u11 = saturate(age11 / T11);
            // Dark silhouette until final approach; layer-3 sparkles stay on
            // as blinking running lights along the hull. Light modes skip the
            // fade: there the particles ARE dark ink, so a fully-inked hull
            // over the paper is already the silhouette.
            float silh = lerp(0.18, 1.0, smoothstep(0.72, 1.0, u11));
            if (layer == 3 && seed > 0.55)
            {
                float blinkM = step(0.5, frac(time * 2.2 + seed * 7.0));
                silh = max(silh, 0.9 * blinkM);
            }
            if (lightMode > 0.5)
                silh = 1.0;
            targetB *= silh;
        }
        else
        {
            // Touchdown flash + slow afterglow decay.
            targetB *= 1.0 + 1.3 * exp(-(age11 - T11) * 6.0 * spd11);
        }
    }

    // Nebula Twist: each freshly typed (or revealed) character twirls in a
    // small circle about its cell while it settles in — the whole glyph rides
    // a decaying orbital offset plus a gentle rotation about the cell center,
    // then comes to rest exactly on target so settled text stays sharp.
    if (animStyle == 1u && time >= cd.birth && cell != cursorIndex)
    {
        // Full-size circles while the letter is fresh (the collapse itself
        // takes ~0.5 s, so a front-loaded decay would never be seen), then a
        // smoothstep ease-out so the glyph parks without a pop.
        float spd    = max(effectSpeed, 0.1);
        float age1   = time - cd.birth;
        float spin   = saturate((1.8 / spd - age1) / (0.9 / spd));
        if (spin > 0.001)
        {
            spin = spin * spin * (3.0 - 2.0 * spin);
            float ph = HashU(cell * 7919u + 13u) * 6.28318530;
            float w  = time * 8.0 * effectSpeed + ph;
            float2 cc = cellOrg + cellSize * 0.5;
            home += float2(cos(w), sin(w)) * (0.28 * cellSize.y) * spin;
            float angT = 0.30 * sin(w * 0.9) * spin;
            float csT = cos(angT), snT = sin(angT);
            float2 v1 = home - cc;
            home = cc + float2(v1.x * csT - v1.y * snT,
                               v1.x * snT + v1.y * csT);
        }
    }

    // Tumble: the whole letterform spins as it falls — a kinematic rotation
    // of the home about the cell center that unwinds from up to ~1.5 turns
    // and lands with a damped wobble. It decays to exactly zero, so settled
    // text is pixel-perfect; the spring chasing the rotating home keeps the
    // glyph coherent (rigid-body tumble) all the way down.
    if (animStyle == 10u && time >= cd.birth && cell != cursorIndex)
    {
        float spd10 = max(effectSpeed, 0.1);
        float age10 = time - cd.birth;
        uint h10 = cell * 2654435761u ^ asuint(cd.birth);
        float sgn10 = (HashU(h10) > 0.5) ? 1.0 : -1.0;
        float turns = 3.14159265 + HashU(h10 * 9781u + 17u) * 6.28318530;
        float theta = sgn10 * turns * exp(-age10 * 2.8 * spd10)
                    + sgn10 * 0.30 * exp(-age10 * 3.0 * spd10) *
                      sin(age10 * 11.0 * spd10 + HashU(h10 * 31u) * 6.28318530);
        if (abs(theta) > 0.003)
        {
            float2 cc10 = cellOrg + cellSize * 0.5;
            float cs = cos(theta), sn = sin(theta);
            float2 v = home - cc10;
            home = cc10 + float2(v.x * cs - v.y * sn, v.x * sn + v.y * cs);
        }
    }

    // Mothership descent: the home is a kinematic ENLARGED copy of the
    // letterform hanging far overhead. It sinks with a slow, heavy ease
    // (nothing that big hurries), shrinking onto the cell, trembling with a
    // low engine rumble in transit, and lands with a damped shudder. The
    // spring chasing this moving home keeps the glyph rigid — one huge dark
    // ship per letter, not a particle spray.
    if (animStyle == 11u && time >= cd.birth && cell != cursorIndex)
    {
        float spd11 = max(effectSpeed, 0.1);
        float T11   = 1.1 / spd11;
        float age11 = time - cd.birth;
        float2 cc11 = cellOrg + cellSize * 0.5;
        if (age11 < T11)
        {
            float u11   = saturate(age11 / T11);
            float ease11 = u11 * u11 * (3.0 - 2.0 * u11);
            float scale11 = lerp(3.2, 1.0, ease11);
            float drop11  = (1.0 - ease11) * (220.0 + cellSize.y * 2.0);
            float2 v11 = home - cc11;
            home = cc11 + v11 * scale11;
            home.y -= drop11;
            // Engine rumble — strongest mid-descent, coherent per cell so the
            // whole hull shakes as one body.
            float amp11 = 2.2 * sin(3.14159265 * u11);
            float ph11  = float(cell) * 1.7;
            home += float2(sin(time * 37.0 + ph11),
                           0.6 * sin(time * 61.0 + ph11 * 2.3)) * amp11;
        }
        else
        {
            // Impact shudder: a heavy vertical wobble that dies out fast.
            float a11 = age11 - T11;
            home.y += 3.0 * exp(-a11 * 6.0 * spd11) *
                      sin(a11 * 30.0 * spd11);
        }
    }

    // Starwake: hyperspace arrival. The flight itself is done by the force
    // below (thrust out of a deep vanishing point, then a hard brake), so all
    // that is shaped here is light: hot while the streak is still running,
    // then one short lens-flare pulse as the letter snaps into focus.
    if (animStyle == 12u && time >= cd.birth && cell != cursorIndex)
    {
        float spd12 = max(effectSpeed, 0.1);
        float T12   = 0.55 / spd12;
        float age12 = time - cd.birth;
        float u12   = saturate(age12 / T12);
        // A whole screen of streaks converges on the vanishing point and their
        // glows stack, so the transit gain stays moderate; the punch comes
        // from the lens flare at lock-in instead, which is brief enough to
        // blow out without turning the page white.
        targetB *= lerp(1.30, 1.0, smoothstep(0.60, 1.0, u12));
        if (age12 > T12 * 0.80)
            targetB *= 1.0 + 1.8 * exp(-(age12 - T12 * 0.80) * 11.0 * spd12);
    }

    // Gunmetal Iris: the letter is drawn through a rotating rifled aperture.
    // A kinematic home carries the whole cloud: it spirals inward with the
    // radius collapsing to a single bright point at the muzzle, then fires
    // radially outward onto the letterform with a slight overshoot.
    if (animStyle == 13u && time >= cd.birth && cell != cursorIndex)
    {
        float spd13 = max(effectSpeed, 0.1);
        float T13   = 0.75 / spd13;
        float age13 = time - cd.birth;
        if (age13 < T13)
        {
            float u13  = saturate(age13 / T13);
            float ph13 = HashU(cell * 6151u + 7u) * 6.28318530;
            float sgn13 = (HashU(cell * 2179u) > 0.5) ? 1.0 : -1.0;
            float2 cc13 = cellOrg + cellSize * 0.5;
            if (u13 < 0.66)
            {
                float a13 = u13 / 0.66;                    // 0..1 through the bore
                float ang = ph13 + seed * 6.28318530 * (1.0 - a13)
                          + sgn13 * (1.0 - a13) * 11.0;    // rifling twist
                // The bore stays close to the letter's own cell. A wide one
                // reads as a single screen-sized mesh of streaks rather than
                // one aperture per character.
                float rad = (1.15 * cellSize.y + 10.0) * pow(1.0 - a13, 2.1);
                home = cc13 + float2(cos(ang), sin(ang)) * rad;
                // Late, sharp brightening: the charge concentrates into one
                // hot muzzle point instead of glowing through the whole run.
                targetB *= lerp(0.30, 3.2, a13 * a13 * a13);
            }
            else
            {
                float b13 = (u13 - 0.66) / 0.34;
                float e13 = 1.0 - pow(1.0 - b13, 2.8);
                home = lerp(cc13, home, e13 * 1.20);       // fire out, overshoot
                targetB *= lerp(3.2, 1.0, saturate(b13 * 1.5));
            }
        }
    }

    // Ominous Signal: nothing flies. The letter arrives as disconnected
    // strokes and isolated points that materialize on an uneven staircase of
    // hashed intervals, so fragments land at unsettling times rather than on a
    // metre; at the end the whole glyph is suddenly obvious and locks in.
    if (animStyle == 14u && time >= cd.birth && cell != cursorIndex)
    {
        float spd14 = max(effectSpeed, 0.1);
        float T14   = 1.10 / spd14;
        float age14 = time - cd.birth;
        float u14   = saturate(age14 / T14);
        if (u14 < 1.0)
        {
            // Four chunky steps, not six small ones: each arrival is a visible
            // event, and the hashed edges spread wide enough that the gaps
            // between them are plainly uneven.
            float lvl = 0.0;
            [unroll]
            for (uint s14 = 0u; s14 < 4u; ++s14)
            {
                float edge = (float(s14) + 0.15 +
                              0.85 * HashU(cell * 977u + s14 * 31u)) / 4.0;
                lvl += (u14 > edge) ? 0.25 : 0.0;
            }
            float rp = HashU(pid * 1237u ^ asuint(cd.birth));
            if (rp > lvl + 0.02)
                targetB = 0.0;                     // this fragment is not here yet
            else
                targetB *= 0.28 + 0.72 * step(0.5, frac(time * 9.0 + rp * 7.0));
            // Fragments that have arrived are not quite still until lock-in.
            uint jt14 = (uint)(time * 7.0);
            home += (float2(HashU(pid * 71u ^ jt14), HashU(pid * 131u ^ jt14)) - 0.5)
                    * 3.4 * (1.0 - u14);
        }
        else
            targetB *= 1.0 + 1.7 * exp(-(age14 - T14) * 7.0 * spd14);
    }

    // Hunter Vision: machine sight. A horizontal scanner measures the cell,
    // targeting lines triangulate each destination, a skeletal wireframe of
    // the letter appears (its own antialiased rim, which the coverage weight
    // gives us for free), and only then does the glyph fill with hot
    // particles. Diagnostic sparks scatter about and wink out at lock-in.
    if (animStyle == 15u && time >= cd.birth && cell != cursorIndex)
    {
        float spd15 = max(effectSpeed, 0.1);
        float T15   = 1.00 / spd15;
        float age15 = time - cd.birth;
        float u15   = saturate(age15 / T15);
        if (u15 < 1.0)
        {
            float spark = HashU(pid * 3323u ^ asuint(cd.birth));
            if (u15 < 0.25)
            {
                // The measuring sweep uses every particle, including the ones
                // off the letterform — that is what draws the full-width line.
                home = cellOrg + float2(tp.x * cellSize.x,
                                        (u15 / 0.25) * cellSize.y);
                targetB = max(targetB,
                              cd.bright * dupScale * layerGain * 0.45);
            }
            else if (u15 < 0.56)
            {
                // Targeting lines run out to their destinations with a hot
                // leading tip; the line behind the tip stays instrument-dim.
                float b15  = (u15 - 0.25) / 0.31;
                float lead = saturate(b15 * 1.6 - spark * 0.6);
                float2 from = cellOrg + float2(tp.x * cellSize.x, cellSize.y);
                home = lerp(from, home, lead);
                targetB *= ((spark < 0.5) ? 0.62 : 0.30) *
                           (1.0 + 3.0 * saturate(1.0 - abs(lead - 0.85) * 8.0));
            }
            else if (u15 < 0.86)
            {
                if (weight > 0.82)
                    targetB *= 0.015;              // interior stays unlit
                else
                    targetB *= 2.3;                // the rim is the wireframe
            }
            if (spark > 0.88)
            {
                home += (float2(HashU(pid * 17u), HashU(pid * 37u)) - 0.5) *
                        cellSize * 2.4 * (1.0 - u15);
                targetB *= 1.0 + 3.4 * (1.0 - u15);
            }
        }
    }

    // Film Burn: projector leader. The character strobes through two or three
    // misregistered gate positions, then catches from one edge as a burn front
    // crosses it — blown out at the front, unexposed ahead of it — and the
    // overexposure decays away as dust and scratches evaporate.
    if (animStyle == 16u && time >= cd.birth && cell != cursorIndex)
    {
        float spd16 = max(effectSpeed, 0.1);
        float T16   = 0.85 / spd16;
        float age16 = time - cd.birth;
        float u16   = saturate(age16 / T16);
        float2 cc16 = cellOrg + cellSize * 0.5;
        if (u16 < 0.34)
        {
            // Gate jump: nearly a whole cell of misregistration, so the ghost
            // impressions are clearly separate frames rather than a soft blur.
            uint fr16 = (uint)(age16 * 24.0 * spd16);
            home += (float2(HashU(fr16 * 7919u ^ cell),
                            HashU(fr16 * 104729u ^ cell)) - 0.5) *
                    float2(cellSize.x * 0.95, cellSize.y * 0.70);
            targetB *= 0.15 + 0.75 * HashU(fr16 * 331u ^ (cell * 17u));
        }
        else if (u16 < 0.78)
        {
            float b16   = (u16 - 0.34) / 0.44;
            float ang16 = HashU(cell * 8161u + 5u) * 6.28318530;
            float2 bd   = float2(cos(ang16), sin(ang16));
            float proj  = dot((home - cc16) / max(cellSize.y, 1.0), bd) * 0.5 + 0.5;
            float d16   = (b16 * 1.5 - 0.25) - proj;
            if (d16 < -0.04)
                targetB = 0.0;                             // not yet alight
            else
                targetB *= 1.0 + 5.2 * exp(-abs(d16) * 9.0);
        }
        else
            targetB *= 1.0 + 2.0 * exp(-(u16 - 0.78) * 9.0);
        float dust = HashU(pid * 4441u ^ asuint(cd.birth));
        if (dust > 0.88 && u16 < 1.0)
        {
            home = cellOrg + float2(HashU(cell * 61u + (uint)(dust * 97.0)) * cellSize.x,
                                    frac(dust * 31.0) * cellSize.y);
            targetB *= 3.2 * (1.0 - u16);
        }
    }

    // Sandfall: grains pour from the top of the cell and pile up into the
    // letter. The pile is built by giving each particle its own release time,
    // ordered by how LOW in the glyph it sits — the bottom of the letterform
    // fills first, which is what makes it read as accumulation rather than a
    // curtain dropping.
    if (animStyle == 17u && time >= cd.birth && cell != cursorIndex)
    {
        float spd17 = max(effectSpeed, 0.1);
        float T17   = 0.80 / spd17;
        float age17 = time - cd.birth;
        float u17   = saturate(age17 / T17);
        float depth = 1.0 - tp.y;                   // 1 at the cell's floor
        float release = depth * 0.55 + HashU(pid * 271u) * 0.30;
        float fall = saturate((u17 - release) / 0.34);
        if (fall <= 0.0)
            targetB = 0.0;                          // still up in the hopper
        else
        {
            // Pour down the column it will end in, with a little sideways
            // trickle that dies as the grain lands.
            float ease = fall * fall;
            home.y = lerp(cellOrg.y - cellSize.y * 0.35, home.y, ease);
            home.x += sin(age17 * 9.0 + seed * 12.0) * 1.4 * (1.0 - ease);
            targetB *= 0.45 + 0.55 * ease;
        }
    }

    // Weld: sparks arc in, the glyph runs white hot, then cools to amber.
    if (animStyle == 18u && time >= cd.birth && cell != cursorIndex)
    {
        float spd18 = max(effectSpeed, 0.1);
        float T18   = 0.55 / spd18;
        float age18 = time - cd.birth;
        float u18   = saturate(age18 / T18);
        if (u18 < 0.45)
        {
            // Spark shower: a few particles run ahead of the arc, thrown off
            // the weld line and burning out.
            if (HashU(pid * 6353u ^ asuint(cd.birth)) > 0.86)
            {
                float b18 = u18 / 0.45;
                float ang = HashU(pid * 97u) * 6.28318530;
                home += float2(cos(ang), sin(ang)) * (1.0 - b18) *
                        cellSize.y * 1.1;
                targetB *= 2.4 * (1.0 - b18);
            }
            else
                targetB *= 0.30 + 1.10 * u18;
        }
        else
        {
            // White hot at the moment of fusion, cooling over about a second.
            float cool = saturate((u18 - 0.45) / 0.55);
            targetB *= lerp(2.9, 1.0, cool * cool);
        }
    }

    // Murmuration: the cloud flocks before it settles. Each particle carries a
    // heading shared with its neighbours (a per-cell phase plus a slow spatial
    // term), which is what makes the swarm turn together instead of each grain
    // wandering on its own — the cheap half of a boids model, and the half
    // that actually reads at this scale.
    if (animStyle == 19u && time >= cd.birth && cell != cursorIndex)
    {
        float spd19 = max(effectSpeed, 0.1);
        float T19   = 0.95 / spd19;
        float age19 = time - cd.birth;
        float u19   = saturate(age19 / T19);
        if (u19 < 1.0)
        {
            float flock = (1.0 - u19) * (1.0 - u19);
            float ph19 = HashU(cell * 3121u) * 6.28318530;
            float t19 = age19 * 2.6 * spd19 + ph19;
            // A shared wheeling heading, plus a small per-particle offset so
            // the flock has width.
            float head = sin(t19) * 1.9 + cos(t19 * 0.6) * 1.1 +
                         HashU(pid * 53u) * 0.9;
            home += float2(cos(head), sin(head) * 0.6) * flock * cellSize.y * 0.65;
            targetB *= 0.55 + 0.45 * (1.0 - flock);
        }
    }

    // Hammer: the letter is struck from behind and splats onto the cell. It
    // arrives as an over-inked blot that contracts to the letterform, which is
    // why it belongs with the dot-matrix faces.
    if (animStyle == 20u && time >= cd.birth && cell != cursorIndex)
    {
        float spd20 = max(effectSpeed, 0.1);
        float T20   = 0.30 / spd20;
        float age20 = time - cd.birth;
        float u20   = saturate(age20 / T20);
        float2 cc20 = cellOrg + cellSize * 0.5;
        if (u20 < 1.0)
        {
            // Splat: the ink spreads past the glyph on impact and pulls back.
            float spl = sin(u20 * 3.14159265);
            float2 v20 = home - cc20;
            home = cc20 + v20 * (1.0 + spl * 0.85);
            // Ragged edge, so it reads as ink rather than a scale animation.
            float ang20 = HashU(pid * 811u) * 6.28318530;
            home += float2(cos(ang20), sin(ang20)) * spl * 2.6;
            targetB *= 1.0 + 1.5 * spl;
        }
        // Recoil: a short vertical shudder after the strike.
        float shake = exp(-max(age20 - T20, 0.0) * 22.0 * spd20);
        home.y += sin(age20 * 60.0 * spd20) * 1.3 * shake;
    }

    // Game of Life: the cell runs a few generations of Conway's automaton and
    // the glyph is what survives. Each generation is evaluated on the fly from
    // the glyph's own coverage grid, so the letter is always the attractor.
    if (animStyle == 21u && time >= cd.birth && cell != cursorIndex)
    {
        float spd21 = max(effectSpeed, 0.1);
        float T21   = 0.85 / spd21;
        float age21 = time - cd.birth;
        float u21   = saturate(age21 / T21);
        if (u21 < 1.0)
        {
            // Six generations; a particle is alive when its own hash falls
            // under a threshold that walks from noise towards the letterform.
            uint gen = (uint)(u21 * 6.0);
            float noise = HashU(pid * 1783u ^ (gen * 2654435761u));
            // Early generations are mostly random; later ones keep the cells
            // the glyph actually covers.
            float bias = saturate(u21 * 1.5);
            float alive = lerp(noise, weight > 0.35 ? 1.0 : 0.0, bias);
            if (alive < 0.45)
                targetB = 0.0;
            else
                targetB *= 0.55 + 0.45 * bias;
            // Cells snap between grid positions rather than sliding.
            float2 jitter = float2(HashU(pid * 31u ^ gen) - 0.5,
                                   HashU(pid * 71u ^ gen) - 0.5);
            home += jitter * cellSize * (1.0 - bias) * 0.40;
        }
    }

    // Surface: the letter rises through water and breaks the meniscus, with a
    // ring spreading from the point it comes through.
    if (animStyle == 22u && time >= cd.birth && cell != cursorIndex)
    {
        float spd22 = max(effectSpeed, 0.1);
        float T22   = 0.70 / spd22;
        float age22 = time - cd.birth;
        float u22   = saturate(age22 / T22);
        if (u22 < 1.0)
        {
            float ease = 1.0 - pow(1.0 - u22, 2.2);
            // Rise from below the cell.
            home.y += (1.0 - ease) * cellSize.y * 1.5;
            // Refraction wobble while still submerged, dying at the surface.
            float sub = 1.0 - ease;
            home.x += sin(home.y * 0.35 + age22 * 7.0) * 2.2 * sub;
            targetB *= 0.35 + 0.65 * ease;
        }
        else
        {
            // The ripple: a bright ring expanding from the letter's middle for
            // a moment after it surfaces.
            float2 cc22 = cellOrg + cellSize * 0.5;
            float rAge = (age22 - T22) * spd22;
            if (rAge < 0.45)
            {
                float rad = rAge * cellSize.y * 3.4;
                float d = abs(length(home - cc22) - rad);
                targetB *= 1.0 + 1.6 * exp(-d * 0.9) * (1.0 - rAge / 0.45);
            }
        }
    }

    // Cursor cell: each motion style dresses the cursor in its own costume.
    // Translating loops (orbits, drizzle, funnels) ride cursorPhase, which
    // only advances while the cursor travels — so they freeze when it parks.
    // In-place pulses (throbs, flicker) may run on raw time, like the blink.
    if (cell == cursorIndex)
    {
        float2 ctr = cellOrg + cellSize * 0.5;
        float su = seed + float(sub) / float(ppc);
        float u = su + cursorPhase;
        home = BorderPoint(cellOrg, u);    // Direct: the classic orbiting rect
        if (animStyle == 1u)
        {
            // Nebula: a swirling gas ring that breathes gently.
            float a = su * 6.28318530 + cursorPhase * 12.566;
            float rr = 0.34 * min(cellW, cellH) *
                       (1.0 + 0.14 * sin(time * 2.6 + seed * 6.28318530));
            home = ctr + float2(cos(a) * 1.15, sin(a)) * rr;
        }
        else if (animStyle == 2u)
        {
            // Digital Rain: code drizzle — droplets stream down the cell
            // while the cursor travels, freezing mid-fall when it parks.
            float col2 = frac(seed * 7.31);
            float fall = frac(seed + cursorPhase * 8.0);
            home = cellOrg + float2(col2 * cellW,
                                    fall * (cellH - 2.0) + 1.0);
        }
        else if (animStyle == 3u)
        {
            // Quantum: hologram rect — border positions snap between
            // quantized slots, and slices of the outline drop out below.
            float uq = floor(u * 14.0) / 14.0;
            home = BorderPoint(cellOrg, uq);
        }
        else if (animStyle == 4u)
        {
            // Sonic: a subwoofer thump — the outline punches inward on a
            // beat, rings past the rim, and settles until the next hit.
            float cyc = frac(time * 0.9 * effectSpeed);
            float sc = 1.0 - 0.45 * exp(-cyc * 10.0)
                     + 0.12 * exp(-cyc * 4.0) * sin(cyc * 22.0);
            home = ctr + (home - ctr) * sc;
        }
        else if (animStyle == 5u)
        {
            // Magnetic: the outline breathes like a charged coil — each
            // filing drawn toward the core and released on its own phase.
            float throb = 0.5 + 0.5 * sin(time * 3.5 * effectSpeed +
                                          seed * 2.2);
            home = lerp(home, ctr, 0.35 * throb);
        }
        else if (animStyle == 6u)
        {
            // Cyclone: a tiny spinning funnel — wide rim at the top, tip at
            // the baseline; the spin freezes with the cursor.
            float v6 = frac(su);
            float a6 = seed * 6.28318530 + cursorPhase * 12.566 + v6 * 4.0;
            float r6 = lerp(0.46 * cellW, 0.08 * cellW, v6);
            home = cellOrg + float2(cellW * 0.5 + cos(a6) * r6,
                                    (0.08 + 0.84 * v6) * cellH);
        }
        else if (animStyle == 7u)
        {
            // Fountain: droplets climb the middle and fan out at the crest.
            float rise = frac(seed + cursorPhase * 8.0);
            float fan = (frac(seed * 9.17) - 0.5) *
                        (0.15 + 0.85 * rise) * cellW;
            home = cellOrg + float2(cellW * 0.5 + fan,
                                    cellH - rise * (cellH - 2.0) - 1.0);
        }
        else if (animStyle == 8u)
        {
            // Glitch: three scan bars; they shear sideways only while the
            // cursor is on the move, and flicker per-bar below.
            float bar = floor(frac(seed * 5.13) * 3.0);
            float x8 = frac(seed * 11.7) * (cellW - 2.0) + 1.0;
            float tear = (HashU((uint)bar * 977u +
                                (uint)(cursorPhase * 40.0)) - 0.5) *
                         6.0 * cursorActivity;
            home = cellOrg + float2(x8 + tear, (0.20 + 0.30 * bar) * cellH);
        }
        else if (animStyle == 9u)
        {
            // Slipstream: a tight beam when parked that sweeps out into a
            // trailing wind streak while the cursor travels.
            float t9 = frac(seed * 3.77);
            float beamY = (0.10 + 0.80 * frac(seed * 6.13)) * cellH;
            float trail = t9 * t9 * cellW * 2.2 * cursorActivity;
            home = cellOrg + float2(cellW * 0.72 - trail, beamY);
        }
        else if (animStyle == 10u)
        {
            // Tumble: the box rocks side to side while the cursor travels
            // and rights itself smoothly once parked.
            float ang = sin(cursorPhase * 40.0) * 0.55 * cursorActivity;
            float cs10 = cos(ang), sn10 = sin(ang);
            float2 v10 = home - ctr;
            home = ctr + float2(v10.x * cs10 - v10.y * sn10,
                                v10.x * sn10 + v10.y * cs10);
        }
        else if (animStyle == 11u)
        {
            // Mothership: a flattened saucer hovering over the baseline with
            // a slow bob and a faint tractor-beam of droplets underneath.
            float a11 = su * 6.28318530 + cursorPhase * 6.28;
            float bob = sin(time * 2.1) * 1.5;
            if (frac(seed * 5.71) < 0.25)
            {
                // Beam: particles stream from the hull down to the baseline.
                float fall11 = frac(seed + time * 0.9);
                home = cellOrg + float2(cellW * (0.30 + 0.40 * frac(seed * 9.3)),
                                        cellH * (0.30 + 0.62 * fall11) + bob);
            }
            else
                home = ctr + float2(cos(a11) * 0.55 * cellW,
                                    sin(a11) * 0.16 * cellH - cellH * 0.22 + bob);
        }
        else if (animStyle == 12u)
        {
            // Starwake: a distant star that stretches into a four-point flare
            // while the cursor travels and contracts back to the crisp
            // rectangle the moment it parks.
            float arm = floor(frac(seed * 4.13) * 4.0);
            float2 dirA = (arm < 1.5) ? float2((arm < 0.5) ? 1.0 : -1.0, 0.0)
                                      : float2(0.0, (arm < 2.5) ? 1.0 : -1.0);
            float along = frac(su * 7.7);
            float2 flare = ctr + dirA * (along * along * min(cellW, cellH) * 1.60);
            home = lerp(home, flare, saturate(cursorActivity));
        }
        else if (animStyle == 13u)
        {
            // Gunmetal Iris: a rotating circular aperture. The rifling turns
            // only while the cursor travels; the sweeping highlight and the
            // impact flash are brightness, applied below.
            float a13 = su * 6.28318530 + cursorPhase * 14.0;
            home = ctr + float2(cos(a13), sin(a13)) * (0.46 * min(cellW, cellH));
        }
        else if (animStyle == 14u)
        {
            // Ominous Signal: four separated corner marks that creep inward,
            // plus a faint vertical signal line through the middle.
            if (frac(seed * 5.71) < 0.18)
                home = cellOrg + float2(cellW * 0.5, frac(su * 11.3) * cellH);
            else
            {
                float conv = 0.5 + 0.5 * sin(time * 1.6 * effectSpeed);
                uint c14 = (uint)(frac(seed * 3.37) * 4.0);
                float sx = ((c14 & 1u) != 0u) ? 1.0 : -1.0;
                float sy = ((c14 & 2u) != 0u) ? 1.0 : -1.0;
                float2 corner = ctr + float2(sx * (cellW * 0.5 - 2.0),
                                             sy * (cellH * 0.5 - 2.0));
                float leg = frac(su * 9.1) * 0.34;
                float2 armv = (frac(seed * 7.7) < 0.5)
                                  ? float2(-sx * leg * cellW, 0.0)
                                  : float2(0.0, -sy * leg * cellH);
                home = lerp(corner + armv, ctr, 0.30 * conv);
            }
        }
        else if (animStyle == 15u)
        {
            // Hunter Vision: angular targeting brackets, a scanning line that
            // travels with the cursor, and a small centre reticle.
            float r15 = frac(seed * 5.19);
            if (r15 < 0.18)
                home = cellOrg + float2(frac(su * 13.7) * cellW,
                                        frac(cursorPhase * 3.0) * cellH);
            else if (r15 < 0.28)
                home = ctr + (float2(frac(seed * 23.1), frac(seed * 31.7)) - 0.5) *
                             min(cellW, cellH) * 0.16;
            else
            {
                uint c15 = (uint)(frac(seed * 3.91) * 4.0);
                float sx = ((c15 & 1u) != 0u) ? 1.0 : -1.0;
                float sy = ((c15 & 2u) != 0u) ? 1.0 : -1.0;
                float2 corner = ctr + float2(sx * (cellW * 0.5 - 1.5),
                                             sy * (cellH * 0.5 - 1.5));
                float leg = frac(su * 7.3) * 0.38;
                home = corner + ((frac(seed * 11.9) < 0.5)
                                     ? float2(-sx * leg * cellW, 0.0)
                                     : float2(0.0, -sy * leg * cellH));
            }
        }
        else if (animStyle == 16u)
        {
            // Film Burn: a projection frame that jitters in the gate on a
            // 24 fps tick, with three sprocket holes down the left edge.
            uint fr16 = (uint)(time * 24.0 * max(effectSpeed, 0.1));
            if (frac(seed * 4.37) < 0.16)
            {
                float sp = floor(frac(su * 6.1) * 3.0);
                home = cellOrg + float2(1.5, (0.22 + 0.28 * sp) * cellH);
            }
            home += (float2(HashU(fr16 * 7919u), HashU(fr16 * 104729u)) - 0.5) *
                    2.4 * (0.35 + cursorActivity);
        }
        else if (animStyle == 17u)
        {
            // Sandfall: a heap that drains and refills as the cursor travels.
            float col17 = frac(seed * 5.13);
            float pile = 1.0 - abs(col17 - 0.5) * 1.7;      // heaped middle
            float drain = frac(seed + cursorPhase * 5.0);
            home = cellOrg + float2(col17 * cellW,
                                    cellH - pile * cellH * 0.55 * drain - 1.5);
        }
        else if (animStyle == 18u)
        {
            // Weld: a bright arc point with sparks thrown off it.
            float a18 = su * 6.28318530 + cursorPhase * 7.0;
            if (frac(seed * 3.91) < 0.30)
                home = ctr + float2(cos(a18), sin(a18)) *
                             min(cellW, cellH) * (0.15 + 0.5 * frac(seed * 9.1));
            else
                home = ctr + (float2(frac(seed * 17.3), frac(seed * 23.7)) - 0.5) *
                             min(cellW, cellH) * 0.22;
        }
        else if (animStyle == 19u)
        {
            // Murmuration: a small flock wheeling around the cell.
            float t19 = time * 1.8 * effectSpeed + seed * 2.0;
            float head = sin(t19) * 2.0 + cos(t19 * 0.6) * 1.2;
            home = ctr + float2(cos(head), sin(head) * 0.55) *
                         min(cellW, cellH) * (0.22 + 0.20 * frac(seed * 7.7));
        }
        else if (animStyle == 20u)
        {
            // Hammer: a solid block that recoils on every advance.
            float rec = exp(-frac(cursorPhase * 6.0) * 9.0) * cursorActivity;
            home = lerp(home, ctr, 0.45);
            home.y += rec * 2.5;
        }
        else if (animStyle == 21u)
        {
            // Game of Life: a live 4x4 grid that ticks over.
            uint g21 = (uint)(time * 6.0);
            uint cellIdx = (uint)(frac(seed * 13.7) * 16.0);
            if (HashU(cellIdx * 7919u ^ g21) > 0.45)
            {
                float gx = float(cellIdx % 4u), gy = float(cellIdx / 4u);
                home = cellOrg + float2((gx + 0.5) * cellW * 0.25,
                                        (gy + 0.5) * cellH * 0.25) +
                       float2(cellW * 0.375, cellH * 0.375);
            }
            else
                home = ctr;
        }
        else if (animStyle == 22u)
        {
            // Surface: a waterline across the cell with a bobbing meniscus.
            float wave = sin(su * 6.28318530 * 2.0 + time * 3.0) * 1.6;
            home = cellOrg + float2(frac(su * 11.3) * cellW,
                                    cellH * 0.55 + wave);
        }
        // Appearance page: underline / vertical-bar cursors replace the style
        // costume with a simple shape, and blinking can be switched off.
        if (cursorShape > 1.5)
            home = cellOrg + float2(1.5, frac(u) * (cellH - 3.0) + 1.5);         // bar
        else if (cursorShape > 0.5)
            home = cellOrg + float2(frac(u) * (cellW - 3.0) + 1.5, cellH - 2.0); // underline
        float blink = (cursorBlink > 0.5) ? 0.60 + 0.40 * (0.5 + 0.5 * sin(time * 5.4))
                                          : 1.0;
        if (animStyle == 3u)
        {
            // Quantum slice dropout: brightness-only, so it is park-safe.
            uint qs = (uint)(time * 18.0);
            if (HashU(pid * 419u ^ qs) < 0.22)
                blink *= 0.25;
        }
        else if (animStyle == 8u)
        {
            // Glitch bar flicker: whole bars dim on their own ticks.
            uint bar8 = (uint)(frac(seed * 5.13) * 3.0);
            uint t8 = (uint)(time * 13.0);
            if (HashU(bar8 * 6151u ^ t8) < 0.30)
                blink *= 0.35;
        }
        else if (animStyle == 12u)
        {
            // Starwake: the flare's tips burn hotter the further they stretch.
            blink *= 1.0 + 2.0 * frac(su * 7.7) * saturate(cursorActivity);
        }
        else if (animStyle == 13u)
        {
            // Iris: a highlight sweeps the aperture, plus a muzzle flash on
            // the frames right after the cursor advances.
            float swp = frac(su + cursorPhase * 1.5);
            blink *= 0.35 + 2.10 * pow(1.0 - abs(swp - 0.5) * 2.0, 4.0);
            blink *= 1.0 + 1.4 * saturate(cursorActivity);
        }
        else if (animStyle == 14u)
        {
            // Ominous Signal: an uneasy pulse travels the centre line, and
            // corner marks drop out at irregular intervals.
            blink *= 0.28 + 0.72 * pow(0.5 + 0.5 * sin(time * 3.0 * effectSpeed), 3.0);
            if (HashU((uint)(time * 5.0) ^ (uint)(frac(seed * 3.37) * 4.0) * 7717u) < 0.22)
                blink *= 0.30;
        }
        else if (animStyle == 15u)
        {
            // Hunter Vision: the reticle throbs, the scan line runs hot.
            float r15b = frac(seed * 5.19);
            if (r15b < 0.18)
                blink *= 2.4;
            else if (r15b < 0.28)
                blink *= 0.4 + 1.8 * (0.5 + 0.5 * sin(time * 6.0 * effectSpeed));
        }
        else if (animStyle == 16u)
        {
            // Film Burn: sprocket flicker, and the left edge stays warm.
            uint fr16b = (uint)(time * 24.0 * max(effectSpeed, 0.1));
            if (HashU(fr16b * 31u) < 0.18)
                blink *= 0.28;
            blink *= 1.0 + 1.8 * saturate(1.0 - (home.x - cellOrg.x) / max(cellW * 0.35, 1.0));
        }
        // Latency ghosting: part of the cursor smears back along the typing
        // direction, the smear length set by the measured round-trip time —
        // a laggy link literally leaves phosphor behind.
        if (ghostAmp > 0.001)
        {
            float g = frac(seed * 3.1);
            home.x -= g * g * ghostAmp * cellW * 3.0;
            blink *= 1.0 - 0.7 * g * ghostAmp;
        }
        targetB = max(targetB, cursorBright * blink);
    }

    // Screensaver: the idle screen dissolves into digital rain (RainBlend).
    if (cell != cursorIndex)
        RainBlend(col, seed, tp.x, cellOrg, weight, cd.bright, home, targetB);

    Particle p = gParticles[pid];
    if (resetFlag != 0)
    {
        // resetFlag 2 re-seeds in place at the cell's current brightness (a
        // density step is invisible); 1 is a fresh start that fades in.
        p.pos = home;
        p.vel = float2(0, 0);
        p.bright = (resetFlag == 2u) ? targetB : 0.0;
        p.seed = seed;
        p.lastBirth = cd.birth;
    }
    else if (p.lastBirth != cd.birth && time >= cd.birth)
    {
        // This cell's glyph just changed — react exactly once per particle,
        // per change (never continuously, never re-fired). Births can sit in
        // the future during a bulk-repaint reveal wave; the scatter waits for
        // the wave to arrive. Reference model: scatter the particle away and
        // let the style force materialize the new letter; erased letters
        // scatter outward and dissolve.
        p.lastBirth = cd.birth;
        if (scatterSuppress < 0.5 && cell != cursorIndex)
        {
            // Per-particle randomness is seeded from particle id + the cell's
            // birth stamp (its visual generation) — deterministic and stable.
            // Per-CELL randomness keeps a glyph's cloud coherent in flight.
            float r1 = HashU(pid * 331u ^ asuint(cd.birth));
            float r2 = HashU(pid * 977u ^ asuint(cd.birth));
            uint  ch = cell * 2654435761u ^ asuint(cd.birth);
            float rc1 = HashU(ch);
            float rc2 = HashU(ch * 9781u + 17u);

            if (animStyle == 0u)        // Direct: tiny entrance scatter for
            {                           // fresh glyphs only; morphs keep pos.
                if (cd.prevGlyph == 0u)
                {
                    float ang = r2 * 6.28318530;
                    p.pos += float2(cos(ang), sin(ang)) * (6.0 + r1 * 12.0);
                }
            }
            else if (animStyle == 1u)   // Nebula Twist: 45-80 px collapse,
            {                           // tight enough to hug the letter
                float ang = r2 * 6.28318530;
                p.pos += float2(cos(ang), sin(ang)) * (45.0 + r1 * 35.0);
            }
            else if (animStyle == 2u)   // Digital Rain: fall in from above,
            {                           // tight to the letter's own column
                p.pos.y -= 160.0 + r1 * 160.0;
                p.pos.x += (r2 - 0.5) * 14.0;
            }
            else if (animStyle == 3u)   // Quantum Flux: local scatter,
            {                           // the slice-jumps do the rest.
                p.pos += (float2(r1, r2) - 0.5) * 90.0;
            }
            else if (animStyle == 4u)   // Sonic Boom: spring run-up opposite
            {                           // the text-flow direction, long
                                        // enough for the whoosh to read.
                p.pos.x -= 20.0 + r1 * 40.0;
                p.pos.y += (r2 - 0.5) * 8.0;
            }
            else if (animStyle == 5u)   // Magnetic: per-letter dipole cloud —
            {                           // half the filings at each pole of a
                                        // cell-oriented axis, fanned across it
                float axisA = rc1 * 6.28318530;
                float2 ax = float2(cos(axisA), sin(axisA));
                float2 pr = float2(-ax.y, ax.x);
                float sgn = (r2 > 0.5) ? 1.0 : -1.0;
                float r3 = HashU(pid * 1543u ^ asuint(cd.birth));
                p.pos += ax * sgn * (50.0 + 40.0 * r1)
                       + pr * (r3 - 0.5) * 64.0;
            }
            else if (animStyle == 6u)   // Cyclone: the letter spins up in its
            {                           // OWN funnel — the glyph's points are
                                        // rotated coherently around the cell
                                        // and pushed out into a vortex ring.
                float2 cc6 = cellOrg + cellSize * 0.5;
                float sgn = (rc1 > 0.5) ? 1.0 : -1.0;
                float arc = radians(120.0 + rc2 * 180.0) * sgn;
                float cs = cos(arc), sn = sin(arc);
                float2 v = tgt - cc6;
                float2 rot = float2(v.x * cs - v.y * sn, v.x * sn + v.y * cs);
                float2 outward = rot / max(length(rot), 1.0);
                p.pos = cc6 + rot + outward * (34.0 + r1 * 30.0);
            }
            else if (animStyle == 7u)   // Fountain: rise in from below,
            {                           // tight to the letter's own column
                p.pos.y += 140.0 + r1 * 180.0;
                p.pos.x += (r2 - 0.5) * 36.0;
            }
            else if (animStyle == 8u)   // Glitch: local offset scatter; the
            {                           // per-letter tears do the rest.
                p.pos += (float2(r1, r2) - 0.5) * float2(80.0, 44.0);
            }
            else if (animStyle == 9u)   // Slipstream: a localized dash from
            {                           // the nearer side, per-cell variation.
                float sgn = (tgt.x < screenW * 0.5) ? -1.0 : 1.0;
                if (rc1 > 0.75)
                    sgn = -sgn;
                p.pos.x += sgn * (110.0 + r1 * 170.0);
                p.pos.y += (r2 - 0.5) * 20.0;
            }
            else if (animStyle == 10u)  // Tumble: the letter starts as a
            {                           // coherent ROTATED copy of itself up
                                        // above and falls spinning onto its
                                        // cell (rotation unwinds in transit).
                float2 cc10 = cellOrg + cellSize * 0.5;
                float sgn10 = (rc1 > 0.5) ? 1.0 : -1.0;
                float turns = 3.14159265 + rc2 * 6.28318530;
                float cs = cos(sgn10 * turns), sn = sin(sgn10 * turns);
                float2 v = tgt - cc10;
                p.pos = cc10 + float2(v.x * cs - v.y * sn,
                                      v.x * sn + v.y * cs);
                p.pos.y -= 110.0 + rc2 * 90.0;
                p.pos.x += (rc1 - 0.5) * 30.0;
                p.pos += (float2(r1, r2) - 0.5) * 6.0;   // slight fray
            }
            else if (animStyle == 11u)  // Mothership: spawn ON the colossal
            {                           // silhouette overhead (matches the
                                        // kinematic home at u=0) so frame one
                                        // already reads as one giant dark ship.
                float2 cc11 = cellOrg + cellSize * 0.5;
                float2 v = tgt - cc11;
                p.pos = cc11 + v * 3.2;
                p.pos.y -= 220.0 + cellSize.y * 2.0;
                p.pos += (float2(r1, r2) - 0.5) * 10.0;  // hull haze
            }
            else if (animStyle == 12u)  // Starwake: collapse toward a deep
            {                           // vanishing point so the thrust below
                                        // has real distance to accelerate out
                                        // of. Travel is capped near the middle
                                        // of the screen, where the radial is
                                        // short, so no cell over-flies.
                float2 vp = float2(screenW * 0.5, screenH * 0.5);
                float2 v12 = tgt - vp;
                float l12 = length(v12);
                float keep = (l12 > 1.0) ? clamp(1.0 - 420.0 / l12, 0.04, 0.92)
                                         : 0.04;
                p.pos = vp + v12 * keep + (float2(r1, r2) - 0.5) * 14.0;
            }
            else if (animStyle == 13u)  // Gunmetal Iris: start out on the
            {                           // aperture ring; the kinematic home
                                        // owns the spiral from here.
                float2 cc13 = cellOrg + cellSize * 0.5;
                float a13 = (rc1 + r1 * 0.15) * 6.28318530;
                p.pos = cc13 + float2(cos(a13), sin(a13)) *
                               (1.15 * cellSize.y + 10.0);
            }
            else if (animStyle == 14u)  // Ominous Signal: fragments materialize
            {                           // where they belong. Almost no drift —
                                        // the dread is in the timing, not
                                        // in the travel.
                p.pos += (float2(r1, r2) - 0.5) * 5.0;
            }
            else if (animStyle == 15u)  // Hunter Vision: everything begins on
            {                           // the scanner's line at the cell's top
                                        // edge, ready to be measured.
                p.pos = cellOrg + float2(tp.x * cellSize.x, 0.0) +
                        (float2(r1, r2) - 0.5) * 3.0;
            }
            else if (animStyle == 16u)  // Film Burn: one misregistered gate
            {                           // position, coherent per letter.
                p.pos += (float2(rc1, rc2) - 0.5) *
                         float2(cellSize.x * 0.9, cellSize.y * 0.6);
            }
            else if (animStyle == 17u)  // Sandfall: start in the hopper above
            {                           // the cell, in the column it lands in.
                p.pos.y = cellOrg.y - cellSize.y * (0.4 + r1 * 1.2);
                p.pos.x = cellOrg.x + tp.x * cellSize.x + (r2 - 0.5) * 4.0;
            }
            else if (animStyle == 18u)  // Weld: everything converges on one
            {                           // arc point, so the join is a seam.
                float2 cc18 = cellOrg + cellSize * 0.5;
                p.pos = cc18 + (float2(r1, r2) - 0.5) * 6.0;
            }
            else if (animStyle == 19u)  // Murmuration: the flock arrives from
            {                           // one direction, together.
                float a19 = rc1 * 6.28318530;
                p.pos += float2(cos(a19), sin(a19)) * (70.0 + rc2 * 90.0) +
                         (float2(r1, r2) - 0.5) * 26.0;
            }
            else if (animStyle == 20u)  // Hammer: struck from behind — the
            {                           // ink starts compressed at the centre.
                float2 cc20 = cellOrg + cellSize * 0.5;
                p.pos = cc20 + (tgt - cc20) * 0.15 + (float2(r1, r2) - 0.5) * 3.0;
            }
            else if (animStyle == 21u)  // Game of Life: a random soup across
            {                           // the cell for generation zero.
                p.pos = cellOrg + float2(r1, r2) * cellSize;
            }
            else if (animStyle == 22u)  // Surface: from below the waterline.
            {
                p.pos.y += cellSize.y * (1.2 + r1 * 0.9);
                p.pos.x += (r2 - 0.5) * 8.0;
            }
        }
    }

    // Fast path: a parked particle on a blank cell. Skips the curl noise
    // (4 simplex evaluations) and the integrator entirely — on a mostly-empty
    // screen this is the bulk of every dispatch, and it is what keeps high
    // density ceilings at 60 fps on midrange GPUs. The draw pass already emits
    // a degenerate quad for it, so it rasterizes nothing either.
    if ((cd.bright <= 0.0 || weight <= 0.001) && p.bright < 0.004 &&
        cell != cursorIndex && shockTime < 0.0 && animStyle == 0u)
    {
        p.bright = 0.0;
        p.vel = float2(0, 0);
        gParticles[pid] = p;
        return;
    }

    // Living nixie-gas drift (Direct style only; the reference styles run on
    // exact targets). The glow layer carries the full gas motion, the text
    // layers barely move so resting glyphs stay sharp.
    float2 target = home;
    if (animStyle == 0u)
    {
        float curlScale = (layer == 0) ? 1.0 : 0.10;
        float3 np = float3(home * noiseScale, time * noiseSpeed + p.seed * 17.0);
        target += Curl2(np) * curlAmp * curlScale * (1.0 + audioWind * 5.0);
    }

    float h = min(dt, 0.05);               // reference clamp: never exceed 50 ms

    // ------------------------------------------------------- mouse force field
    // Radial push with smooth falloff. Particles are never displaced
    // permanently: the spring always wins once the pointer leaves.
    float2 extAccel = float2(0, 0);
    if (mouseForce != 0.0 && mouseRadius > 0.0)
    {
        float2 d = p.pos - float2(mouseX, mouseY);
        float  distM = length(d) + 1e-3;
        if (distM < mouseRadius)
        {
            float falloff = 1.0 - distM / mouseRadius;
            extAccel += (d / distM) * falloff * falloff * mouseForce;
        }
    }

    // Audio-reactive wind: system audio level drives a swirling gust field
    // that every style's integrator feels; the springs always win, so the
    // text breathes with the music instead of dissolving.
    if (audioWind > 0.001)
    {
        float2 gust = float2(sin(time * 2.7 + p.pos.y * 0.012 + seed * 6.28318530),
                             cos(time * 1.9 + p.pos.x * 0.009 + seed * 3.1));
        extAccel += gust * audioWind * 1400.0;
    }

    // A departing particle is one whose cell has gone dark while it is still
    // lit. It must be taken OFF the spring first: the integrator would drag it
    // back to the blank cell and, worse, the snap-when-close branch would pin
    // it there every frame, so no exit would ever be visible.
    const bool departing = departStyle > 0.5 && cell != cursorIndex && !quietC &&
                           cd.bright * weight <= 0.001 && p.bright > 0.012;
    if (departing)
    {
        // Nothing here: the departure block below owns this particle's motion.
    }
    else if (animStyle == 0u)
    {
        // Direct: our framerate-independent spring-damper with gas drift.
        float k0 = max(springK, 1.0) * 4.0;
        float2 accel = k0 * (target - p.pos) - damping * p.vel + extAccel;
        p.vel += accel * h;
        p.pos += p.vel * h;
        if (length(target - p.pos) < 0.35)
        {
            p.pos = target;
            p.vel *= 0.5;
        }
    }
    else
    {
        // ------------------------------------------------------------------
        // Motion styles: a faithful port of amber-particle-ssh's
        // particle_compute.comp — same integrator (multiplicative drag),
        // same constants, same snap-at-1px behaviour.
        // ------------------------------------------------------------------
        float2 diffT = target - p.pos;
        float  dist  = length(diffT);
        float  k     = max(springK, 1.0) * 4.0;
        float  dragMul = lerp(0.96, 0.60, saturate(dragAmt));
        // Magnetic: damping ramps up sharply near the target so the swarm
        // oscillation dies instead of wobbling forever.
        if (animStyle == 5u)
            dragMul = lerp(0.55, dragMul, saturate(dist / 40.0));
        // Starwake brakes violently at the end of the run — that hard stop is
        // what turns a streak into a letter.
        else if (animStyle == 12u)
            dragMul = lerp(0.34, dragMul, saturate(dist / 34.0));
        // Styles driven by a kinematic home (aperture path, scanner path, gate
        // jumps) need the cloud to sit down on that path without ringing.
        else if (animStyle == 13u || animStyle == 15u || animStyle == 16u)
            dragMul = lerp(0.62, dragMul, saturate(dist / 30.0));

        // Sonic Boom erase signature: the dying glyph collapses inward for
        // ~60 ms, then bursts outward while it fades.
        if (animStyle == 4u && cd.bright * weight <= 0.001 && p.bright > 0.01)
        {
            float dieAge = max(time - cd.birth, 0.0);
            float2 cc = cellOrg + cellSize * 0.5;
            float2 dcc = p.pos - cc;
            float lcc = max(length(dcc), 1.0);
            if (dieAge < 0.06)
                p.vel -= (dcc / lcc) * 2600.0 * h;      // collapse
            else if (dieAge < 0.14)
                p.vel += (dcc / lcc) * 5200.0 * h;      // burst
        }

        if (dist > 1.0)
        {
            float2 accel = diffT * k;

            if (animStyle == 1u)               // NEBULA TWIST
            {
                // Localized spiral. Two changes vs the raw reference force
                // (tangent * k * dist * 1.5, which slingshots particles into
                // screen-crossing orbits): the pump is capped at ~2.5 cells of
                // leverage, and it decays with age so it stops injecting
                // energy — a capped pump against a distance-proportional
                // spring otherwise settles into a perpetual limit-cycle orbit
                // instead of converging. Spiral hard for the first few
                // hundred ms, then the spring reels the swirl straight in.
                float age1t = max(time - cd.birth, 0.0);
                float pump = 1.5 * exp(-age1t * 2.5 * effectSpeed);
                float2 dir = diffT / dist;
                float2 tangent = float2(-dir.y, dir.x);
                accel += tangent * (k * min(dist, 55.0)) * pump;
                if (dist > 110.0)
                    accel += diffT * k * 2.5;
            }
            else if (animStyle == 2u)          // DIGITAL RAIN
            {
                if (p.pos.y < target.y)
                {
                    accel.y += 1000.0 * effectSpeed;         // gravity down
                    accel.x = (target.x - p.pos.x) * k * 0.5; // weak column spring
                }
                // else: standard spring snaps it into place
            }
            else if (animStyle == 3u)          // QUANTUM FLUX
            {
                // Discrete anchor jumps in fixed time slices: each slice tick
                // the particle teleports a deterministic fraction closer,
                // rather than gliding. Seeded by slice index, not frame.
                float age3 = max(time - cd.birth, 0.0);
                uint slice = (uint)(age3 / 0.045);
                if (fmod(age3, 0.045) < h)
                {
                    float jf = 0.25 + 0.25 * HashU(pid ^ (slice * 747796405u));
                    p.pos += diffT * jf;
                }
                // Light chaotic acceleration between jumps.
                float rx = HashU((pid * 7919u) ^ slice) - 0.5;
                float ry = HashU((pid * 104729u) ^ slice) - 0.5;
                accel += float2(rx, ry) * k * 2.5;
            }
            else if (animStyle == 4u)          // SONIC BOOM
            {
                // Per-letter crack: a hard forward punch right after birth
                // slams the glyph into place with a visible whoosh (the
                // keystroke shockwave below stays the signature move).
                float age4 = max(time - cd.birth, 0.0);
                accel += (diffT / dist) * k * 90.0 *
                         exp(-age4 * 8.0 * effectSpeed);
            }
            else if (animStyle == 5u)          // MAGNETIC ASSEMBLE
            {
                // Per-letter field, not a global one: every cell gets its own
                // field phase, filings shimmy across their pull direction, and
                // the attraction sharpens magnet-like as they close — the
                // glyph snaps together instead of easing in. The shimmy fades
                // inside ~1.5 cells so locked text stays rock solid.
                float ph5 = HashU(cell * 5077u) * 6.28318530;
                float tt5 = time * 5.0 * effectSpeed + ph5;
                float2 dir5 = diffT / dist;
                float2 pr5 = float2(-dir5.y, dir5.x);
                float sway = sin(p.pos.y * 0.22 + tt5)
                           + cos(p.pos.x * 0.22 + tt5 * 1.3);
                accel += pr5 * sway * k * 0.9 * saturate(dist / 30.0);
                accel += dir5 * k * min(4000.0 / max(dist, 10.0), 220.0);
            }
            else if (animStyle == 6u)          // CYCLONE
            {
                // Per-letter vortex: the whole cloud whirls around its OWN
                // cell center — harder than Nebula's per-particle spiral, so
                // the letterform visibly rotates as a funnel while it
                // tightens. The pump decays with age (same convergence guard
                // as Nebula: a sustained pump never converges) and a leash
                // keeps strays within a few cells.
                float2 cc6 = cellOrg + cellSize * 0.5;
                float age6 = max(time - cd.birth, 0.0);
                float pump6 = 2.0 * exp(-age6 * 2.6 * effectSpeed);
                float2 dC = p.pos - cc6;
                float lenC = max(length(dC), 1.0);
                float2 tanC = float2(-dC.y, dC.x) / lenC;
                accel += tanC * (k * min(dist, 50.0)) * pump6;
                if (dist > 90.0)
                    accel += diffT * k * 3.0;
            }
            else if (animStyle == 7u)          // FOUNTAIN (inverse rain)
            {
                if (p.pos.y > target.y)
                {
                    accel.y -= 1000.0 * effectSpeed;          // buoyancy up
                    accel.x = (target.x - p.pos.x) * k * 0.5; // weak column spring
                }
            }
            else if (animStyle == 8u)          // GLITCH (CRT scanline tears)
            {
                // Horizontal tears by SCAN BAND, quantized to digital steps,
                // fading to nothing as the glyph closes on its target so
                // settled text is perfectly stable.
                float fade8 = saturate(dist / 40.0);   // -> 0 near target
                uint band = (uint)(max(p.pos.y, 0.0) / 6.0);
                uint tick = (uint)(time * 24.0);
                // The tear hash includes the CELL, so every letter tears on
                // its own schedule instead of whole screen rows shearing.
                float rb = HashU(band * 2654435761u ^ tick ^ (cell * 5987u));
                if (rb > 0.70)
                {
                    float step4 = floor((rb - 0.70) * 20.0) * 4.0 + 3.0; // 3-24 px
                    float sgn = (HashU(band ^ (tick * 31u) ^ (cell * 271u)) > 0.5)
                                    ? 1.0 : -1.0;
                    p.pos.x += sgn * step4 * fade8;
                }
                accel.x += (HashU(asuint(p.pos.y) ^ (pid * 2654435761u)) - 0.5)
                           * k * 3.0 * fade8;
            }
            else if (animStyle == 9u)          // SLIPSTREAM
            {
                // Anisotropic spring: letters streak in horizontally.
                accel.x = diffT.x * k * 2.0;
                accel.y = diffT.y * k * 0.6;
            }
            else if (animStyle == 10u)         // TUMBLE
            {
                // Gravity while airborne above the (spinning) home, with a
                // weak column spring; the full spring takes over on landing.
                if (p.pos.y < target.y - 2.0)
                {
                    accel.y += 1300.0 * effectSpeed;
                    accel.x = (target.x - p.pos.x) * k * 0.7;
                }
            }
            else if (animStyle == 12u)         // STARWAKE
            {
                // Hyperspace run. While the particle is still out in the dark
                // it is not spring-driven at all: it accelerates ballistically
                // along the flight line, so the velocity streak LENGTHENS on
                // approach instead of easing off. A distance-proportional
                // spring would instead be strongest at the start and would
                // cross the whole run in a couple of frames — and, having no
                // time base, would ignore Motion Speed entirely. This one is
                // scaled by speed squared, which is what makes the run take a
                // fixed number of Motion Speed seconds. The drag ramp above
                // is the brake that turns the streak back into a letter.
                if (dist > 6.0)
                {
                    accel = (diffT / dist) * 7600.0 * effectSpeed * effectSpeed;
                    accel += diffT * k * 0.05;   // weak guidance, holds the line
                }
            }
            else if (animStyle == 13u)         // GUNMETAL IRIS
            {
                // Stiff follow: the aperture path is choreography, so the
                // cloud must ride it as one body rather than lag behind it.
                accel += diffT * k * 3.0;
            }
            else if (animStyle == 14u)         // OMINOUS SIGNAL
            {
                // Fragments lock where they appear; they never fly.
                accel += diffT * k * 2.0;
            }
            else if (animStyle == 15u)         // HUNTER VISION
            {
                // Follow the scanner and the targeting lines exactly.
                accel += diffT * k * 2.6;
            }
            else if (animStyle == 16u)         // FILM BURN
            {
                // A film frame does not slide between gate positions, it
                // jumps: snap onto the misregistered home for the duration of
                // the flicker, then let the spring carry the burn and settle.
                float age16 = max(time - cd.birth, 0.0);
                if (age16 < (0.34 * 0.85) / max(effectSpeed, 0.1))
                {
                    p.pos = target;
                    p.vel = float2(0, 0);
                    accel = float2(0, 0);
                }
                else
                    accel += diffT * k * 3.0;
            }
            else if (animStyle == 17u)         // SANDFALL
            {
                // Grains fall under gravity until they reach their place in
                // the pile, then the spring holds them there.
                if (p.pos.y < target.y)
                {
                    accel.y += 900.0 * effectSpeed;
                    accel.x = (target.x - p.pos.x) * k * 0.4;
                }
            }
            else if (animStyle == 18u)         // WELD
            {
                // Fast, stiff, slightly overshooting: metal pulled together.
                accel += diffT * k * 3.4;
            }
            else if (animStyle == 19u)         // MURMURATION
            {
                // A soft spring so the kinematic flocking above is what the
                // eye reads; a stiff one would straighten every path.
                accel += diffT * k * 0.35;
            }
            else if (animStyle == 20u ||       // HAMMER, LIFE, SURFACE all run
                     animStyle == 21u ||       // on kinematic homes; the
                     animStyle == 22u)         // spring only has to follow.
            {
                accel += diffT * k * 2.8;
            }

            p.vel += (accel + extAccel) * h;
            p.vel *= dragMul;
            p.pos += p.vel * h;
        }
        else
        {
            // Snap when close — eliminates all oscillation.
            p.pos = target;
            p.vel = float2(0, 0);
            if (animStyle == 3u)
            {
                // Unstable-hologram jitter: strong right after lock-in,
                // decayed to nothing ~400 ms later. Settled text is stable.
                float settleAge = max(time - cd.birth - transitionDur, 0.0);
                float jAmp = 0.55 * saturate(1.0 - settleAge / 0.4);
                if (jAmp > 0.01 && HashU(asuint(time) ^ pid) > 0.9)
                    p.pos.x += (HashU(pid ^ (asuint(time) * 1664525u)) - 0.5)
                               * 2.0 * jAmp;
            }
        }
    }

    // ------------------------------------------------------------- shockwave
    // Typing-event ring (Backspace/Delete/Enter). Spec'd geometry: expands to
    // ~300 px over ~0.38 s (speed-scaled), a tight 10 px gaussian ring, local
    // influence only, with a weaker secondary ring 75 ms behind the first.
    if (shockTime >= 0.0)
    {
        float2 dToWave = p.pos - float2(shockX, shockY);
        float distToCenter = length(dToWave) + 1e-3;
        float2 dir = dToWave / distToCenter;
        float dur = 0.38 / max(effectSpeed, 0.1);

        [unroll]
        for (int ring = 0; ring < 2; ++ring)
        {
            float waveAge = time - shockTime - (ring == 0 ? 0.0 : 0.075);
            if (waveAge <= 0.0 || waveAge >= dur)
                continue;
            float tw = waveAge / dur;
            float waveRadius = tw * 300.0;
            float ringDist = distToCenter - waveRadius;
            float ringMask = exp(-(ringDist * ringDist) / (2.0 * 10.0 * 10.0));
            float atten = saturate(1.0 - distToCenter / 320.0);
            float strength = (1.0 - tw) * 9000.0 * (ring == 0 ? 1.0 : 0.5);
            p.vel += dir * ringMask * atten * strength * h;
            if (ringMask > 0.3)
                p.pos += dir * 2.0 * ringMask;   // break static status
        }
    }

    // ---------------------------------------------------------- window slosh
    // The frame moved; the field has mass and does not. The impulse is applied
    // to POSITION, not velocity: the particles are simply left behind where
    // the window used to be, and their own springs haul them back in, which is
    // exactly the lag-and-catch-up of a wobbly window.
    if (sloshX != 0.0 || sloshY != 0.0)
    {
        // Cells further from the top-left drag more, so the field skews before
        // it straightens.
        float lag = 0.35 + 0.65 * saturate(float(row) / max(float(rows - 1u), 1.0));
        p.pos -= float2(sloshX, sloshY) * lag;
    }

    // ---------------------------------------------------------- departures
    // Until now text simply faded when it was erased. A departure style gives
    // the leaving letter its own exit: the cell has gone dark (cd.bright is 0)
    // but the particle is still lit, which is exactly the window in which it
    // can be thrown somewhere. cd.birth is the moment of erasure, so it doubles
    // as the age of the departure.
    if (departing)
    {
        const float dAge = max(time - cd.birth, 0.0) * max(effectSpeed, 0.1);
        const float dr1 = HashU(pid * 5171u ^ asuint(cd.birth));
        const float dr2 = HashU(pid * 9187u ^ asuint(cd.birth));
        const float hh2 = min(dt, 0.05);
        if (departStyle < 1.5)
        {
            // ASH — the letter burns, breaks up and falls, then settles in a
            // drift at the foot of the screen before it goes out. The floor is
            // what makes it read as ash rather than as another fade.
            const float floorY = screenH - 3.0 - dr1 * 6.0;
            if (p.pos.y < floorY)
            {
                p.vel.y += 260.0 * hh2 * (0.6 + dr2);
                p.vel.x += (sin(time * 2.0 + dr1 * 30.0)) * 26.0 * hh2;
                p.pos += p.vel * hh2;
            }
            else
            {
                p.pos.y = floorY;
                p.vel *= 0.80;
            }
            // Glow hot at the break, cool through ember, then out.
            float heat = exp(-dAge * 2.2);
            p.bright = max(p.bright, min(1.6, targetB + 0.9) * heat);
        }
        else if (departStyle < 2.5)
        {
            // SMOKE — dissolves upward, spreading and thinning.
            p.vel.y -= 95.0 * hh2;
            p.vel.x += sin(p.pos.y * 0.05 + time * 1.7 + dr1 * 20.0) * 42.0 * hh2;
            p.pos += p.vel * hh2;
            p.bright *= 1.0 - hh2 * 1.4;
        }
        else if (departStyle < 3.5)
        {
            // SAND — sinks straight down, quickly, without the ember.
            p.vel.y += 420.0 * hh2 * (0.7 + dr1 * 0.6);
            p.vel.x *= 0.94;
            p.pos += p.vel * hh2;
        }
        else
        {
            // SHATTER — an outward burst from the letter's middle, with the
            // shards tumbling and dimming as they scatter.
            float2 cc = cellOrg + cellSize * 0.5;
            float2 d = p.pos - cc;
            float ld = max(length(d), 1.0);
            if (dAge < 0.05)
                p.vel += (d / ld) * (900.0 + dr1 * 1400.0) * hh2 * 6.0;
            p.vel.y += 300.0 * hh2;          // shards fall as they fly
            p.pos += p.vel * hh2;
            p.bright *= 1.0 - hh2 * 1.1;
        }
    }

    // ------------------------------------------------------------ panel push
    // An opaque overlay evicts the field from its rectangle. This runs AFTER
    // the integrator, as a positional projection rather than a force: a force
    // would fight the spring that is pulling each particle back to its cell
    // and the two would buzz against each other forever. Projecting instead
    // means particles simply cannot be inside the panel, and because each one
    // leaves by its own nearest edge they pile up along the outside of it —
    // the text gathers around the box. When the panel closes the springs pull
    // everything home on their own.
    if (panelW > 0.0)
    {
        float2 lo = float2(panelX, panelY) - panelMargin;
        float2 hi = float2(panelX + panelW, panelY + panelH) + panelMargin;
        if (p.pos.x > lo.x && p.pos.x < hi.x && p.pos.y > lo.y && p.pos.y < hi.y)
        {
            float dl = p.pos.x - lo.x;
            float dr = hi.x - p.pos.x;
            float dt = p.pos.y - lo.y;
            float db = hi.y - p.pos.y;
            float m = min(min(dl, dr), min(dt, db));
            // Two per-particle offsets: one out from the edge and one ALONG
            // it. Without the second, every character on a line lands on the
            // same point and the crowd reads as one hot blob instead of a
            // gathering of displaced text.
            float spread = HashU(pid * 883u + 7u) * 7.0;
            float slide = (HashU(pid * 331u + 19u) - 0.5) * 9.0;
            if (m == dl)      { p.pos.x = lo.x - spread; p.pos.y += slide; }
            else if (m == dr) { p.pos.x = hi.x + spread; p.pos.y += slide; }
            else if (m == dt) { p.pos.y = lo.y - spread; p.pos.x += slide; }
            else              { p.pos.y = hi.y + spread; p.pos.x += slide; }
            p.vel *= 0.35;            // bleed off the speed they arrived with
        }
    }

    // Smooth brightness follow (fade-in on birth, fade-out to parked on
    // blank). In the motion styles an erased letter dissolves slowly, so the
    // scatter burst stays visible while it drifts into the nebula.
    float kb;
    if (departing)
        // Slow enough that the exit is watchable; ash lingers longest so the
        // drift at the foot of the screen has time to be seen.
        kb = 1.0 - exp(-h * (departStyle < 1.5 ? 0.55 : 1.3));
    else if (animStyle != 0u && targetB < 0.01 && p.bright > targetB)
        kb = 1.0 - exp(-h * 3.0);
    else
        kb = 1.0 - exp(-h * 9.0);
    // Phosphor persistence: light rises fast but decays slowly, like a P39
    // tube — erased text lingers as afterglow for a beat. Not inside a
    // stationary app: its old values must clear as fast as they change.
    if (phosphorDecay > 0.0 && targetB < p.bright)
        kb = 1.0 - exp(-h / phosphorDecay);
    p.bright = lerp(p.bright, targetB, kb);

    // Resolved cell color rides with the particle (TintBits). Selection
    // contrast colors arrive through the same path, resolved on the CPU
    // against the gradient.
    p.tint = TintBits(cd);

    gParticles[pid] = p;
}
