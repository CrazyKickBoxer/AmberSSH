// bloom_down.hlsl — prefilter (soft threshold) + 13-tap Jimenez downsample.
cbuffer BloomCB : register(b0)
{
    float2 invSrcSize;   // 1 / source texture size
    float2 invDstSize;   // 1 / destination texture size
    float  threshold;
    float  knee;
    float  bloomPad0, bloomPad1;
};

Texture2D<float4>   gSrc  : register(t0);
RWTexture2D<float4> gDst  : register(u0);
SamplerState        gLin  : register(s0);

float3 SoftThreshold(float3 c)
{
    float br   = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float w = max(soft, br - threshold) / max(br, 1e-4);
    return c * max(w, 0.0);
}

float3 Sample(float2 uv) { return gSrc.SampleLevel(gLin, uv, 0).rgb; }

// 13-tap downsample (Jimenez, SIGGRAPH 2014) — stable, low-fireflies.
float3 Down13(float2 uv)
{
    float2 o = invSrcSize;
    float3 a = Sample(uv + float2(-2, -2) * o);
    float3 b = Sample(uv + float2( 0, -2) * o);
    float3 c = Sample(uv + float2( 2, -2) * o);
    float3 d = Sample(uv + float2(-2,  0) * o);
    float3 e = Sample(uv);
    float3 f = Sample(uv + float2( 2,  0) * o);
    float3 g = Sample(uv + float2(-2,  2) * o);
    float3 h = Sample(uv + float2( 0,  2) * o);
    float3 i = Sample(uv + float2( 2,  2) * o);
    float3 j = Sample(uv + float2(-1, -1) * o);
    float3 k = Sample(uv + float2( 1, -1) * o);
    float3 l = Sample(uv + float2(-1,  1) * o);
    float3 m = Sample(uv + float2( 1,  1) * o);

    float3 col = e * 0.125;
    col += (a + c + g + i) * 0.03125;
    col += (b + d + f + h) * 0.0625;
    col += (j + k + l + m) * 0.125;
    return col;
}

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (id.xy + 0.5) * invDstSize;
    gDst[id.xy] = float4(SoftThreshold(Down13(uv)), 1.0);
}

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (id.xy + 0.5) * invDstSize;
    gDst[id.xy] = float4(Down13(uv), 1.0);
}
