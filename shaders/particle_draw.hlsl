// particle_draw.hlsl — instanced screen-space quads with radial nixie glow.
// Fast-moving particles stretch along their velocity into a fading capsule
// streak (motion trails): the head stays a hot round core, the tail dies to
// nothing. Settled particles have ~zero velocity, so resting text is always
// drawn as tight round points — trails can never smear a stable glyph.
#include "amber_common.hlsli"

StructuredBuffer<Particle> gParticles : register(t0);

struct VSOut
{
    float4 pos    : SV_Position;
    // uv.x runs along the streak axis: -1 = tail, +1 = head.
    float2 uv     : TEXCOORD0;
    float  bright : TEXCOORD1;
    // Raw color bits — must never be interpolated or the payload corrupts.
    nointerpolation float tint : TEXCOORD2;
    nointerpolation float streak : TEXCOORD3;   // 0 round .. 1 full trail
    nointerpolation float lenRatio : TEXCOORD4; // trail length / half width
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Particle p = gParticles[iid];

    // Triangle-strip corners: (-1,-1) (1,-1) (-1,1) (1,1)
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);

    float b = p.bright;
    float halfSize = glowSize * (0.50 + 0.60 * saturate(b));
    if (b < 0.004)
        halfSize = 0.0; // degenerate quad, no raster cost

    // Velocity streak: onset above ~40 px/s, saturating by ~200 px/s.
    float speed = length(p.vel);
    float streak = (trailScale > 0.0)
                       ? saturate((speed - 40.0) / 160.0)
                       : 0.0;
    float trailLen = min(speed * trailScale, 70.0) * streak;
    float2 axis = (speed > 1.0) ? p.vel / speed : float2(1.0, 0.0);
    float2 perp = float2(-axis.y, axis.x);

    // Head extends halfSize past the particle; the tail reaches back by the
    // trail length. Width tapers slightly toward the tail.
    float u01 = (c.x + 1.0) * 0.5;                    // 0 tail .. 1 head
    float along = lerp(-(halfSize + trailLen), halfSize, u01);
    float width = halfSize * lerp(1.0 - 0.35 * streak, 1.0, u01);
    float2 sp = p.pos + axis * along + perp * (c.y * width);

    float2 ndc = sp / float2(screenW, screenH) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    VSOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv = c;
    o.bright = b;
    o.tint = p.tint;
    o.streak = streak;
    o.lenRatio = (halfSize > 0.0) ? trailLen / halfSize : 0.0;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // Round profile: plain radial distance. Streak profile: distance to the
    // capsule spine (the head cap sits at uv.x = +1 in a space where the
    // geometry is pre-stretched, so distances along the body use the spine).
    float dRound = length(i.uv);
    // The particle center sits at uv.x = r/(2+r) for trail-to-halfwidth
    // ratio r (the quad extends halfSize past it for the head cap and
    // trailLen behind it). Ahead of the center: round cap. Behind: capsule
    // body whose profile is just the cross-axis distance.
    float r = i.lenRatio;
    float xc = r / (2.0 + r);
    float x = i.uv.x;
    float dx = max(0.0, (x - xc) / max(1.0 - xc, 1e-3));
    float dStreak = length(float2(dx, i.uv.y));
    // Tail fade: full brightness at the center, dying toward uv.x = -1.
    float tBody = saturate((x + 1.0) / max(xc + 1.0, 1e-3));
    float tailFade = lerp(1.0, 0.10 + 0.90 * pow(tBody, 1.4), i.streak);
    float d = lerp(dRound, dStreak, i.streak);

    // Glyph-pixel rendering: a near-flat core the width of one glyph pixel so
    // neighbouring particles fuse into continuous strokes (TrueType-like),
    // plus a short feather; the wide halo is bloom's job now.
    float halo = pow(smoothstep(0.80, 0.0, d), 3.0);
    float core = smoothstep(0.62, 0.18, d);

    float  b = i.bright;
    // Explicit ANSI/truecolor cells carry their resolved color; default
    // foreground keeps the amber nixie ramp. The hot center whitens slightly
    // toward the glyph color rather than always toward warm white.
    uint  cbits = asuint(i.tint);
    bool  hasColor = (cbits & 0x01000000u) != 0u;
    float3 fg = UnpackSrgbLinear(cbits & 0x00FFFFFFu);
    float3 c = hasColor ? fg * (0.40 + 0.70 * b) : AmberRamp(b);
    float3 hot = hasColor ? lerp(fg, float3(1.0, 1.0, 1.0), 0.35)
                          : lerp(ramp4.xyz, float3(1.0, 1.0, 1.0), 0.30);
    // ---- light ("paper") mode: dark ink, alpha-blended over the page -------
    // The draw PSO for light mode uses premultiplied over, so we output
    // (ink*a, a): strokes darken the paper, the halo is a faint ink bleed.
    if (lightMode > 0.5)
    {
        float3 ink = hasColor ? fg * 0.75 : inkColor.rgb;
        float a = saturate(core * b * 1.15 + halo * b * 0.18) * tailFade;
        return float4(ink * a, a);
    }

    float3 outc = c * halo * (0.18 + 0.22 * b)
                + hot * core * b * 1.05;

    outc *= tailFade;

    // In HDR mode the hottest cores push beyond SDR white.
    outc *= 1.0 + hdrBoost * smoothstep(0.75, 1.25, b) * 3.0;
    return float4(outc, 1.0);
}
