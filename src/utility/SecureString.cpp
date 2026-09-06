#include "SecureString.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#include <Windows.h>

namespace amber
{

void ScrubString(std::string& value) noexcept
{
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size());
    value.clear();
}

void ScrubWString(std::wstring& value) noexcept
{
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
}

SecureString::SecureString(std::string_view text) { Assign(text); }

SecureString::SecureString(const SecureString& other) { Assign(other.View()); }

SecureString::SecureString(SecureString&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr)),
      m_size(std::exchange(other.m_size, 0)),
      m_capacity(std::exchange(other.m_capacity, 0))
{
}

SecureString& SecureString::operator=(const SecureString& other)
{
    if (this != &other)
        Assign(other.View());
    return *this;
}

SecureString& SecureString::operator=(SecureString&& other) noexcept
{
    if (this != &other)
    {
        Clear();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_capacity = std::exchange(other.m_capacity, 0);
    }
    return *this;
}

SecureString::~SecureString() { Clear(); }

void SecureString::Allocate(std::size_t size)
{
    // +1 so Data() can always return a NUL-terminated buffer.
    char* buffer = static_cast<char*>(std::calloc(size + 1, 1));
    if (!buffer)
        throw std::bad_alloc();
    Clear();
    m_data = buffer;
    m_capacity = size + 1;
}

void SecureString::Assign(std::string_view text)
{
    if (text.empty())
    {
        Clear();
        return;
    }
    if (m_capacity < text.size() + 1)
        Allocate(text.size());
    else
    {
        // Reusing the buffer: zero it first. Copying a shorter secret over a
        // longer one otherwise leaves the tail of the old one in the block,
        // hidden from Size() but not from a memory dump, which is exactly the
        // window this class exists to close.
        SecureZeroMemory(m_data, m_capacity);
    }
    std::copy(text.begin(), text.end(), m_data);
    m_data[text.size()] = '\0';
    m_size = text.size();
}

void SecureString::Clear() noexcept
{
    if (m_data)
    {
        SecureZeroMemory(m_data, m_capacity);
        std::free(m_data);
        m_data = nullptr;
    }
    m_size = 0;
    m_capacity = 0;
}

} // namespace amber
