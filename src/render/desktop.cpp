// desktop.cpp — the VNC particle desktop. See desktop.h.
#include "desktop.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using amber::vnc::CursorShape;
using amber::vnc::Damage;
using amber::vnc::Rect;

namespace
{

constexpr uint32_t kParticleBytes = 16;   // DeskParticle in desktop_common.hlsli

uint32_t AlignUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* d, uint64_t bytes, D3D12_HEAP_TYPE heap,
                                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    ThrowIfFailed(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
                  "desktop buffer");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* d, DXGI_FORMAT fmt, uint32_t w, uint32_t h,
                                   D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = std::max<uint32_t>(w, 1);
    rd.Height = std::max<uint32_t>(h, 1);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    ThrowIfFailed(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
                  "desktop texture");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12RootSignature> MakeRS(ID3D12Device* d, const D3D12_ROOT_SIGNATURE_DESC& rs, const char* what)
{
    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), what);
    ComPtr<ID3D12RootSignature> out;
    ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)),
                  what);
    return out;
}

} // namespace

bool DesktopParticles::Init(Device& dev, ShaderCompiler& sc)
{
    m_dev = &dev;
    ID3D12Device* d = dev.Dev();

    // ---- energy pass: CBV(b1) + table { UAV u0 energy, UAV u1 inject } ------
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 1, 0 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = { 1, &range };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;
        m_energyRS = MakeRS(d, rs, "desktop energy RS");
        ShaderBlob cs = sc.Load(L"desktop_energy", L"CSMain", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_energyRS.Get();
        pd.CS = cs.Bytecode();
        ThrowIfFailed(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_energyPSO)), "desktop energy PSO");
    }
    // ---- sim pass: CBV(b1) + UAV(u0 particles) + table { UAV u1 energy } ---
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 1;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 1, 0 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &range };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3;
        rs.pParameters = params;
        m_simRS = MakeRS(d, rs, "desktop sim RS");
        ShaderBlob cs = sc.Load(L"desktop_sim", L"CSMain", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_simRS.Get();
        pd.CS = cs.Bytecode();
        ThrowIfFailed(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_simPSO)), "desktop sim PSO");
    }
    // ---- draw: CBV(b1) + SRV(t0 particles) + table { SRV t1 frame, t2 cursor, t3 energy }
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
        range.BaseShaderRegister = 1;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 1, 0 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &range };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3;
        rs.pParameters = params;
        m_drawRS = MakeRS(d, rs, "desktop draw RS");

        ShaderBlob vs = sc.Load(L"desktop_draw", L"VSMain", L"vs_6_0");
        ShaderBlob ps = sc.Load(L"desktop_draw", L"PSMain", L"ps_6_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_drawRS.Get();
        pd.VS = vs.Bytecode();
        pd.PS = ps.Bytecode();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kSceneFormat;
        pd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_drawAdd)), "desktop draw PSO");
        // faithful / paper: premultiplied "over", so nothing is summed
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_drawOver)), "desktop draw PSO (over)");
    }

    // descriptors: two consecutive pairs
    m_slotEnergyUav = dev.AllocSrv();
    m_slotInjectUav = dev.AllocSrv();
    if (m_slotInjectUav != m_slotEnergyUav + 1)
    {
        m_slotEnergyUav = dev.AllocSrv();
        m_slotInjectUav = dev.AllocSrv();
    }
    // ... and the draw's triple
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        m_slotFrameSrv = dev.AllocSrv();
        m_slotCursorSrv = dev.AllocSrv();
        m_slotEnergySrv = dev.AllocSrv();
        if (m_slotCursorSrv == m_slotFrameSrv + 1 && m_slotEnergySrv == m_slotFrameSrv + 2)
            break;
    }
    if (m_slotInjectUav != m_slotEnergyUav + 1 || m_slotCursorSrv != m_slotFrameSrv + 1 ||
        m_slotEnergySrv != m_slotFrameSrv + 2)
        return false;

    // the cursor texture exists from the start, empty
    m_cursor = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, amber::vnc::kMaxCursorDim, amber::vnc::kMaxCursorDim,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"desktop cursor");
    m_cursorState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(m_cursor.Get(), &sv, dev.SrvCpu(m_slotCursorSrv));
    return true;
}

