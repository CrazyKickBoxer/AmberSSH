// desktop_draw.hlsl — every desktop particle as an instanced quad, coloured
// from the framebuffer pixel it belongs to.
//
// The colour is fetched here, every frame, from the particle's fixed source
// pixel — never stored on the particle. Wherever the swarm has carried a
// particle, it shows the colour of the pixel it will come home to, so the
// picture reassembles itself as the particles settle and no stale colour
// can survive an update.
//
// Two blend states exist because faithful mode is an exact contract:
// "over" with alpha 1, one particle per pixel at native scale, so the scene
// holds the framebuffer's linear colour and nothing is summed. The swarm
// draws additively, sized and softened by (1 - solidity), so overlap glows
// the way the terminal's field does. Which PSO is bound is the C++ side's
// decision (faithful, or light mode's paper ground); the shader reads the
// same constants either way.
#include "desktop_common.hlsli"

StructuredBuffer<DeskParticle> gParticles : register(t0);
Texture2D<float4>              gFrame     : register(t1);   // B8G8R8A8_UNORM: sRGB-encoded bytes
Texture2D<float4>              gCursor    : register(t2);   // the pointer's shape, premultiplied alpha
Texture2D<float>               gEnergy    : register(t3);   // disturbance energy, sampled resolution
Texture2D<float4>              gPrev      : register(t4);   // each pixel before its last change
Texture2D<float>               gStamp     : register(t5);   // when it changed (seconds), sampled resolution

// The redraw transitions: how a changed pixel goes from what it was to
// what it is, over transitionSecs, by t in 0..1 and the particle's seed.
// Each ends exactly on the new colour; a value above 1 mid-way is meant to
// bloom for a moment (burn's flame, the scan line, the emboss relief).
float3 Redraw(int style, float t, float3 oldc, float3 newc, float seed, float edge, float2 fp)
{
    if (style == 1)
    {
        // burn: heat to a flame, char to near-black, then the new picture
        // appears through a ragged front the seed decides
        const float3 flame = float3(1.7, 0.6, 0.12);
        const float front = 0.5 + 0.45 * seed;
        if (t < 0.22)
            return lerp(oldc, flame, t / 0.22);
        if (t < front)
            return lerp(flame, float3(0.02, 0.006, 0.0), saturate((t - 0.22) / max(front - 0.22, 0.01)));
        return newc;
    }
    if (style == 2)
        return t > seed ? newc : oldc;   // dissolve: each pixel flips at its own moment
    if (style == 3)
    {
        // scan wipe: a bright line sweeps down each 48-row band, the new
        // picture behind it
        const float band = frac(fp.y / 48.0);
        if (abs(t - band) < 0.03)
            return float3(1.5, 1.5, 1.5);
        return t > band ? newc : oldc;
    }
    if (style == 4)
    {
        // emboss flash: the new region as its edges alone, a bright relief,
        // then the flat colour floods in behind them
        const float3 relief = edge.xxx * 2.2;
        if (t < 0.3)
            return relief;
        return lerp(relief, newc, (t - 0.3) / 0.7);
    }
    // 5 (light speed) recolours nothing: the particle itself is flying in
    // (desktop_sim.hlsl), and the streak below is what shows.
    return newc;
}

// The luminance of the desktop directly under the pointer, so the cursor
// can pick colours that stand against it — white on white is why a plain
// cluster disappears.
float PointerLuma()
{
    const float2 fp = (float2(cursorX, cursorY) - float2(dstX, dstY)) / max(scale, 1e-3);
    const int2 q = clamp(int2(fp), int2(0, 0), int2(int(fbW) - 1, int(fbH) - 1));
    return dot(gFrame.Load(int3(q, 0)).rgb, float3(0.299, 0.587, 0.114));
}

// The built-in pointer, when the server sends no shape: the classic arrow
// as a triangle from (0,0) to (0,14) to (10,10) in cell space. 0 outside,
// 1 inside, 2 on the outline — an outline is what makes a cursor readable
// on a background of any colour.
int ArrowAt(float2 q)
{
    const float d1 = q.x;
    const float d2 = (140.0 - 4.0 * q.x - 10.0 * q.y) / 10.7703;
    const float d3 = (q.y - q.x) / 1.4142136;
    if (d1 < -1.2 || d2 < -1.2 || d3 < -1.2)
        return 0;
    return min(d1, min(d2, d3)) < 1.4 ? 2 : 1;
}

