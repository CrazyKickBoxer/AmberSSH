// text_draw.hlsl — crisp DirectWrite-rasterized glyphs from the atlas. Serves
// two passes with one shader: the terminal's sharp glyph-core pass (premult
// source-over, per-cell ANSI color, optional italic shear) and the additive
// amber UI text (tabs, status line, overlay). No blur touches this pass —
// legibility lives here; the particles supply the phosphor life around it.
#include "amber_common.hlsli"

struct TextInst
{
    float2 pos;        // top-left of the glyph box, pixels
    float2 size;       // pixels
    float2 uv0;
    float2 uv1;
    float4 color;      // straight linear RGBA (premultiplied in the PS)
    float  shear;      // px of +x displacement at the glyph top (italic)
    float3 pad;
};

StructuredBuffer<TextInst> gGlyphs : register(t0);
Texture2D<float>           gAtlas  : register(t1);
SamplerState               gLin    : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    nointerpolation float4 color : TEXCOORD1;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    TextInst t = gGlyphs[iid];
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    float2 sp = t.pos + c * t.size;
    sp.x += t.shear * (1.0 - c.y);   // shear the top toward +x for italics
    float2 ndc = sp / float2(screenW, screenH) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    VSOut o;
    o.pos = float4(ndc, 0, 1);
    o.uv = lerp(t.uv0, t.uv1, c);
    o.color = t.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float cov = gAtlas.Sample(gLin, i.uv);
    float a = cov * i.color.a;
    return float4(i.color.rgb * a, a);   // premultiplied
}