bool DesktopParticles::Configure(uint32_t fbW, uint32_t fbH, uint32_t density)
{
    if (!m_dev || fbW == 0 || fbH == 0)
        return false;
    ID3D12Device* d = m_dev->Dev();

    // ---- the budget ---------------------------------------------------------
    // A quarter of the adapter's memory for particles, never more than
    // kMaxDesktopParticles. Density comes down first; only when density 1
    // still does not fit does the desktop get sampled.
    const uint64_t vram = m_dev->VideoMemoryBytes();
    uint64_t budget = kMaxDesktopParticles;
    if (vram > 0)
        budget = std::min<uint64_t>(budget, (vram / 4) / kParticleBytes);
    Layout L;
    L.fbW = fbW;
    L.fbH = fbH;
    L.density = std::clamp<uint32_t>(density, 1, 4);
    L.stride = 1;
    for (;;)
    {
        L.sampledW = (fbW + L.stride - 1) / L.stride;
        L.sampledH = (fbH + L.stride - 1) / L.stride;
        const uint64_t n = static_cast<uint64_t>(L.sampledW) * L.sampledH * L.density;
        if (n <= budget)
            break;
        if (L.density > 1)
        {
            --L.density;
            L.clamped = true;
        }
        else
        {
            ++L.stride;
            L.sampled = true;
        }
    }
    L.particles = L.sampledW * L.sampledH * L.density;
    L.gpuBytes = static_cast<uint64_t>(L.particles + kCursorParticles) * kParticleBytes +
                 static_cast<uint64_t>(fbW) * fbH * 4 +
                 static_cast<uint64_t>(L.sampledW) * L.sampledH * 3;
    m_layout = L;

    // ---- resources --------------------------------------------------------------
    m_dev->WaitIdle();   // the old ones may be in flight; a resize is rare and visible anyway
    // buffers are created in COMMON whatever is asked for; the first
    // transition takes it to the UAV the sim needs
    m_particles = MakeBuffer(d, static_cast<uint64_t>(L.particles + kCursorParticles) * kParticleBytes,
                             D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COMMON, L"desktop particles");
    m_particleState = D3D12_RESOURCE_STATE_COMMON;
    m_frame = MakeTexture(d, DXGI_FORMAT_B8G8R8A8_UNORM, fbW, fbH, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, L"desktop framebuffer");
    m_frameState = D3D12_RESOURCE_STATE_COPY_DEST;
    m_energy = MakeTexture(d, DXGI_FORMAT_R16_FLOAT, L.sampledW, L.sampledH,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           L"desktop energy");
    m_energyState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_inject = MakeTexture(d, DXGI_FORMAT_R8_UNORM, L.sampledW, L.sampledH,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST,
                           L"desktop inject");
    m_injectState = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_UNORDERED_ACCESS_VIEW_DESC ue = {};
    ue.Format = DXGI_FORMAT_R16_FLOAT;
    ue.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d->CreateUnorderedAccessView(m_energy.Get(), nullptr, &ue, m_dev->SrvCpu(m_slotEnergyUav));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ui = {};
    ui.Format = DXGI_FORMAT_R8_UNORM;
    ui.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d->CreateUnorderedAccessView(m_inject.Get(), nullptr, &ui, m_dev->SrvCpu(m_slotInjectUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(m_frame.Get(), &sv, m_dev->SrvCpu(m_slotFrameSrv));
    D3D12_SHADER_RESOURCE_VIEW_DESC se = {};
    se.Format = DXGI_FORMAT_R16_FLOAT;
    se.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    se.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    se.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(m_energy.Get(), &se, m_dev->SrvCpu(m_slotEnergySrv));

    m_shadow.assign(static_cast<size_t>(fbW) * fbH, 0xFF000000u);
    m_needReset = true;
    return true;
}

void DesktopParticles::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                                  D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES to)
{
    if (cur == to)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    cur = to;
}

DesktopParticles::Staging& DesktopParticles::StagingFor(FrameContext& frame, uint64_t bytes)
{
    // one staging buffer per frame slot, identified by the FrameContext
    uint32_t idx = 0;
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        if (m_frameSlots[i] == &frame) { idx = i; break; }
        if (m_frameSlots[i] == nullptr) { m_frameSlots[i] = &frame; idx = i; break; }
    }
    Staging& s = m_staging[idx];
    // sized for a whole framebuffer at aligned pitch, plus the inject plane,
    // grown only when a bigger desktop arrives; the slot's previous use is
    // at least kFramesInFlight frames old, which BeginFrame has waited for
    const uint64_t full = static_cast<uint64_t>(AlignUp(m_layout.fbW * 4, 256)) * m_layout.fbH +
                          static_cast<uint64_t>(AlignUp(m_layout.sampledW, 256)) * m_layout.sampledH + 64 * 1024;
    const uint64_t want = std::max(full, bytes + 512);
    if (!s.buffer || s.size < want)
    {
        s.buffer = MakeBuffer(m_dev->Dev(), want, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_GENERIC_READ, L"desktop staging");
        s.size = want;
        D3D12_RANGE none = { 0, 0 };
        void* p = nullptr;
        ThrowIfFailed(s.buffer->Map(0, &none, &p), "desktop staging map");
        s.mapped = static_cast<uint8_t*>(p);
    }
    return s;
}

