// SecureString.h — heap buffer for passwords and passphrases that is zeroed
// on destruction. Not swap-proof; it narrows the window in which a secret is
// recoverable from process memory, and guarantees we never leave copies behind
// in a std::string that reallocated.
#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace amber
{

class SecureString
{
public:
    SecureString() = default;
    explicit SecureString(std::string_view text);
    SecureString(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(const SecureString& other);
    SecureString& operator=(SecureString&& other) noexcept;
    ~SecureString();

    void Assign(std::string_view text);
    void Clear() noexcept;               // zeroes then frees

    bool Empty() const noexcept { return m_size == 0; }
    std::size_t Size() const noexcept { return m_size; }

    // Borrowed view. Valid until the next mutation. Never store the pointer.
    const char* Data() const noexcept { return m_data ? m_data : ""; }
    std::string_view View() const noexcept { return { Data(), m_size }; }

    // Materialises a std::string copy. The caller becomes responsible for a
    // secret that is no longer protected — used only at the libssh2 boundary.
    std::string Reveal() const { return std::string(Data(), m_size); }

private:
    void Allocate(std::size_t size);

    char* m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
};

// Zeroes a std::string's buffer in place before clearing it.
void ScrubString(std::string& value) noexcept;

} // namespace amber
