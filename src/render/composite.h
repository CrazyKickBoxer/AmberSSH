// composite.h — final pass: scene + bloom → backbuffer with vignette,
// scanlines, and SDR tonemap or HDR (scRGB) output.
#pragma once

#include "../common.h"
#include "../dx/device.h"
#include "../dx/shaders.h"

class Composite
{
public:
    bool Init(Device& dev, ShaderCompiler& sc);
    // Rebuild the PSO when the backbuffer format changes (SDR ↔ HDR).
    void RebuildForBackbuffer();

    struct Params
    {
        float bloomStrength = 0.9f;
        float vignette = 0.02f;
        float scanAmp = 0.03f;
        float time = 0.0f;
        uint32_t hdrMode = 0;
        float maxNits = 400.0f;
        float paperWhiteNits = 240.0f;
        float exposure = 1.0f;
        float grayscale = 0.0f;   // 1 = e-ink / Hercules greyscale
        float pixelBlock = 0.0f;  // >0 = pixel-art block size (device px)
        float invResX = 0.0f, invResY = 0.0f;
        // Exit-code edge flash (additive tint) + CRT power-off collapse.
        float flashR = 0.0f, flashG = 0.0f, flashB = 0.0f, flashAmt = 0.0f;
        float crtOff = 0.0f;
        float shakeX = 0.0f, shakeY = 0.0f;   // screen-shake UV offset
        // Compiz cube on session switch: rotation 0..pi/2 (0 = off),
        // direction (+1 next from the right, -1 previous from the left),
        // backbuffer aspect, colour of the void behind the cube.
        float cubeAngle = 0.0f, cubeDir = 1.0f, aspect = 1.0f;
        float cubeBgR = 0.0f, cubeBgG = 0.0f, cubeBgB = 0.0f;
        // A remote desktop tab: no filmic curve, no scanlines, no vignette,
        // an exact sRGB encode — a document window is not a glowing tube.
        // Bloom still adds, at whatever strength the caller scaled it to.
        float desktopMode = 0.0f;
    };
    static_assert(sizeof(Params) % 4 == 0 && sizeof(Params) / 4 <= 64,
                  "composite root constants must fit in 64 DWORDs");

    // snapSceneSrv / snapBloomSrv: the frozen departing frame for the cube
    // (always bound; only sampled while p.cubeAngle > 0).
    void Record(ID3D12GraphicsCommandList* cl,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
                D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv,
                D3D12_GPU_DESCRIPTOR_HANDLE snapSceneSrv,
                D3D12_GPU_DESCRIPTOR_HANDLE snapBloomSrv,
                const Params& p, uint32_t width, uint32_t height);

private:
    Device* m_dev = nullptr;
    ShaderCompiler* m_sc = nullptr;
    ComPtr<ID3D12RootSignature> m_rs;
    ComPtr<ID3D12PipelineState> m_pso;
    DXGI_FORMAT m_builtFormat = DXGI_FORMAT_UNKNOWN;
};