bool DesktopParticles::UploadRect(ID3D12GraphicsCommandList* cl, FrameContext& frame, ID3D12Resource* target,
                                  uint32_t bpp, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  const uint8_t* rows, uint32_t srcPitch)
{
    if (w == 0 || h == 0)
        return true;
    const uint32_t rowBytes = w * bpp;
    const uint32_t pitch = AlignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t total = static_cast<uint64_t>(pitch) * h;

    ID3D12Resource* src = nullptr;
    uint64_t offset = 0;
    uint8_t* cpu = nullptr;
    const uint64_t ringFree = frame.ring.Capacity() > frame.ring.Used() ? frame.ring.Capacity() - frame.ring.Used() : 0;
    if (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT <= ringFree && total <= frame.ring.Capacity() / 2)
    {
        UploadRing::Alloc a = frame.ring.Allocate(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!a.cpu)
            return false;
        src = a.resource;
        offset = a.offset;
        cpu = a.cpu;
    }
    else
    {
        Staging& s = StagingFor(frame, m_stagingHead + total);
        if (m_stagingHead + total > s.size)
        {
            m_fellBack = true;
            return false;   // more than a frame's worth: the caller re-sends
        }
        src = s.buffer.Get();
        offset = m_stagingHead;
        cpu = s.mapped + m_stagingHead;
        m_stagingHead = AlignUp(static_cast<uint32_t>(m_stagingHead + total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        m_fellBack = true;
    }
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(cpu + static_cast<size_t>(r) * pitch, rows + static_cast<size_t>(r) * srcPitch, rowBytes);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = src;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = offset;
    sl.PlacedFootprint.Footprint.Format = bpp == 4 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    sl.PlacedFootprint.Footprint.Width = w;
    sl.PlacedFootprint.Footprint.Height = h;
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = pitch;
    cl->CopyTextureRegion(&dst, x, y, 0, &sl, nullptr);
    m_bytesLast += total;
    return true;
}

void DesktopParticles::Begin(ID3D12GraphicsCommandList* cl)
{
    m_dev->Stamp(cl, Device::StampDesktopBegin);
    m_rectsLast = 0;
    m_bytesLast = 0;
    m_fellBack = false;
    m_stagingHead = 0;
}

void DesktopParticles::Upload(ID3D12GraphicsCommandList* cl, FrameContext& frame, const Damage& d, float disturbance)
{
    if (!Ready() || d.rects.empty() || d.width != m_layout.fbW || d.height != m_layout.fbH)
        return;
    Transition(cl, m_frame.Get(), m_frameState, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(cl, m_inject.Get(), m_injectState, D3D12_RESOURCE_STATE_COPY_DEST);

    const uint32_t W = m_layout.fbW, H = m_layout.fbH, S = m_layout.stride;
    size_t base = 0;
    for (const Rect& r : d.rects)
    {
        const size_t count = static_cast<size_t>(r.w) * r.h;
        if (base + count > d.pixels.size() || static_cast<uint32_t>(r.x) + r.w > W ||
            static_cast<uint32_t>(r.y) + r.h > H)
            break;   // a Damage that does not describe itself is not uploaded
        const uint32_t* px = d.pixels.data() + base;
        base += count;

        // colour deltas against the shadow -> bursts at sampled resolution
        const uint32_t sx0 = r.x / S, sy0 = r.y / S;
        const uint32_t sx1 = (r.x + r.w - 1) / S, sy1 = (r.y + r.h - 1) / S;
        const uint32_t sw = sx1 - sx0 + 1, sh = sy1 - sy0 + 1;
        m_injectScratch.assign(static_cast<size_t>(sw) * sh, 0);
        bool any = false;
        for (uint32_t yy = 0; yy < r.h; ++yy)
        {
            uint32_t* shadowRow = m_shadow.data() + static_cast<size_t>(r.y + yy) * W + r.x;
            const uint32_t* newRow = px + static_cast<size_t>(yy) * r.w;
            uint8_t* injRow = m_injectScratch.data() + static_cast<size_t>((r.y + yy) / S - sy0) * sw;
            for (uint32_t xx = 0; xx < r.w; ++xx)
            {
                const uint32_t o = shadowRow[xx], n = newRow[xx];
                if (o != n)
                {
                    const int dr = std::abs(static_cast<int>((o >> 16) & 0xFF) - static_cast<int>((n >> 16) & 0xFF));
                    const int dg = std::abs(static_cast<int>((o >> 8) & 0xFF) - static_cast<int>((n >> 8) & 0xFF));
                    const int db = std::abs(static_cast<int>(o & 0xFF) - static_cast<int>(n & 0xFF));
                    const int delta = std::max(dr, std::max(dg, db));
                    const int burst = std::min(255, static_cast<int>(delta * disturbance));
                    uint8_t& cell = injRow[(r.x + xx) / S - sx0];
                    if (burst > cell)
                        cell = static_cast<uint8_t>(burst);
                    shadowRow[xx] = n;
                    any = true;
                }
            }
        }
        if (!UploadRect(cl, frame, m_frame.Get(), 4, r.x, r.y, r.w, r.h,
                        reinterpret_cast<const uint8_t*>(px), r.w * 4))
            break;
        if (any)
            UploadRect(cl, frame, m_inject.Get(), 1, sx0, sy0, sw, sh, m_injectScratch.data(), sw);
        ++m_rectsLast;
    }
    Transition(cl, m_frame.Get(), m_frameState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DesktopParticles::SetCursor(ID3D12GraphicsCommandList* cl, FrameContext& frame, const CursorShape& cs)
{
    m_cursorShape = cs;
    if (cs.width == 0 || cs.height == 0 || cs.bgra.size() < static_cast<size_t>(cs.width) * cs.height)
        return;
    Transition(cl, m_cursor.Get(), m_cursorState, D3D12_RESOURCE_STATE_COPY_DEST);
    UploadRect(cl, frame, m_cursor.Get(), 4, 0, 0, cs.width, cs.height,
               reinterpret_cast<const uint8_t*>(cs.bgra.data()), cs.width * 4);
    Transition(cl, m_cursor.Get(), m_cursorState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DesktopParticles::Simulate(ID3D12GraphicsCommandList* cl, FrameContext& frame, const Params& p,
                                float screenW, float screenH)
{
    if (!Ready())
        return;
    m_dev->Stamp(cl, Device::StampDesktopSimBegin);

    DesktopCB cb;
    cb.time = p.time;
    cb.dt = p.dt;
    cb.effectSpeed = p.effectSpeed;
    cb.solidity = std::clamp(p.solidity, 0.0f, 1.0f);
    cb.particleSize = std::clamp(p.particleSize, 1.0f, 3.0f);
    cb.disturbance = std::clamp(p.disturbance, 0.0f, 2.0f);
    cb.dstX = p.dstX;
    cb.dstY = p.dstY;
    cb.scale = p.scale;
    cb.jitter = 1.5f;
    cb.screenW = screenW;
    cb.screenH = screenH;
    cb.mouseX = p.mouseX;
    cb.mouseY = p.mouseY;
    cb.mouseRadius = p.mouseRadius;
    cb.mouseForce = p.mouseForce;
    cb.shockX = p.shockX;
    cb.shockY = p.shockY;
    cb.shockTime = p.shockTime;
    cb.shockAmp = p.shockAmp;
    cb.fxShock = std::clamp(p.fxShock, 0.0f, 1.0f);
    cb.fxEdge = std::clamp(p.fxEdge, 0.0f, 1.0f);
    cb.fxHeat = std::clamp(p.fxHeat, 0.0f, 1.0f);
    cb.fxMaterialise = std::clamp(p.fxMaterialise, 0.0f, 1.0f);
    cb.edgeGain = 4.0f;
    if (m_needReset)
        m_bornTime = p.time;
    cb.bornTime = static_cast<float>(m_bornTime);
    const bool anyFx = cb.fxShock > 0.0f || cb.fxEdge > 0.0f || cb.fxHeat > 0.0f || cb.fxMaterialise > 0.0f;
    cb.motion = std::clamp(p.motion, 0.25f, 4.0f);
    // vividness recolours; the exact contract forbids that, so faithful
    // (solidity 1, no effect) pins it to 1 whatever the profile says
    cb.vivid = (cb.solidity >= 0.999f && !anyFx) ? 1.0f : std::clamp(p.vivid, 0.5f, 2.0f);
    cb.lightMode = p.lightMode ? 1.0f : 0.0f;
    cb.hdrBoost = p.hdrBoost;
    cb.audioWind = p.audioWind;
    cb.fbW = m_layout.fbW;
    cb.fbH = m_layout.fbH;
    cb.density = m_layout.density;
    cb.stride = m_layout.stride;
    cb.particleCount = m_layout.particles;
    cb.animStyle = p.reducedMotion ? 0u : p.animStyle;
    cb.reducedMotion = p.reducedMotion ? 1u : 0u;
    // faithful — the exact-pixel contract — is solidity 1 with no effect on;
    // an effect displaces or recolours particles by design
    cb.faithful = (cb.solidity >= 0.999f && !anyFx) ? 1u : 0u;
    cb.resetFlag = m_needReset ? 1u : 0u;
    cb.sampledW = m_layout.sampledW;
    cb.sampledH = m_layout.sampledH;
    cb.cursorCount = p.showCursor ? kCursorParticles : 0u;
    cb.brightness = p.brightness;
    cb.dragAmt = p.dragAmt;
    cb.cursorX = p.cursorX;
    cb.cursorY = p.cursorY;
    cb.cursorScale = 3.0f + 4.0f * (1.0f - cb.solidity);
    cb.cursorW = static_cast<float>(m_cursorShape.width);
    cb.cursorH = static_cast<float>(m_cursorShape.height);
    m_lastFaithful = cb.faithful != 0;
    m_lastLight = p.lightMode;
    m_lastInstances = cb.particleCount + cb.cursorCount;

    UploadRing::Alloc a = frame.ring.Allocate(sizeof(DesktopCB), 256);
    if (!a.cpu)
        return;
    std::memcpy(a.cpu, &cb, sizeof cb);
    m_cbGpu = a.gpu;

    // ---- energy: decay + inject ----------------------------------------------
    Transition(cl, m_inject.Get(), m_injectState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, m_energy.Get(), m_energyState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(m_energyRS.Get());
    cl->SetPipelineState(m_energyPSO.Get());
    cl->SetComputeRootConstantBufferView(0, m_cbGpu);
    cl->SetComputeRootDescriptorTable(1, m_dev->SrvGpu(m_slotEnergyUav));
    cl->Dispatch((m_layout.sampledW + 15) / 16, (m_layout.sampledH + 15) / 16, 1);
    {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = m_energy.Get();
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[1].UAV.pResource = m_inject.Get();
        cl->ResourceBarrier(2, b);
    }

    // ---- particles --------------------------------------------------------------
    Transition(cl, m_particles.Get(), m_particleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(m_simRS.Get());
    cl->SetPipelineState(m_simPSO.Get());
    cl->SetComputeRootConstantBufferView(0, m_cbGpu);
    cl->SetComputeRootUnorderedAccessView(1, m_particles->GetGPUVirtualAddress());
    cl->SetComputeRootDescriptorTable(2, m_dev->SrvGpu(m_slotEnergyUav));
    cl->Dispatch((m_lastInstances + 255) / 256, 1, 1);
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = m_particles.Get();
        cl->ResourceBarrier(1, &b);
    }
    Transition(cl, m_particles.Get(), m_particleState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // the draw reads the framebuffer, the cursor and the energy in the vertex stage
    Transition(cl, m_energy.Get(), m_energyState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cl, m_frame.Get(), m_frameState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(cl, m_cursor.Get(), m_cursorState,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_needReset = false;
}

void DesktopParticles::Draw(ID3D12GraphicsCommandList* cl)
{
    if (!Ready() || m_lastInstances == 0)
        return;
    m_dev->Stamp(cl, Device::StampDesktopDrawBegin);
    cl->SetGraphicsRootSignature(m_drawRS.Get());
    cl->SetPipelineState((m_lastFaithful || m_lastLight) ? m_drawOver.Get() : m_drawAdd.Get());
    cl->SetGraphicsRootConstantBufferView(0, m_cbGpu);
    cl->SetGraphicsRootShaderResourceView(1, m_particles->GetGPUVirtualAddress());
    cl->SetGraphicsRootDescriptorTable(2, m_dev->SrvGpu(m_slotFrameSrv));
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(6, m_lastInstances, 0, 0);
    m_dev->Stamp(cl, Device::StampDesktopEnd);
}

bool DesktopParticles::ReadbackEnergy(std::vector<float>& out, uint32_t& w, uint32_t& h)
{
    if (!Ready() || !m_energy)
        return false;
    w = m_layout.sampledW;
    h = m_layout.sampledH;
    std::vector<uint8_t> raw;
    if (!m_dev->ReadbackTexture(m_energy.Get(), m_energyState, DXGI_FORMAT_R16_FLOAT, w, h, 2, raw))
        return false;
    out.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size(); ++i)
    {
        // IEEE half to float
        const uint16_t v = static_cast<uint16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
        const uint32_t sign = (v >> 15) & 1u, exp = (v >> 10) & 0x1Fu, mant = v & 0x3FFu;
        float f;
        if (exp == 0)
            f = std::ldexp(static_cast<float>(mant), -24);
        else if (exp == 31)
            f = mant ? NAN : INFINITY;
        else
            f = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
        out[i] = sign ? -f : f;
    }
    return true;
}
