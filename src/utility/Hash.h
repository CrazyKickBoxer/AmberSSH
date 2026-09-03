// Hash.h — SHA-256 for post-transfer verification.
//
// Uses the OS provider (bcrypt) rather than a bundled implementation: this
// has to agree byte for byte with whatever `sha256sum` on the far end
// produces, and the platform's own primitive is the one least likely to be
// subtly wrong.
//
// Streaming, because the files being verified are the files being
// transferred: hashing a 4 GB download must not need 4 GB of memory.
#pragma once

#include <cstdint>
#include <string>

namespace amber
{

class Sha256
{
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Ok() const { return m_ok; }
    void Update(const void* data, size_t len);
    // Lower-case hex, 64 characters. Empty when the provider failed.
    std::string HexDigest();

private:
    void* m_alg = nullptr;      // BCRYPT_ALG_HANDLE
    void* m_hash = nullptr;     // BCRYPT_HASH_HANDLE
    std::string m_obj;          // provider scratch
    bool m_ok = false;
    bool m_done = false;
};

// Hashes a local file. Returns false and sets `err` on any read failure —
// never a partial digest, because a digest of part of a file that looks like
// a digest of the file is the worst possible outcome here.
bool Sha256File(const std::wstring& path, std::string& hexOut, std::string& err);

// Hashes a byte range of a local file, for verifying a resumed transfer's
// existing prefix against the source's.
bool Sha256FileRange(const std::wstring& path, uint64_t offset, uint64_t length,
                     std::string& hexOut, std::string& err);

bool Sha256Bytes(const void* data, size_t len, std::string& hexOut);

// Pulls a 64-character hex digest out of whatever a remote command printed.
// `sha256sum` writes "<hex>  <name>", `shasum -a 256` the same, BSD `sha256`
// writes "SHA256 (name) = <hex>", and some wrappers add a banner — so the
// parse looks for the digest rather than assuming a layout. Returns false
// when the output contains no digest, which is how "the remote has no
// sha256sum" is reported instead of guessed at.
bool ParseRemoteSha256(const std::string& commandOutput, std::string& hexOut);

// Case-insensitive comparison of two hex digests, tolerating whitespace.
bool DigestsMatch(const std::string& a, const std::string& b);

} // namespace amber
