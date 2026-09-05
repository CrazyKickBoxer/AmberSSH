#include "device.h"

bool Device::Init(HWND hwnd, uint32_t width, uint32_t height)
{
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    UINT factoryFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
                debug1->SetEnableGPUBasedValidation(TRUE);
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)),
                  "CreateDXGIFactory2");

    // Tearing (VRR / uncapped present) support.
    {
        ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(m_factory.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                              &allow, sizeof(allow))))
            m_allowTearing = allow == TRUE;
    }

    // High-performance adapter.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         SUCCEEDED(m_factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&m_device))))
        {
            m_videoMemory = desc.DedicatedVideoMemory;
            break;
        }
        m_device.Reset();
    }
    if (!m_device)
        return false;
    m_device->SetName(L"AmberSSH Device");

#ifdef _DEBUG
    {
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(m_device.As(&iq)))
        {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }
#endif

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_directQueue)),
                  "direct queue");
    m_directQueue->SetName(L"DirectQueue");

    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_copyQueue)),
                  "copy queue");
    m_copyQueue->SetName(L"CopyQueue");

    // Descriptor heaps.
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 128;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)),
                      "srv heap");
        m_srvStride = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 16;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)),
                      "rtv heap");
        m_rtvStride = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        m_backRtvSlots[i] = AllocRtv();

    // Start SDR; UpdateColorSpace() upgrades to HDR right after the swapchain
    // exists (it needs the containing output to decide).

    // Swapchain (flip model, 3 buffers, tearing-capable).
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = width;
        sd.Height = height;
        sd.Format = m_backFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFramesInFlight;
        sd.Scaling = DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> sc1;
        ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
                          m_directQueue.Get(), hwnd, &sd, nullptr, nullptr, &sc1),
                      "CreateSwapChainForHwnd");
        ThrowIfFailed(sc1.As(&m_swapchain), "IDXGISwapChain4");
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    // Frames.
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(
                          D3D12_COMMAND_LIST_TYPE_DIRECT,
                          IID_PPV_ARGS(&m_frames[i].allocator)),
                      "frame allocator");
        m_frames[i].ring.Init(m_device.Get(), 8ull * 1024 * 1024);
    }
    ThrowIfFailed(m_device->CreateCommandList(
                      0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                      m_frames[0].allocator.Get(), nullptr,
                      IID_PPV_ARGS(&m_cmdList)),
                  "command list");
    m_cmdList->Close();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&m_fence)), "fence");
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&m_copyFence)), "copy fence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // GPU timestamp machinery: 4 slots per frame in flight.
    {
        D3D12_QUERY_HEAP_DESC qh = {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = StampCount * kFramesInFlight;
        ThrowIfFailed(m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_tsHeap)),
                      "timestamp heap");
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(uint64_t) * StampCount * kFramesInFlight;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_device->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                          IID_PPV_ARGS(&m_tsReadback)),
                      "timestamp readback");
        m_directQueue->GetTimestampFrequency(&m_tsFrequency);
    }

    CreateSwapchainRTVs();
    UpdateColorSpace();   // may switch to HDR fp16 buffers
    return true;
}

void Device::Stamp(ID3D12GraphicsCommandList* cl, StampSlot slot)
{
    if (!m_tsHeap)
        return;
    cl->EndQuery(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                 m_frameIndex * StampCount + slot);
}

void Device::Shutdown()
{
    if (m_device)
        WaitIdle();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void Device::CreateSwapchainRTVs()
{
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        ThrowIfFailed(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])),
                      "swapchain GetBuffer");
        m_backBuffers[i]->SetName(L"BackBuffer");
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr,
                                         RtvCpu(m_backRtvSlots[i]));
    }
    m_backIndex = m_swapchain->GetCurrentBackBufferIndex();
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::BackBufferRTV() const
{
    return RtvCpu(m_backRtvSlots[m_backIndex]);
}

