// bloom.h — compute bloom: threshold prefilter, 5-level 13-tap downsample,
// tent-filter upsample chain. Runs at half resolution.
#pragma once

#include "../common.h"
#include "../dx/device.h"
#include "../dx/shaders.h"

class Bloom
{
public:
    static constexpr uint32_t kLevels = 5;

    bool Init(Device& dev, ShaderCompiler& sc);
    void Resize(uint32_t width, uint32_t height);

    // sceneSrv: descriptor of the HDR scene texture (must be in
    // NON_PIXEL_SHADER_RESOURCE state). Result readable via ResultSrvGpu().
    void Record(ID3D12GraphicsCommandList* cl, D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv);

    D3D12_GPU_DESCRIPTOR_HANDLE ResultSrvGpu() const;   // upsample level 0 (half res)
    uint32_t ResultWidth() const { return m_up[0].w; }
    uint32_t ResultHeight() const { return m_up[0].h; }
    // Copy the result into dst (same size and kSceneFormat). dstState is the
    // caller's tracked state of dst; it is left PIXEL_SHADER_RESOURCE. Used
    // by the session-switch cube to freeze the departing frame's bloom.
    void CopyResultTo(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst,
                      D3D12_RESOURCE_STATES& dstState);

    float threshold = 1.15f;
    float knee = 0.35f;

private:
    struct Tex
    {
        ComPtr<ID3D12Resource> res;
        uint32_t srvSlot = UINT32_MAX;
        uint32_t uavSlot = UINT32_MAX;
        uint32_t w = 0, h = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    void Transition(ID3D12GraphicsCommandList* cl, Tex& t, D3D12_RESOURCE_STATES to);
    void CreateTex(Tex& t, uint32_t w, uint32_t h, const wchar_t* name);

    Device* m_dev = nullptr;
    ComPtr<ID3D12RootSignature> m_rs;
    ComPtr<ID3D12PipelineState> m_prefilterPSO;
    ComPtr<ID3D12PipelineState> m_downPSO;
    ComPtr<ID3D12PipelineState> m_upPSO;

    Tex m_down[kLevels];
    Tex m_up[kLevels - 1];
    bool m_slotsAllocated = false;
};
