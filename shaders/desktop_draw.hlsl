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
    float2 uv  : TEXCOORD0;    // -1..1 across the quad
    float3 rgb : COLOR0;       // linear light
    float  alpha : COLOR1;
    float  soft : COLOR2;      // 0 = hard pixel, 1 = soft disc
};

static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(-1, 1),
    float2(-1, 1),  float2(1, -1), float2(1, 1)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VSOut o;
    const DeskParticle p = gParticles[iid];
    const bool isCursor = iid >= particleCount;
    const float loose = 1.0 - solidity;

    float3 rgb;
    float alpha = 1.0;
    float lift = 0.0;   // screen px upward, the heat effect
    if (isCursor)
    {
        // the cluster's particles sample the cursor shape by their seed
        // the shape occupies the top-left cursorW x cursorH of gCursor
        const float seed = UnpackSeed(p.extra);
        if (cursorW < 1.0 || cursorH < 1.0)
        {
            // no shape from the server: a plain white cluster
            rgb = float3(1.0, 1.0, 1.0);
            alpha = 1.0;
        }
        else
        {
            const uint2 cp = uint2(seed * cursorW, HashU(iid * 31u + 5u) * cursorH);
            const float4 c = gCursor.Load(int3(cp, 0));
            rgb = SrgbToLinearExact(c.rgb);
            alpha = c.a;
        }
    }
    else
    {
        uint2 src;
        float2 home;
        uint sub;
        HomeOf(iid, src, home, sub);
        // B8G8R8A8_UNORM reads as (b,g,r,a) in .rgba order already swizzled
        // by the format: .rgb is red, green, blue
        const float4 c = gFrame.Load(int3(src, 0));
        rgb = SrgbToLinearExact(c.rgb);

        // Edge glow: the luminance gradient at this pixel, from its four
        // neighbours. Window frames, title bars and text outlines are where
        // it is large; flat fills are where it is zero. The boost pushes the
        // scene above 1, which is what the bloom pass turns into a halo.
        if (fxEdge > 0.0)
        {
            const int2 sp = int2(src);
            const float gx = LumaAt(sp + int2(1, 0)) - LumaAt(sp - int2(1, 0));
            const float gy = LumaAt(sp + int2(0, 1)) - LumaAt(sp - int2(0, 1));
            const float edge = saturate(sqrt(gx * gx + gy * gy) * edgeGain);
            rgb *= 1.0 + edge * fxEdge * 1.2;
        }
        // Heat: a pixel that just changed runs warm and lifts a few pixels,
        // cooling with the energy texture; a repaint looks re-etched, a
        // video region simmers. The lift is drawn, not simulated, so the
        // particle's own state is untouched and it settles with the energy.
        if (fxHeat > 0.0)
        {
            const float e = gEnergy.Load(int3(src / max(stride, 1u), 0));
            rgb += float3(1.0, 0.55, 0.15) * e * fxHeat * 0.9;
            lift = e * fxHeat * 3.0 * scale;
        }
    }

    // Vividness: saturation about the pixel's own luminance and contrast
    // about mid grey, in linear light. 1 leaves the decoded colour alone
    // (and faithful mode pins it there).
    if (vivid != 1.0 && !isCursor)
    {
        const float l = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(l.xxx, rgb, vivid);
        const float contrast = 1.0 + (vivid - 1.0) * 0.6;
        rgb = max((rgb - 0.18) * contrast + 0.18, 0.0);
    }

    // size: a hard pixel at solidity 1, growing into a soft glow disc; never
    // under one screen pixel, or a scaled-down desktop turns to speckle
    const float half = max(0.5 * particleSize * scale, 0.5) + glowSize * loose;
    o.soft = loose;
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

    const float2 corner = kCorners[vid];
    const float2 px = p.pos + corner * half - float2(0.0, lift);
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
    const float cov = lerp(hard, disc, i.soft) * i.alpha;
    if (lightMode > 0.5 || faithful)
        return float4(i.rgb * cov, cov);       // over: premultiplied
    return float4(i.rgb * cov, cov);           // additive: alpha ignored by the blend
}
