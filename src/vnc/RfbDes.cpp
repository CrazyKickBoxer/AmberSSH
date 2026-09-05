// RfbDes.cpp — DES (FIPS 46-3) and the VNC Authentication key variant.
//
// Written from the standard's tables, bit by bit. Speed is irrelevant: the
// protocol uses this for sixteen bytes per connection. Clarity is not, since
// a permutation table with one wrong entry still produces confident-looking
// ciphertext — which is why VncDesTests.cpp checks the published FIPS 46
// known-answer vectors rather than round-tripping the code against itself.
#include "RfbDes.h"

#include <cstring>

namespace amber::vnc
{
namespace
{

// Tables are 1-based bit positions, MSB-first, exactly as FIPS 46-3 prints
// them, so they can be checked against the standard line by line.
constexpr uint8_t kIP[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,  60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,  64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7,
};
constexpr uint8_t kFP[64] = {
    40, 8, 48, 16, 56, 24, 64, 32,  39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,  37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,  35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,  33, 1, 41, 9,  49, 17, 57, 25,
};
constexpr uint8_t kE[48] = {
    32, 1,  2,  3,  4,  5,   4,  5,  6,  7,  8,  9,
    8,  9,  10, 11, 12, 13,  12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,  20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,  28, 29, 30, 31, 32, 1,
};
constexpr uint8_t kP[32] = {
    16, 7,  20, 21, 29, 12, 28, 17,  1,  15, 23, 26, 5,  18, 31, 10,
    2,  8,  24, 14, 32, 27, 3,  9,   19, 13, 30, 6,  22, 11, 4,  25,
};
constexpr uint8_t kPC1[56] = {
    57, 49, 41, 33, 25, 17, 9,   1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27,  19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,  7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29,  21, 13, 5,  28, 20, 12, 4,
};
constexpr uint8_t kPC2[48] = {
    14, 17, 11, 24, 1,  5,   3,  28, 15, 6,  21, 10,
    23, 19, 12, 4,  26, 8,   16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55,  30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,  46, 42, 50, 36, 29, 32,
};
constexpr uint8_t kShifts[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };

constexpr uint8_t kS[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
      0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
      15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
      3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
      13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
      13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
      1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
      13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
      3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
      14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
      11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
      10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
      4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
      13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
      6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
      1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
      2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
};

// One bit per byte, bit 1 of the standard = element 0, MSB of byte 0 first.
void ToBits(const uint8_t* in, int nbits, uint8_t* bits)
{
    for (int i = 0; i < nbits; ++i)
        bits[i] = static_cast<uint8_t>((in[i / 8] >> (7 - (i % 8))) & 1u);
}

void FromBits(const uint8_t* bits, int nbits, uint8_t* out)
{
    std::memset(out, 0, static_cast<size_t>(nbits / 8));
    for (int i = 0; i < nbits; ++i)
        out[i / 8] = static_cast<uint8_t>(out[i / 8] | (bits[i] << (7 - (i % 8))));
}

void Permute(const uint8_t* in, const uint8_t* table, int n, uint8_t* out)
{
    for (int i = 0; i < n; ++i)
        out[i] = in[table[i] - 1];
}

void KeySchedule(const uint8_t key[8], uint8_t subkeys[16][48])
{
    uint8_t kb[64], cd[56];
    ToBits(key, 64, kb);
    Permute(kb, kPC1, 56, cd);
    for (int r = 0; r < 16; ++r)
    {
        for (int s = 0; s < kShifts[r]; ++s)
        {
            // rotate C (0..27) and D (28..55) left by one, separately
            const uint8_t c0 = cd[0], d0 = cd[28];
            std::memmove(cd, cd + 1, 27);
            cd[27] = c0;
            std::memmove(cd + 28, cd + 29, 27);
            cd[55] = d0;
        }
        Permute(cd, kPC2, 48, subkeys[r]);
    }
}

void Feistel(const uint8_t R[32], const uint8_t K[48], uint8_t out[32])
{
    uint8_t e[48], s[32];
    Permute(R, kE, 48, e);
    for (int i = 0; i < 48; ++i)
        e[i] ^= K[i];
    for (int box = 0; box < 8; ++box)
    {
        const uint8_t* b = e + box * 6;
        const int row = (b[0] << 1) | b[5];
        const int col = (b[1] << 3) | (b[2] << 2) | (b[3] << 1) | b[4];
        const uint8_t v = kS[box][row * 16 + col];
        s[box * 4 + 0] = static_cast<uint8_t>((v >> 3) & 1);
        s[box * 4 + 1] = static_cast<uint8_t>((v >> 2) & 1);
        s[box * 4 + 2] = static_cast<uint8_t>((v >> 1) & 1);
        s[box * 4 + 3] = static_cast<uint8_t>(v & 1);
    }
    Permute(s, kP, 32, out);
}

uint8_t ReverseBits(uint8_t b)
{
    b = static_cast<uint8_t>(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = static_cast<uint8_t>(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = static_cast<uint8_t>(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

} // namespace

void DesEncryptBlock(const uint8_t key[8], const uint8_t in[8], uint8_t out[8])
{
    uint8_t subkeys[16][48];
    KeySchedule(key, subkeys);

    uint8_t bits[64], lr[64];
    ToBits(in, 64, bits);
    Permute(bits, kIP, 64, lr);
    uint8_t* L = lr;
    uint8_t* R = lr + 32;
    for (int r = 0; r < 16; ++r)
    {
        uint8_t f[32], newR[32];
        Feistel(R, subkeys[r], f);
        for (int i = 0; i < 32; ++i)
            newR[i] = static_cast<uint8_t>(L[i] ^ f[i]);
        std::memcpy(L, R, 32);
        std::memcpy(R, newR, 32);
    }
    // the final swap: pre-output is R16 L16
    uint8_t pre[64];
    std::memcpy(pre, R, 32);
    std::memcpy(pre + 32, L, 32);
    Permute(pre, kFP, 64, bits);
    FromBits(bits, 64, out);
}

void VncAuthKey(std::string_view password, uint8_t key[8])
{
    for (int i = 0; i < 8; ++i)
        key[i] = i < static_cast<int>(password.size())
                     ? ReverseBits(static_cast<uint8_t>(password[static_cast<size_t>(i)]))
                     : 0;
}

void VncAuthResponse(std::string_view password, const uint8_t challenge[16],
                     uint8_t response[16])
{
    uint8_t key[8];
    VncAuthKey(password, key);
    DesEncryptBlock(key, challenge, response);
    DesEncryptBlock(key, challenge + 8, response + 8);
    std::memset(key, 0, sizeof key);
}

} // namespace amber::vnc
