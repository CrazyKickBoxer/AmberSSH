// rects.hlsl — instanced RGBA rectangles: ANSI cell backgrounds, the Miami
// Sunset selection gradient, underline/strike bars, UI fills and borders.
// Output is premultiplied linear color; the pipeline state chooses the blend
// (source-over for backgrounds/underlays, additive for glowing UI chrome).
//
// flags selects the shape (UI chrome skins):
//   0 plain rect            1 selection (cyan glint on the top edge)
//   2 chamfer: pad.x px cut off the top-left and bottom-right corners
//   3 slant:   parallelogram leaning right by pad.x px (top edge shifted)
//   4 hexcut:  pad.x px cut off all four corners (HUD panel)
//   5 pill:    fully rounded ends (LCARS segments)
//   6 rounded: rounded rectangle with corner radius pad.x
#include "amber_common.hlsli"

struct RectInst
{
    float2 pos;        // top-left, pixels
    float2 size;       // pixels
    float4 c0;         // premultiplied linear RGBA at the left edge
    float4 c1;         // premultiplied linear RGBA at the right edge
    float  border;     // 0 = filled, >0 = border thickness in pixels
    float  flags;      // shape / glint, see above
    float2 pad;        // x = shape parameter (px)
};

StructuredBuffer<RectInst> gRects : register(t0);

struct VSOut
{
    float4 pos    : SV_Position;
    float2 local  : TEXCOORD0;   // pixel position inside rect
    float2 size   : TEXCOORD1;
    nointerpolation float4 c0     : TEXCOORD2;
    nointerpolation float4 c1     : TEXCOORD3;
    nointerpolation float2 params : TEXCOORD4;   // x = border, y = flags
    nointerpolation float2 extra  : TEXCOORD5;   // shape parameter
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    RectInst r = gRects[iid];
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    float2 sp = r.pos + c * r.size;
    float2 ndc = sp / float2(screenW, screenH) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    VSOut o;
    o.pos = float4(ndc, 0, 1);
    o.local = c * r.size;
    o.size = r.size;
    o.c0 = r.c0;
    o.c1 = r.c1;
    o.params = float2(r.border, r.flags);
    o.extra = r.pad;
    return o;
}

// Signed inside-distance (px) for the skin shapes; the plain rect is +inf.
float ShapeInside(float2 l, float2 s, float flags, float p)
{
    if (flags > 5.5)          // rounded rect, corner radius p
    {
        float r = min(p, min(s.x, s.y) * 0.5);
        float2 c = clamp(l, float2(r, r), s - r);
        return r - length(l - c);
    }
    if (flags > 4.5)          // pill: fully rounded ends (LCARS segments)
    {
        float r = min(s.x, s.y) * 0.5;
        float2 c = clamp(l, float2(r, r), s - r);
        return r - length(l - c);
    }
    if (flags > 3.5)          // hexcut: all four corners
    {
        float d = min(min(l.x + l.y, (s.x - l.x) + (s.y - l.y)),
                      min((s.x - l.x) + l.y, l.x + (s.y - l.y)));
        return d - p;
    }
    if (flags > 2.5)          // slant: lean right by p
    {
        float t = l.y / max(s.y, 1.0);
        float leftEdge = p * (1.0 - t);
        float rightEdge = s.x - p * t;
        return min(l.x - leftEdge, rightEdge - l.x);
    }
    if (flags > 1.5)          // chamfer: top-left and bottom-right corners
        return min(l.x + l.y, (s.x - l.x) + (s.y - l.y)) - p;
    return 1e6;
}

float4 PSMain(VSOut i) : SV_Target
{
    float u = saturate(i.local.x / max(i.size.x, 1.0));
    float4 col = lerp(i.c0, i.c1, u);

    float border = i.params.x;
    float flags = i.params.y;
    float inside = ShapeInside(i.local, i.size, flags, i.extra.x);
    if (border > 0.0)
    {
        float2 dEdge = min(i.local, i.size - i.local);
        float  d = min(min(dEdge.x, dEdge.y), inside);   // shaped outline
        float  a = 1.0 - smoothstep(border - 1.0, border + 0.5, d);
        a = max(a, 0.12 * (1.0 - smoothstep(0.0, border * 4.0, d)));
        col *= a;
    }
    // Shaped fill: antialiased cut along the slant / chamfer.
    col *= saturate(inside + 0.5);

    // Selection glint: a faint cyan light along the upper edge.
    if (abs(flags - 1.0) < 0.5)
    {
        float v = i.local.y / max(i.size.y, 1.0);
        float glint = (1.0 - smoothstep(0.0, 0.22, v)) * 0.10;
        col.rgb += float3(0.024, 0.72, 0.76) * glint;   // #2DE2E6 linear-ish
    }
    return col;
}
