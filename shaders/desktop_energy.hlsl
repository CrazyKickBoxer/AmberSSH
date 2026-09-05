// desktop_energy.hlsl — the disturbance energy texture, one pass a frame.
//
// Changed pixels inject energy (the CPU side writes a burst proportional to
// the colour delta into gInject as each damage rectangle is uploaded); this
// pass folds the burst into the persistent energy, decays what was there,
// and clears the injection so the same burst is not counted twice. The sim
// reads gEnergy at each particle's source pixel.
//
// The textures are the sampled resolution (sampledW x sampledH), so this
// costs one texel per particle-home, never more than the framebuffer.
#include "desktop_common.hlsli"

RWTexture2D<float>       gEnergy : register(u0);
RWTexture2D<unorm float> gInject : register(u1);
RWTexture2D<float>       gStamp  : register(u2);   // when each pixel last changed (seconds)
Texture2D<float>         gPrevE  : register(t0);   // last frame's energy, for the front
Texture2D<float4>        gPicture : register(t1);  // what the front conducts through

// The picture's luminance at a sampled cell, from the sRGB bytes.
float CellLuma(int2 s)
{
    const int2 q = clamp(s * int(max(stride, 1u)), int2(0, 0), int2(int(fbW) - 1, int(fbH) - 1));
    return dot(gPicture.Load(int3(q, 0)).rgb, float3(0.299, 0.587, 0.114));
}

static const int2 kSpread[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= sampledW || id.y >= sampledH)
        return;
    const float keep = pow(max(energyDecay, 1e-4), dt);   // retention per second, applied for dt
    const float burst = gInject[id.xy];
    float e = gEnergy[id.xy] * keep;

    // Ignition front: instead of fading where it was lit, the energy
    // travels. Each cell takes the most its neighbours four cells away had
    // last frame, damped by how unlike them the picture is — so the front
    // runs through a window's fill at around 240 px a second and stops at
    // its border. The interface's own structure conducts the light.
    if (ignite > 0.0)
    {
        const int reach = 4;
        const float here = CellLuma(int2(id.xy));
        float incoming = 0.0;
        [unroll]
        for (int k = 0; k < 4; ++k)
        {
            const int2 q = clamp(int2(id.xy) + kSpread[k] * reach, int2(0, 0),
                                 int2(int(sampledW) - 1, int(sampledH) - 1));
            const float alike = exp(-abs(CellLuma(q) - here) * 7.0);
            incoming = max(incoming, gPrevE.Load(int3(q, 0)) * alike);
        }
        e = max(e, incoming * 0.965 * ignite);
    }
    gEnergy[id.xy] = resetFlag ? 0.0 : saturate(e + burst);
    // the change stamp, for the redraw transitions: any burst at all means
    // the pixel changed this frame (the CPU marks changed pixels with at
    // least 1/255 whatever the disturbance setting)
    if (resetFlag)
        gStamp[id.xy] = -1.0e6;
    else if (burst > 0.0)
        gStamp[id.xy] = time;
    gInject[id.xy] = 0.0;
}
