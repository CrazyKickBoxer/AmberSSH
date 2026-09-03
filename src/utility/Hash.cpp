#include "Hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <cstdio>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace amber
{

namespace
{

std::string ToHex(const unsigned char* p, size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    return out;
}

bool IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

Sha256::Sha256()
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return;
    m_alg = alg;
    DWORD objLen = 0, got = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0) != 0)
        return;
    m_obj.resize(objLen);
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, reinterpret_cast<PUCHAR>(m_obj.data()), objLen,
                         nullptr, 0, 0) != 0)
        return;
    m_hash = h;
    m_ok = true;
}

Sha256::~Sha256()
{
    if (m_hash)
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
    if (m_alg)
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_alg), 0);
}

void Sha256::Update(const void* data, size_t len)
{
    if (!m_ok || m_done || len == 0)
        return;
    // BCryptHashData takes a ULONG, so a buffer larger than 4 GB is fed in
    // chunks rather than truncated.
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (len > 0)
    {
        const ULONG chunk = static_cast<ULONG>(
            len > 0x40000000u ? 0x40000000u : len);
        if (BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(m_hash),
                           const_cast<PUCHAR>(p), chunk, 0) != 0)
        {
            m_ok = false;
            return;
        }
        p += chunk;
        len -= chunk;
    }
}

std::string Sha256::HexDigest()
{
    if (!m_ok || m_done)
        return {};
    unsigned char digest[32] = {};
    if (BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash), digest,
                         sizeof(digest), 0) != 0)
    {
        m_ok = false;
        return {};
    }
    m_done = true;
    return ToHex(digest, sizeof(digest));
}

bool Sha256Bytes(const void* data, size_t len, std::string& hexOut)
{
    Sha256 h;
    if (!h.Ok())
        return false;
    h.Update(data, len);
    hexOut = h.HexDigest();
    return !hexOut.empty();
}

namespace
{

bool HashStream(FILE* f, uint64_t length, bool wholeFile, std::string& hexOut,
                std::string& err)
{
    Sha256 h;
    if (!h.Ok())
    {
        err = "the system SHA-256 provider is unavailable";
        return false;
    }
    std::vector<char> buf(256 * 1024);
    uint64_t left = length;
    for (;;)
    {
        size_t want = buf.size();
        if (!wholeFile)
        {
            if (left == 0)
                break;
            if (left < want)
                want = static_cast<size_t>(left);
        }
        const size_t n = fread(buf.data(), 1, want, f);
        if (n == 0)
        {
            if (ferror(f))
            {
                err = "read error while hashing";
                return false;
            }
            if (!wholeFile && left > 0)
            {
                // The range asked for is not all there. Returning a digest of
                // what WAS there would look like a digest of the range.
                err = "the file is shorter than the range to hash";
                return false;
            }
            break;
        }
        h.Update(buf.data(), n);
        if (!wholeFile)
            left -= n;
    }
    if (!h.Ok())
    {
        err = "hashing failed";
        return false;
    }
    hexOut = h.HexDigest();
    return !hexOut.empty();
}

} // namespace

bool Sha256File(const std::wstring& path, std::string& hexOut, std::string& err)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
    {
        err = "cannot open the local file to hash it";
        return false;
    }
    const bool ok = HashStream(f, 0, true, hexOut, err);
    fclose(f);
    return ok;
}

bool Sha256FileRange(const std::wstring& path, uint64_t offset, uint64_t length,
                     std::string& hexOut, std::string& err)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f)
    {
        err = "cannot open the local file to hash it";
        return false;
    }
    if (_fseeki64(f, static_cast<__int64>(offset), SEEK_SET) != 0)
    {
        fclose(f);
        err = "cannot seek to the range to hash";
        return false;
    }
    const bool ok = HashStream(f, length, false, hexOut, err);
    fclose(f);
    return ok;
}

bool ParseRemoteSha256(const std::string& commandOutput, std::string& hexOut)
{
    hexOut.clear();
    // Look for a run of exactly 64 hex characters that is not part of a
    // longer token. Every layout sha256sum, shasum and BSD sha256 use puts
    // the digest in its own word, and a banner or a warning line has no
    // 64-character hex run in it.
    const std::string& s = commandOutput;
    size_t i = 0;
    while (i < s.size())
    {
        if (!IsHex(s[i]))
        {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < s.size() && IsHex(s[i]))
            ++i;
        const size_t len = i - start;
        if (len != 64)
            continue;
        // Must be a whole token: a 64-hex run glued to other characters is
        // not a digest.
        const bool leftOk = start == 0 || !isalnum(static_cast<unsigned char>(s[start - 1]));
        const bool rightOk = i >= s.size() || !isalnum(static_cast<unsigned char>(s[i]));
        if (!leftOk || !rightOk)
            continue;
        hexOut = s.substr(start, 64);
        for (char& c : hexOut)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return true;
    }
    return false;
}

bool DigestsMatch(const std::string& a, const std::string& b)
{
    auto clean = [](const std::string& s) {
        std::string o;
        for (char c : s)
            if (!isspace(static_cast<unsigned char>(c)))
                o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return o;
    };
    const std::string x = clean(a), y = clean(b);
    // Two empty digests are not a match: "we could not hash either end" must
    // never read as "they agree".
    if (x.empty() || y.empty() || x.size() != 64 || y.size() != 64)
        return false;
    return x == y;
}

} // namespace amber
