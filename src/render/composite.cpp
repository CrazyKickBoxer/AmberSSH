#include "composite.h"

bool Composite::Init(Device& dev, ShaderCompiler& sc)
{
    m_dev = &dev;
    m_sc = &sc;

    D3D12_DESCRIPTOR_RANGE sceneRange = {};
    sceneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sceneRange.NumDescriptors = 1;
    sceneRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE bloomRange = {};
    bloomRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bloomRange.NumDescriptors = 1;
    bloomRange.BaseShaderRegister = 1;

    // Cube snapshots (departing frame): scene at t2, bloom at t3.
    D3D12_DESCRIPTOR_RANGE snapRange = {};
    snapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    snapRange.NumDescriptors = 1;
    snapRange.BaseShaderRegister = 2;

    D3D12_DESCRIPTOR_RANGE snapBloomRange = {};
    snapBloomRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    snapBloomRange.NumDescriptors = 1;
    snapBloomRange.BaseShaderRegister = 3;

    D3D12_ROOT_PARAMETER params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(Params) / 4 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &sceneRange };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = { 1, &bloomRange };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = { 1, &snapRange };
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable = { 1, &snapBloomRange };
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 5;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &blob, &err),
                  "composite RS serialize");
    ThrowIfFailed(dev.Dev()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                 blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&m_rs)),
                  "composite RS");
    RebuildForBackbuffer();
    return true;
}

void Composite::RebuildForBackbuffer()
{
    DXGI_FORMAT fmt = m_dev->BackBufferFormat();
    if (fmt == m_builtFormat && m_pso)
        return;

    ShaderBlob vs = m_sc->Load(L"composite", L"VSMain", L"vs_6_0");
    ShaderBlob ps = m_sc->Load(L"composite", L"PSMain", L"ps_6_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rs.Get();
    pd.VS = vs.Bytecode();
    pd.PS = ps.Bytecode();
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    m_pso.Reset();
    ThrowIfFailed(m_dev->Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)),
                  "composite PSO");
    m_builtFormat = fmt;
}

void Composite::Record(ID3D12GraphicsCommandList* cl,
                       D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                       D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
                       D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv,
                       D3D12_GPU_DESCRIPTOR_HANDLE snapSceneSrv,
                       D3D12_GPU_DESCRIPTOR_HANDLE snapBloomSrv,
                       const Params& p, uint32_t width, uint32_t height)
{
    PixScope pix(cl, L"Composite");

    D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(width),
                          static_cast<float>(height), 0, 1 };
    D3D12_RECT sc = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    cl->SetGraphicsRootSignature(m_rs.Get());
    cl->SetPipelineState(m_pso.Get());
    cl->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &p, 0);
    cl->SetGraphicsRootDescriptorTable(1, sceneSrv);
    cl->SetGraphicsRootDescriptorTable(2, bloomSrv);
    cl->SetGraphicsRootDescriptorTable(3, snapSceneSrv);
    cl->SetGraphicsRootDescriptorTable(4, snapBloomSrv);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
}
