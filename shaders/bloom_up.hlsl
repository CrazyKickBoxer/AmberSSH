// bloom_up.hlsl — 9-tap tent upsample, additively combined with the same-level
// downsample result: up[i] = down[i] + tent(up[i+1]).
cbuffer BloomCB : register(b0)
{
    float2 invSrcSize;   // 1 / lower (smaller) level size
    float2 invDstSize;   // 1 / destination level size
    float  threshold;
    float  knee;
    float  bloomPad0, bloomPad1;
};

Texture2D<float4>   gLower : register(t0);  // smaller mip being upsampled
Texture2D<float4>   gSame  : register(t1);  // matching downsample level
RWTexture2D<float4> gDst   : register(u0);
SamplerState        gLin   : register(s0);

float3 Tent9(float2 uv)
{
    float2 o = invSrcSize;
    float3 s = float3(0, 0, 0);
    s += gLower.SampleLevel(gLin, uv + float2(-1, -1) * o, 0).rgb * 1.0;
    s += gLower.SampleLevel(gLin, uv + float2( 0, -1) * o, 0).rgb * 2.0;
    s += gLower.SampleLevel(gLin, uv + float2( 1, -1) * o, 0).rgb * 1.0;
    s += gLower.SampleLevel(gLin, uv + float2(-1,  0) * o, 0).rgb * 2.0;
    s += gLower.SampleLevel(gLin, uv, 0).rgb * 4.0;
    s += gLower.SampleLevel(gLin, uv + float2( 1,  0) * o, 0).rgb * 2.0;
    s += gLower.SampleLevel(gLin, uv + float2(-1,  1) * o, 0).rgb * 1.0;
    s += gLower.SampleLevel(gLin, uv + float2( 0,  1) * o, 0).rgb * 2.0;
    s += gLower.SampleLevel(gLin, uv + float2( 1,  1) * o, 0).rgb * 1.0;
    return s / 16.0;
}

[numthreads(8, 8, 1)]
void CSUp(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (id.xy + 0.5) * invDstSize;
    float3 col = gSame.SampleLevel(gLin, uv, 0).rgb + Tent9(uv);
    gDst[id.xy] = float4(col, 1.0);
}
