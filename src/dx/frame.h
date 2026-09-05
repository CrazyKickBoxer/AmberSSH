// frame.h — per-frame resources: command allocator, fence value, and a
// persistent-mapped linear upload ring (zero per-frame heap allocations).
#pragma once

#include "../common.h"

// A linear allocator over persistently-mapped upload memory. It hands out a
// resource + offset rather than a bare pointer, so it is free to spill into a
// second block when a frame asks for more than the first one holds: a frame
// that uploads an unusually large picture costs a one-off allocation instead
// of killing the process. Blocks are kept once made, so the steady state is
// still zero allocations per frame.
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
    void Reset() { m_block = 0; m_head = 0; }
    Alloc Allocate(uint64_t sizeBytes, uint64_t align = 256);

    // The first block's size and the head within the current block. Callers
    // that choose between the ring and their own staging read these: they
    // describe the cheap path, not the hard limit.
    uint64_t Capacity() const { return m_blockSize; }
    uint64_t Used() const { return m_head; }
    // How many blocks this ring has had to make. 1 is the steady state.
    size_t   BlockCount() const { return m_blocks.size(); }

private:
    struct Block
    {
        ComPtr<ID3D12Resource> buffer;
        uint8_t*               mapped = nullptr;
        uint64_t               size = 0;
    };
    bool AddBlock(uint64_t sizeBytes);

    ID3D12Device*      m_device = nullptr;
    std::vector<Block> m_blocks;
    uint64_t           m_blockSize = 0;   // the size blocks are made at
    size_t             m_block = 0;       // block the head is in
    uint64_t           m_head = 0;        // offset within that block
};

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    UploadRing ring;
    uint64_t fenceValue = 0;
};