// The shockwave as a refraction: a particle stays on its pixel and takes
// its colour from a displaced source pixel, so the picture itself ripples
// — the way a surface under water does — with no gap and no drawn edge,
// because nothing moves except where the colour is read from. Returns the
// displacement in framebuffer pixels for a pixel at fb position `fp`.
// Every style is brief: gone in under half a second.
float2 ShockRefraction(float2 fp)
{
    if (shockTime < 0.0 || fxShock <= 0.0)
        return float2(0.0, 0.0);
    const float age = time - shockTime;
    if (age < 0.0 || age > 0.6)
        return float2(0.0, 0.0);
    // the click, in framebuffer pixels
    const float2 centre = (float2(shockX, shockY) - float2(dstX, dstY)) / max(scale, 1e-3);
    const float2 ds = fp - centre;
    const float sd = max(length(ds), 1.0);
    const float2 away = ds / sd;
    const float amp = 18.0 * shockAmp * fxShock;   // pixels of displacement at full strength
    const int style = int(shockStyle + 0.5);
    if (style == 1)
    {
        // water drop: concentric ripples running outward and dying fast
        const float wave = sin(sd * 0.09 - age * 34.0);
        return away * wave * amp * exp(-sd / 260.0) * exp(-age * 6.0);
    }
    if (style == 2)
    {
        // splash: a bulge that leans upward
        const float2 lean = normalize(away + float2(0.0, -0.8));
        return lean * amp * 1.4 * exp(-sd * sd / 50000.0) * exp(-age * 8.0);
    }
    if (style == 3)
    {
        // vortex: a swirl that unwinds
        const float2 tangent = float2(-away.y, away.x);
        return tangent * amp * 1.6 * exp(-sd * sd / 70000.0) * exp(-age * 6.0);
    }
    // ring: one wave travelling out at 1400 px/s, a refractive crest and trough
    const float ring = age * 1400.0;
    const float band = exp(-(sd - ring) * (sd - ring) / 9000.0);
    return away * sin((sd - ring) * 0.07) * amp * 1.2 * band * exp(-age * 5.0);
}

// Luminance of the framebuffer at a clamped pixel, from the sRGB bytes:
// the edge effect wants contrast as the eye sees it, not linear light.
float LumaAt(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(fbW) - 1, int(fbH) - 1));
    return dot(gFrame.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114));
}

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;    // -1..1 across the sprite, x along the motion
    float3 rgb : COLOR0;       // linear light
    float  alpha : COLOR1;
    float  soft : COLOR2;      // 0 = hard pixel, 1 = soft disc
    float  split : COLOR3;     // prism: how far red and blue separate along x
};

