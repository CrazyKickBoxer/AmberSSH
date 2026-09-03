// ring.h — lock-free single-producer/single-consumer byte ring buffer.
// Producer: SSH network thread. Consumer: main/parser thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

class SpscRing
{
public:
    explicit SpscRing(size_t capacityPow2 = 1u << 20)
        : m_buf(capacityPow2), m_mask(capacityPow2 - 1)
    {
        // capacity must be a power of two
    }

    // Producer side. Returns bytes actually written (drops on overflow).
    size_t Push(const uint8_t* data, size_t len)
    {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t tail = m_tail.load(std::memory_order_acquire);
        size_t space = m_buf.size() - (head - tail);
        size_t n = len < space ? len : space;
        for (size_t i = 0; i < n; ++i)
            m_buf[(head + i) & m_mask] = data[i];
        m_head.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer side.
    size_t Available() const
    {
        return m_head.load(std::memory_order_acquire) -
               m_tail.load(std::memory_order_relaxed);
    }

    size_t Pop(uint8_t* out, size_t maxLen)
    {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        size_t avail = head - tail;
        size_t n = maxLen < avail ? maxLen : avail;
        for (size_t i = 0; i < n; ++i)
            out[i] = m_buf[(tail + i) & m_mask];
        m_tail.store(tail + n, std::memory_order_release);
        return n;
    }

    void Clear()
    {
        m_tail.store(m_head.load(std::memory_order_acquire),
                     std::memory_order_release);
    }

private:
    std::vector<uint8_t> m_buf;
    size_t m_mask;
    std::atomic<size_t> m_head{ 0 };
    std::atomic<size_t> m_tail{ 0 };
};
