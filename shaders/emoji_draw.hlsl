// emoji_draw.hlsl — full-colour emoji from the RGBA colour atlas. Same quad
// geometry as text_draw, but the pixel shader emits the glyph's own colour
// (premultiplied) scaled by a fade alpha, instead of a theme tint.
#include "amber_common.hlsli"

struct TextInst
{
    float2 pos;
    float2 size;
    float2 uv0;
    float2 uv1;
    float4 color;      // rgb unused; a = fade
    float  shear;
    float3 pad;
};

StructuredBuffer<TextInst> gGlyphs : register(t0);
Texture2D<float4>          gAtlas  : register(t1);   // premultiplied RGBA
SamplerState               gLin    : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    nointerpolation float fade : TEXCOORD1;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    TextInst t = gGlyphs[iid];
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    float2 sp = t.pos + c * t.size;
    float2 ndc = sp / float2(screenW, screenH) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    VSOut o;
    o.pos = float4(ndc, 0, 1);
    o.uv = lerp(t.uv0, t.uv1, c);
    o.fade = t.color.a;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // Atlas is premultiplied; scale by the fade for birth reveal / dimming.
    return gAtlas.Sample(gLin, i.uv) * i.fade;
}
