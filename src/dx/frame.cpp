#include "frame.h"

void UploadRing::Init(ID3D12Device* device, uint64_t sizeBytes)
{
    m_size = sizeBytes;
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

    ThrowIfFailed(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_buffer)), "UploadRing buffer");
    m_buffer->SetName(L"UploadRing");

    D3D12_RANGE noRead = { 0, 0 };
    ThrowIfFailed(m_buffer->Map(0, &noRead, reinterpret_cast<void**>(&m_mapped)),
                  "UploadRing map");
    m_head = 0;
}

UploadRing::Alloc UploadRing::Allocate(uint64_t sizeBytes, uint64_t align)
{
    uint64_t start = AlignUp(m_head, align);
    if (start + sizeBytes > m_size)
        throw std::runtime_error("UploadRing exhausted — frame upload too large");
    m_head = start + sizeBytes;

    Alloc a;
    a.cpu = m_mapped + start;
    a.gpu = m_buffer->GetGPUVirtualAddress() + start;
    a.resource = m_buffer.Get();
    a.offset = start;
    return a;
}
