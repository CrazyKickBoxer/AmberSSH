#include "bloom.h"

#include <algorithm>

struct BloomConstants
{
    float invSrcW, invSrcH;
    float invDstW, invDstH;
    float threshold, knee;
    float pad0, pad1;
};

bool Bloom::Init(Device& dev, ShaderCompiler& sc)
{
    m_dev = &dev;
    ID3D12Device* d = dev.Dev();

    // Root signature: 8 root constants (b0), SRV table t0-t1, UAV table u0,
    // static linear-clamp sampler.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE srvRange2 = {};
    srvRange2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange2.NumDescriptors = 1;
    srvRange2.BaseShaderRegister = 1;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(BloomConstants) / 4 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &srvRange };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = { 1, &srvRange2 };
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = { 1, &uavRange };

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 4;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &blob, &err),
                  "bloom RS serialize");
    ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(), IID_PPV_ARGS(&m_rs)),
                  "bloom RS");

    auto makePso = [&](const wchar_t* entry, ComPtr<ID3D12PipelineState>& pso,
                       const wchar_t* stem)
    {
        ShaderBlob cs = sc.Load(stem, entry, L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rs.Get();
        pd.CS = cs.Bytecode();
        ThrowIfFailed(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
                      "bloom PSO");
    };
    makePso(L"CSPrefilter", m_prefilterPSO, L"bloom_down");
    makePso(L"CSDown", m_downPSO, L"bloom_down");
    makePso(L"CSUp", m_upPSO, L"bloom_up");
    return true;
}

void Bloom::CreateTex(Tex& t, uint32_t w, uint32_t h, const wchar_t* name)
{
    ID3D12Device* d = m_dev->Dev();
    t.w = std::max(1u, w);
    t.h = std::max(1u, h);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = t.w;
    rd.Height = t.h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = kSceneFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    t.res.Reset();
    ThrowIfFailed(m_dev->Dev()->CreateCommittedResource(
                      &hp, D3D12_HEAP_FLAG_NONE, &rd,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                      IID_PPV_ARGS(&t.res)),
                  "bloom texture");
    t.res->SetName(name);
    t.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (t.srvSlot == UINT32_MAX)
    {
        t.srvSlot = m_dev->AllocSrv();
        t.uavSlot = m_dev->AllocSrv();
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = kSceneFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(t.res.Get(), &sv, m_dev->SrvCpu(t.srvSlot));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = kSceneFormat;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d->CreateUnorderedAccessView(t.res.Get(), nullptr, &uv, m_dev->SrvCpu(t.uavSlot));
}

void Bloom::Resize(uint32_t width, uint32_t height)
{
    uint32_t w = std::max(1u, width / 2);
    uint32_t h = std::max(1u, height / 2);
    for (uint32_t i = 0; i < kLevels; ++i)
    {
        wchar_t name[32];
        swprintf_s(name, L"BloomDown%u", i);
        CreateTex(m_down[i], w >> i, h >> i, name);
    }
    for (uint32_t i = 0; i < kLevels - 1; ++i)
    {
        wchar_t name[32];
        swprintf_s(name, L"BloomUp%u", i);
        CreateTex(m_up[i], w >> i, h >> i, name);
    }
    m_slotsAllocated = true;
}

void Bloom::Transition(ID3D12GraphicsCommandList* cl, Tex& t,
                       D3D12_RESOURCE_STATES to)
{
    if (t.state == to)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    t.state = to;
}

D3D12_GPU_DESCRIPTOR_HANDLE Bloom::ResultSrvGpu() const
{
    return m_dev->SrvGpu(m_up[0].srvSlot);
}

void Bloom::Record(ID3D12GraphicsCommandList* cl,
                   D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv)
{
    if (!m_slotsAllocated)
        return;
    PixScope pix(cl, L"Bloom");

    cl->SetComputeRootSignature(m_rs.Get());

    auto setConstants = [&](const Tex& src, const Tex& dst)
    {
        BloomConstants c = {};
        c.invSrcW = 1.0f / src.w;
        c.invSrcH = 1.0f / src.h;
        c.invDstW = 1.0f / dst.w;
        c.invDstH = 1.0f / dst.h;
        c.threshold = threshold;
        c.knee = knee;
        cl->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    };
    auto dispatchFor = [&](const Tex& dst)
    {
        cl->Dispatch((dst.w + 7) / 8, (dst.h + 7) / 8, 1);
    };
    auto uavBarrier = [&](Tex& t)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = t.res.Get();
        cl->ResourceBarrier(1, &b);
    };

    // Prefilter: scene (full res) → down[0] with soft threshold.
    {
        Tex fakeScene;
        fakeScene.w = m_down[0].w * 2;
        fakeScene.h = m_down[0].h * 2;
        Transition(cl, m_down[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->SetPipelineState(m_prefilterPSO.Get());
        setConstants(fakeScene, m_down[0]);
        cl->SetComputeRootDescriptorTable(1, sceneSrv);
        cl->SetComputeRootDescriptorTable(2, sceneSrv);   // t1 unused here
        cl->SetComputeRootDescriptorTable(3, m_dev->SrvGpu(m_down[0].uavSlot));
        dispatchFor(m_down[0]);
        uavBarrier(m_down[0]);
    }

    // Downsample chain.
    cl->SetPipelineState(m_downPSO.Get());
    for (uint32_t i = 1; i < kLevels; ++i)
    {
        Transition(cl, m_down[i - 1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cl, m_down[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setConstants(m_down[i - 1], m_down[i]);
        cl->SetComputeRootDescriptorTable(1, m_dev->SrvGpu(m_down[i - 1].srvSlot));
        cl->SetComputeRootDescriptorTable(2, m_dev->SrvGpu(m_down[i - 1].srvSlot));
        cl->SetComputeRootDescriptorTable(3, m_dev->SrvGpu(m_down[i].uavSlot));
        dispatchFor(m_down[i]);
        uavBarrier(m_down[i]);
    }

    // Upsample chain: up[i] = down[i] + tent(lower), lower = down[4] then up[i+1].
    cl->SetPipelineState(m_upPSO.Get());
    for (int i = static_cast<int>(kLevels) - 2; i >= 0; --i)
    {
        Tex& lower = (i == static_cast<int>(kLevels) - 2) ? m_down[i + 1] : m_up[i + 1];
        Transition(cl, lower, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cl, m_down[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cl, m_up[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setConstants(lower, m_up[i]);
        cl->SetComputeRootDescriptorTable(1, m_dev->SrvGpu(lower.srvSlot));
        cl->SetComputeRootDescriptorTable(2, m_dev->SrvGpu(m_down[i].srvSlot));
        cl->SetComputeRootDescriptorTable(3, m_dev->SrvGpu(m_up[i].uavSlot));
        dispatchFor(m_up[i]);
        uavBarrier(m_up[i]);
    }

    // Result → pixel-shader readable for the composite pass.
    Transition(cl, m_up[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Bloom::CopyResultTo(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst,
                         D3D12_RESOURCE_STATES& dstState)
{
    if (!m_slotsAllocated || !dst || !m_up[0].res)
        return;
    auto barrier = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    };
    Transition(cl, m_up[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(dst, m_up[0].res.Get());
    barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dstState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Transition(cl, m_up[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
