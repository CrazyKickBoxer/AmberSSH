#include "frame.h"

bool UploadRing::AddBlock(uint64_t sizeBytes)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeBytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Block b;
    b.size = sizeBytes;
    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&b.buffer))))
        return false;
    b.buffer->SetName(L"UploadRing");

    D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(b.buffer->Map(0, &noRead, reinterpret_cast<void**>(&b.mapped))))
        return false;
    // A second block means one frame asked for more upload memory than the
    // ring was sized for. Rare and self-limiting, but worth a line in the
    // temp directory so an oversized frame can be traced afterwards.
    if (!m_blocks.empty())
    {
        wchar_t tmp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmp))
        {
            std::wstring path = std::wstring(tmp) + L"amber-upload.txt";
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f)
            {
                fprintf(f, "upload ring block %zu: %llu bytes (base %llu)\n",
                        m_blocks.size() + 1,
                        static_cast<unsigned long long>(sizeBytes),
                        static_cast<unsigned long long>(m_blockSize));
                fclose(f);
            }
        }
    }
    m_blocks.push_back(std::move(b));
    return true;
}

void UploadRing::Init(ID3D12Device* device, uint64_t sizeBytes)
{
    m_device = device;
    m_blockSize = sizeBytes;
    m_blocks.clear();
    m_block = 0;
    m_head = 0;
    if (!AddBlock(sizeBytes))
        throw std::runtime_error("UploadRing buffer");
}

UploadRing::Alloc UploadRing::Allocate(uint64_t sizeBytes, uint64_t align)
{
    uint64_t start = AlignUp(m_head, align);
    // Walk to a block with room, making one if this frame has outgrown what
    // we have. A single request larger than the block size gets its own.
    while (m_block >= m_blocks.size() || start + sizeBytes > m_blocks[m_block].size)
    {
        if (m_block + 1 >= m_blocks.size())
        {
            uint64_t want = m_blockSize;
            while (want < sizeBytes + align)
                want *= 2;
            if (!AddBlock(want))
                throw std::runtime_error("UploadRing exhausted — frame upload too large");
        }
        ++m_block;
        m_head = 0;
        start = 0;
    }
    m_head = start + sizeBytes;

    Block& b = m_blocks[m_block];
    Alloc a;
    a.cpu = b.mapped + start;
    a.gpu = b.buffer->GetGPUVirtualAddress() + start;
    a.resource = b.buffer.Get();
    a.offset = start;
    return a;
}
