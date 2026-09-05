// device.h — D3D12 device, direct + copy queues, DXGI 1.6 flip-model swapchain,
// HDR (scRGB) detection/setup, descriptor allocation, frame synchronization.
#pragma once

#include "../common.h"
#include "frame.h"

class Device
{
public:
    bool Init(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    // Frame lifecycle -----------------------------------------------------
    // Waits for this frame slot's fence, resets allocator + ring + list.
    ID3D12GraphicsCommandList* BeginFrame();
    void EndFrame(bool vsync);
    void WaitIdle();

    void Resize(uint32_t width, uint32_t height);
    // Re-evaluates HDR support of the current output. Returns true if the
    // backbuffer format/colorspace changed (callers must rebuild PSOs).
    bool UpdateColorSpace();

    // Accessors -----------------------------------------------------------
    ID3D12Device*        Dev() const { return m_device.Get(); }
    ID3D12CommandQueue*  DirectQueue() const { return m_directQueue.Get(); }
    ID3D12CommandQueue*  CopyQueue() const { return m_copyQueue.Get(); }
    uint32_t             FrameIndex() const { return m_frameIndex; }
    FrameContext&        Frame() { return m_frames[m_frameIndex]; }
    ID3D12Resource*      BackBuffer() const { return m_backBuffers[m_backIndex].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV() const;
    DXGI_FORMAT          BackBufferFormat() const { return m_backFormat; }
    bool                 HdrActive() const { return m_hdrActive; }
    float                MaxNits() const { return m_maxNits; }
    uint32_t             Width() const { return m_width; }
    uint32_t             Height() const { return m_height; }
    bool                 TearingSupported() const { return m_allowTearing; }
    // The adapter's dedicated video memory, as DXGI reports it; 0 when the
    // adapter did not say. The desktop particle budget is derived from it.
    uint64_t             VideoMemoryBytes() const { return m_videoMemory; }
    // The adapter's name as DXGI reports it, for measurements that must say
    // what hardware produced them.
    const std::wstring&  AdapterName() const { return m_adapterName; }
    // Debug builds: every message the D3D12 debug layer has stored, appended
    // to `out` one per line and cleared; the count is returned. Release
    // builds return -1 and append a line saying there is no debug layer.
    int DrainDebugMessages(std::string& out);
    // Debug builds break into the debugger on an error-severity message,
    // which with no debugger attached ends the process before anything is
    // reported. The self-check turns that off so the messages reach its
    // report instead; a no-op in release builds.
    void SetDebugBreaks(bool on);

    // Descriptor heaps ----------------------------------------------------
    ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap.Get(); }
    uint32_t AllocSrv();                       // slot in shader-visible heap
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(uint32_t slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(uint32_t slot) const;
    uint32_t SrvSlotFromCpu(D3D12_CPU_DESCRIPTOR_HANDLE h) const
    {
        return static_cast<uint32_t>(
            (h.ptr - m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) /
            m_srvStride);
    }
    uint32_t AllocRtv();
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(uint32_t slot) const;

    // Synchronous readback of a 2D texture's first subresource, for the
    // self-checks: waits for the GPU, copies through a readback heap, waits
    // again, and returns the rows tightly packed at `bytesPerPixel`. The
    // resource is returned to `state`. Slow by design and never per frame.
    bool ReadbackTexture(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, DXGI_FORMAT format,
                         uint32_t width, uint32_t height, uint32_t bytesPerPixel,
                         std::vector<uint8_t>& out);
    // Copy-queue fence, used for glyph-point uploads between frames.
    uint64_t SignalCopy();
    void     DirectWaitCopy(uint64_t value);

    // GPU timestamps per frame: frame begin/end, bloom begin/end, and the
    // VNC desktop pass's upload / simulate / draw boundaries.
    enum StampSlot { StampFrameBegin = 0, StampBloomBegin, StampBloomEnd,
                     StampFrameEnd,
                     StampDesktopBegin, StampDesktopSimBegin, StampDesktopDrawBegin,
                     StampDesktopEnd, StampCount };
    void  Stamp(ID3D12GraphicsCommandList* cl, StampSlot slot);
    float GpuFrameMs() const { return m_gpuFrameMs; }
    float GpuBloomMs() const { return m_gpuBloomMs; }
    // Zero on frames without a desktop pass.
    float GpuDesktopUploadMs() const { return m_gpuDeskUploadMs; }
    float GpuDesktopSimMs() const { return m_gpuDeskSimMs; }
    float GpuDesktopDrawMs() const { return m_gpuDeskDrawMs; }

private:
    void CreateSwapchainRTVs();

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0;
    uint64_t m_videoMemory = 0;
    std::wstring m_adapterName;

    ComPtr<IDXGIFactory6>       m_factory;
    ComPtr<ID3D12Device>        m_device;
    ComPtr<ID3D12CommandQueue>  m_directQueue;
    ComPtr<ID3D12CommandQueue>  m_copyQueue;
    ComPtr<IDXGISwapChain4>     m_swapchain;
    ComPtr<ID3D12Resource>      m_backBuffers[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    uint32_t m_srvNext = 0, m_rtvNext = 0;
    uint32_t m_srvStride = 0, m_rtvStride = 0;
    uint32_t m_backRtvSlots[kFramesInFlight] = {};

    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceLast = 0;
    ComPtr<ID3D12Fence> m_copyFence;
    uint64_t m_copyFenceLast = 0;

    FrameContext m_frames[kFramesInFlight];
    uint32_t m_frameIndex = 0;
    uint32_t m_backIndex = 0;

    DXGI_FORMAT m_backFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    bool  m_hdrActive = false;
    float m_maxNits = 400.0f;
    bool  m_allowTearing = false;

    // GPU timing
    ComPtr<ID3D12QueryHeap> m_tsHeap;
    ComPtr<ID3D12Resource>  m_tsReadback;
    uint64_t m_tsFrequency = 0;
    float m_gpuFrameMs = 0.0f;
    float m_gpuBloomMs = 0.0f;
    float m_gpuDeskUploadMs = 0.0f, m_gpuDeskSimMs = 0.0f, m_gpuDeskDrawMs = 0.0f;
    // Which slots this frame actually stamped: only those are resolved.
    // Resolving a query that was never performed is a debug-layer error,
    // and the desktop slots are written only on desktop frames.
    uint32_t m_stamped = 0;
};
