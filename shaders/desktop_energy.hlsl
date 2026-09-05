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

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= sampledW || id.y >= sampledH)
        return;
    const float keep = pow(max(energyDecay, 1e-4), dt);   // retention per second, applied for dt
    const float burst = gInject[id.xy];
    gEnergy[id.xy] = resetFlag ? 0.0 : saturate(gEnergy[id.xy] * keep + burst);
    gInject[id.xy] = 0.0;
}
