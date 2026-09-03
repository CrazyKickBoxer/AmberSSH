// frame.h — per-frame resources: command allocator, fence value, and a
// persistent-mapped linear upload ring (zero per-frame heap allocations).
#pragma once

#include "../common.h"

class UploadRing
{
public:
    struct Alloc
    {
        uint8_t*                  cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        ID3D12Resource*           resource = nullptr;
        uint64_t                  offset = 0;
    };

    void Init(ID3D12Device* device, uint64_t sizeBytes);
    void Reset() { m_head = 0; }
    Alloc Allocate(uint64_t sizeBytes, uint64_t align = 256);
    uint64_t Capacity() const { return m_size; }
    uint64_t Used() const { return m_head; }

private:
    ComPtr<ID3D12Resource> m_buffer;
    uint8_t* m_mapped = nullptr;
    uint64_t m_size = 0;
    uint64_t m_head = 0;
};

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    UploadRing ring;
    uint64_t fenceValue = 0;
};