uint32_t Device::AllocSrv()
{
    if (m_srvNext >= 128)
        throw std::runtime_error("SRV heap exhausted");
    return m_srvNext++;
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::SrvCpu(uint32_t slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_srvStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Device::SrvGpu(uint32_t slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * m_srvStride;
    return h;
}

uint32_t Device::AllocRtv()
{
    if (m_rtvNext >= 16)
        throw std::runtime_error("RTV heap exhausted");
    return m_rtvNext++;
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::RtvCpu(uint32_t slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_rtvStride;
    return h;
}

ID3D12GraphicsCommandList* Device::BeginFrame()
{
    FrameContext& f = m_frames[m_frameIndex];
    if (m_fence->GetCompletedValue() < f.fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(f.fenceValue, m_fenceEvent),
                      "fence wait");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    f.ring.Reset();
    ThrowIfFailed(f.allocator->Reset(), "allocator reset");
    ThrowIfFailed(m_cmdList->Reset(f.allocator.Get(), nullptr), "cmdlist reset");
    m_backIndex = m_swapchain->GetCurrentBackBufferIndex();

    // This slot's previous timestamps completed with its fence — read them.
    if (m_tsReadback && m_tsFrequency)
    {
        uint64_t vals[StampCount] = {};
        D3D12_RANGE range = {
            sizeof(uint64_t) * StampCount * m_frameIndex,
            sizeof(uint64_t) * StampCount * (m_frameIndex + 1)
        };
        void* mapped = nullptr;
        if (SUCCEEDED(m_tsReadback->Map(0, &range, &mapped)))
        {
            memcpy(vals,
                   static_cast<uint8_t*>(mapped) + range.Begin,
                   sizeof(vals));
            D3D12_RANGE none = { 0, 0 };
            m_tsReadback->Unmap(0, &none);
            double toMs = 1000.0 / static_cast<double>(m_tsFrequency);
            if (vals[StampFrameEnd] > vals[StampFrameBegin])
                m_gpuFrameMs = static_cast<float>(
                    (vals[StampFrameEnd] - vals[StampFrameBegin]) * toMs);
            if (vals[StampBloomEnd] > vals[StampBloomBegin])
                m_gpuBloomMs = static_cast<float>(
                    (vals[StampBloomEnd] - vals[StampBloomBegin]) * toMs);
            // The desktop slots are only written on frames with a desktop
            // pass. On any other frame they hold whatever an earlier use of
            // this slot left, so they count only when all four sit in order
            // inside this frame's own begin/end — a stale set never does.
            const bool desk = vals[StampDesktopBegin] >= vals[StampFrameBegin] &&
                              vals[StampDesktopSimBegin] >= vals[StampDesktopBegin] &&
                              vals[StampDesktopDrawBegin] >= vals[StampDesktopSimBegin] &&
                              vals[StampDesktopEnd] >= vals[StampDesktopDrawBegin] &&
                              vals[StampDesktopEnd] <= vals[StampFrameEnd];
            m_gpuDeskUploadMs = desk ? static_cast<float>((vals[StampDesktopSimBegin] - vals[StampDesktopBegin]) * toMs) : 0.0f;
            m_gpuDeskSimMs = desk ? static_cast<float>((vals[StampDesktopDrawBegin] - vals[StampDesktopSimBegin]) * toMs) : 0.0f;
            m_gpuDeskDrawMs = desk ? static_cast<float>((vals[StampDesktopEnd] - vals[StampDesktopDrawBegin]) * toMs) : 0.0f;
        }
    }

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_cmdList->SetDescriptorHeaps(1, heaps);
    return m_cmdList.Get();
}

void Device::EndFrame(bool vsync)
{
    if (m_tsHeap)
        m_cmdList->ResolveQueryData(
            m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            m_frameIndex * StampCount, StampCount, m_tsReadback.Get(),
            sizeof(uint64_t) * StampCount * m_frameIndex);
    ThrowIfFailed(m_cmdList->Close(), "cmdlist close");
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_directQueue->ExecuteCommandLists(1, lists);

    UINT flags = (!vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = m_swapchain->Present(vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        throw std::runtime_error("D3D12 device removed");

    m_frames[m_frameIndex].fenceValue = ++m_fenceLast;
    m_directQueue->Signal(m_fence.Get(), m_fenceLast);
    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

void Device::WaitIdle()
{
    if (!m_directQueue || !m_fence)
        return;
    ++m_fenceLast;
    m_directQueue->Signal(m_fence.Get(), m_fenceLast);
    if (m_fence->GetCompletedValue() < m_fenceLast)
    {
        m_fence->SetEventOnCompletion(m_fenceLast, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    // Copy queue too.
    ++m_copyFenceLast;
    m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceLast);
    if (m_copyFence->GetCompletedValue() < m_copyFenceLast)
    {
        m_copyFence->SetEventOnCompletion(m_copyFenceLast, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void Device::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;
    WaitIdle();
    for (auto& bb : m_backBuffers)
        bb.Reset();
    ThrowIfFailed(m_swapchain->ResizeBuffers(
                      kFramesInFlight, width, height, m_backFormat,
                      m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                  "ResizeBuffers");
    m_width = width;
    m_height = height;
    CreateSwapchainRTVs();
}

bool Device::UpdateColorSpace()
{
    if (!m_swapchain)
        return false;

    bool wantHdr = false;
    float maxNits = 400.0f;

    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(m_swapchain->GetContainingOutput(&output)))
    {
        ComPtr<IDXGIOutput6> output6;
        if (SUCCEEDED(output.As(&output6)))
        {
            DXGI_OUTPUT_DESC1 desc;
            if (SUCCEEDED(output6->GetDesc1(&desc)))
            {
                wantHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                maxNits = desc.MaxLuminance > 0 ? desc.MaxLuminance : 400.0f;
            }
        }
    }

    DXGI_FORMAT wantFormat = wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                     : DXGI_FORMAT_R10G10B10A2_UNORM;
    DXGI_COLOR_SPACE_TYPE wantSpace = wantHdr
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709      // scRGB, linear
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    UINT support = 0;
    if (FAILED(m_swapchain->CheckColorSpaceSupport(wantSpace, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        wantHdr = false;
        wantFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
        wantSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    bool changed = (wantFormat != m_backFormat) || (wantHdr != m_hdrActive);
    m_maxNits = maxNits;
    if (!changed)
        return false;

    WaitIdle();
    for (auto& bb : m_backBuffers)
        bb.Reset();
    m_backFormat = wantFormat;
    ThrowIfFailed(m_swapchain->ResizeBuffers(
                      kFramesInFlight, m_width, m_height, m_backFormat,
                      m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
                  "ResizeBuffers (colorspace)");
    m_swapchain->SetColorSpace1(wantSpace);
    m_hdrActive = wantHdr;
    CreateSwapchainRTVs();
    return true;
}

uint64_t Device::SignalCopy()
{
    ++m_copyFenceLast;
    m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceLast);
    return m_copyFenceLast;
}

void Device::DirectWaitCopy(uint64_t value)
{
    m_directQueue->Wait(m_copyFence.Get(), value);
}