static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(-1, 1),
    float2(-1, 1),  float2(1, -1), float2(1, 1)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VSOut o;
    // Exposure trails: a moving particle is drawn `trails` times a frame at
    // sub-positions back along its own path, each covering its share of it.
    // That is an exposure rather than one stretched blob, so a streak stays
    // smooth and evenly lit however fast the particle is going. At rest the
    // extra copies collapse to nothing (see `degenerate` below).
    const uint mult = max(uint(trails), 1u);
    const uint sub = iid % mult;
    // The pointer is a second draw call whose instance ids start at 0;
    // instanceBase says where its particles are (DesktopParticles::Draw).
    const uint idx = uint(instanceBase) + iid / mult;
    const DeskParticle p = gParticles[idx];
    const bool isCursor = idx >= particleCount;
    const float loose = 1.0 - solidity;

    float3 rgb;
    float alpha = 1.0;
    float lift = 0.0;   // screen px upward, the heat effect
    float edge = 0.0;   // 0..1 luminance-gradient strength, the edge effect
    float heat = 0.0;   // 0..1 disturbance energy, the heat effect
    int2 at = int2(0, 0);   // the framebuffer pixel the colour came from
    float cursorHalf = 0.0;   // > 0 for a pointer particle: its own crisp size
    if (isCursor)
    {
        // The cluster IS the cursor: a grid over the server's shape, one
        // particle per shape pixel, or the built-in arrow when there is no
        // shape — and around it a ring in the accent colour. Every colour
        // is chosen against the desktop underneath, so the pointer reads on
        // a white window as well as a dark one.
        const uint k = idx - particleCount;
        const uint g = uint(max(cursorGrid, 1.0));
        const float dark = PointerLuma() > 0.45 ? 1.0 : 0.0;   // a bright desktop wants dark ink
        if (k < g * g)
        {
            const float2 cell = float2(float(k % g), float(k / g));
            if (cursorW >= 1.0)
            {
                const uint2 cp = uint2(cell * float2(cursorW, cursorH) / float(g));
                const float4 c = gCursor.Load(int3(cp, 0));
                rgb = SrgbToLinearExact(c.rgb);
                alpha = c.a;
                // the server's own shape, with a rim of the opposite tone so
                // a white cursor still shows on a white window
                if (alpha > 0.5 && dark > 0.5 && dot(rgb, float3(0.299, 0.587, 0.114)) > 0.6)
                    rgb = lerp(rgb, float3(0.02, 0.02, 0.03), 0.35);
            }
            else
            {
                const int a = ArrowAt(cell);
                alpha = a == 0 ? 0.0 : 1.0;
                const float3 ink = float3(0.015, 0.015, 0.02);
                const float3 pale = float3(1.0, 1.0, 1.0);
                rgb = (a == 2) ? (dark > 0.5 ? pale : ink) : (dark > 0.5 ? ink : pale);
            }
            cursorHalf = 0.62;
        }
        else
        {
            // the ring: amber, which stands out on white and on black
            rgb = float3(1.0, 0.42, 0.06);
            alpha = 0.85;
            cursorHalf = 0.9;
        }
    }
    else
    {
        uint2 src;
        float2 home;
        uint sub;
        HomeOf(idx, src, home, sub);
        // the shockwave refracts: the colour comes from a displaced pixel
        const float2 refract = ShockRefraction(float2(src) + 0.5);
        at = clamp(int2(floor(float2(src) + 0.5 + refract)), int2(0, 0), int2(int(fbW) - 1, int(fbH) - 1));
        // B8G8R8A8_UNORM reads as (b,g,r,a) in .rgba order already swizzled
        // by the format: .rgb is red, green, blue
        const float4 c = gFrame.Load(int3(at, 0));
        rgb = SrgbToLinearExact(c.rgb);

        // Edge glow: the luminance gradient at this pixel, from its four
        // neighbours. Window frames, title bars and text outlines are where
        // it is large; flat fills are where it is zero. The boost pushes the
        // scene above 1, which is what the bloom pass turns into a halo.
        // (applied after vividness below, so the base colour is clipped to
        // 1 first and only the edge's own boost can cross the bloom threshold)
        if (fxEdge > 0.0)
        {
            const int2 sp = at;   // the glow follows the refracted picture
            const float gx = LumaAt(sp + int2(1, 0)) - LumaAt(sp - int2(1, 0));
            const float gy = LumaAt(sp + int2(0, 1)) - LumaAt(sp - int2(0, 1));
            edge = saturate(sqrt(gx * gx + gy * gy) * edgeGain);
        }
        // Heat: a pixel that just changed runs warm and lifts a few pixels,
        // cooling with the energy texture; a repaint looks re-etched, a
        // video region simmers. The lift is drawn, not simulated, so the
        // particle's own state is untouched and it settles with the energy.
        if (fxHeat > 0.0)
        {
            heat = gEnergy.Load(int3(src / max(stride, 1u), 0)) * fxHeat;
            lift = heat * 3.0 * scale;
        }
    }

    // Vividness: saturation about the pixel's own luminance and contrast
    // about mid grey, in linear light. 1 leaves the decoded colour alone
    // (and faithful mode pins it there).
    if (vivid != 1.0 && !isCursor)
    {
        const float l = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(l.xxx, rgb, vivid);
        const float contrast = 1.0 + (vivid - 1.0) * 0.4;
        rgb = (rgb - 0.18) * contrast + 0.18;
    }
    // The base colour never exceeds 1: a white window must not bloom. Only
    // the effects' own additions may, and they are bounded.
    rgb = clamp(rgb, 0.0, 1.0);
    // The redraw transition, for a pixel that changed within transitionSecs:
    // after the clamp, so its flame, line or relief may bloom for a moment.
    if (transition > 0.5 && !isCursor)
    {
        const float age = time - gStamp.Load(int3(at / max(stride, 1u), 0));
        if (age >= 0.0 && age < transitionSecs)
        {
            const float3 oldc = clamp(SrgbToLinearExact(gPrev.Load(int3(at, 0)).rgb), 0.0, 1.0);
            float edgeNew = edge;
            if (fxEdge <= 0.0)
            {
                // the emboss relief needs the gradient even with edge glow off
                const float gx = LumaAt(at + int2(1, 0)) - LumaAt(at - int2(1, 0));
                const float gy = LumaAt(at + int2(0, 1)) - LumaAt(at - int2(0, 1));
                edgeNew = saturate(sqrt(gx * gx + gy * gy) * edgeGain);
            }
            rgb = Redraw(int(transition + 0.5), age / transitionSecs, oldc, rgb, UnpackSeed(p.extra), edgeNew,
                         float2(at));
        }
    }
    // A particle in flight stretches along its velocity and glows blue-white
    // with its own speed: the movement is drawn, not implied. At rest the
    // speed is zero and nothing here does anything.
    float2 axis = float2(1.0, 0.0), across = float2(0.0, 1.0);
    float stretch = 0.0;
    float split = 0.0;
    float2 subOff = float2(0.0, 0.0);
    bool degenerate = sub > 0u;   // an extra exposure copy draws only if there is motion
    if (streak > 0.0 && !isCursor)
    {
        const float2 vel = UnpackVel(p.velPacked);
        const float sp = length(vel);
        if (sp > 60.0)
        {
            degenerate = false;
            axis = vel / sp;
            across = float2(-axis.y, axis.x);
            // the path covered this frame, shared out among the copies
            const float path = min(sp * max(dt, 1.0 / 240.0), 90.0) * streak;
            stretch = path * 0.5 / float(mult);
            subOff = -axis * path * (float(sub) / float(mult));
            split = prism;
            rgb += float3(0.22, 0.5, 1.35) * saturate(sp / 1400.0) * streak;
        }
    }
    rgb *= 1.0 + edge * 0.35;                              // edges: a little past 1, so bloom finds them
    rgb += float3(1.0, 0.55, 0.15) * heat * 0.6;            // heat: an amber tint that cools away

    // size: a hard pixel at solidity 1, growing into a soft glow disc; never
    // under one screen pixel, or a scaled-down desktop turns to speckle
    const float half = isCursor ? cursorHalf : (max(0.5 * particleSize * scale, 0.5) + glowSize * loose);
    o.soft = isCursor ? 0.0 : loose;
    o.alpha = alpha;
    o.rgb = rgb * brightness;
    if (!faithful && !isCursor)
    {
        // additive overlap: density particles per pixel must not sum to
        // density times the colour, and a desktop scaled below native size
        // lands 1/scale^2 particles on each screen pixel - which must not
        // sum either, or every highlight blows out
        o.rgb /= max(float(density), 1.0);
        o.rgb *= min(1.0, scale * scale);
        o.rgb *= 1.0 + hdrBoost * loose;
    }
    // Materialise: the arriving particles brighten over the flight, so the
    // desktop fades in as it assembles rather than popping.
    if (fxMaterialise > 0.0 && !isCursor)
        o.rgb *= smoothstep(0.0, kMaterialiseSeconds, time - bornTime);

    // The sprite is a spine from where the particle was to where it is,
    // tapered towards the tail, and — with tails on — bent sideways by the
    // curl field at the tail's own position, which is the field it just
    // flew through. A still particle keeps the plain square.
    const float2 corner = kCorners[vid];
    const float halfLong = half + stretch;
    const float2 centre = p.pos + subOff;
    float2 headPt = centre + axis * halfLong;
    float2 tailPt = centre - axis * halfLong;
    float taper = 1.0;
    if (stretch > 0.0)
    {
        taper = 0.35;
        if (tails > 0.0)
        {
            const float2 c2 = Curl2(float3(tailPt * curlScale, time * 0.3));
            tailPt += across * dot(c2, across) * curlAmp * tails * 2.0;
        }
    }
    const float along = corner.x * 0.5 + 0.5;      // 0 at the tail, 1 at the head
    const float2 spine = lerp(tailPt, headPt, along);
    const float wide = lerp(half * taper, half, along);
    float2 px = spine + across * (corner.y * wide) - float2(0.0, lift);
    if (degenerate)
        px = centre;                               // no motion: this copy has no area
    o.split = split;
    o.uv = corner;
    o.pos = float4(px.x / screenW * 2.0 - 1.0, 1.0 - px.y / screenH * 2.0, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // a hard square at solidity 1; a soft disc as the swarm loosens
    const float d = length(i.uv);
    const float hard = 1.0;
    const float disc = saturate(1.0 - d) * saturate(1.0 - d);
    float cov = lerp(hard, disc, i.soft) * i.alpha;
    // Prism: red and blue separate along the direction of travel, so a fast
    // particle leads blue and trails red instead of smearing to grey. The
    // motion is carried by the colour, which stays bright at any speed.
    float3 rgb = i.rgb;
    if (i.split > 0.0)
    {
        const float g = clamp(i.uv.x, -1.0, 1.0) * i.split;
        rgb.r *= 1.0 - g;
        rgb.b *= 1.0 + g;
        rgb.g *= 1.0 - 0.22 * abs(g);
    }
    if (lightMode > 0.5 || faithful)
        return float4(rgb * cov, cov);         // over: premultiplied
    return float4(rgb * cov, cov);             // additive: alpha ignored by the blend
}
