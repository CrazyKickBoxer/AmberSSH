// VncDesTests.cpp — the DES core against published known-answer vectors, and
// the VNC key variant against its defining property.
//
// A DES with one wrong table entry still produces confident-looking output,
// so the core is checked against vectors that predate this program:
//
//   * FIPS 46 / "DES Example" (Grabbe's walkthrough, reproduced in NIST
//     SP 800-17): key 133457799BBCDFF1, plaintext 0123456789ABCDEF,
//     ciphertext 85E813540F0AB405.
//   * NIST SP 800-17 Table A.1 (variable-plaintext known answer, first
//     entry): key 0101010101010101, plaintext 8000000000000000,
//     ciphertext 95F8A5E5DD31D900.
//   * The all-zero key and block, listed in every DES test suite since
//     Schneier's: ciphertext 8CA64DE9C1B123A7.
//   * The all-ones key and block: ciphertext 7359B2163E4EDC58.
//
// The VNC variant has no published vector — RFC 6143 does not even document
// the bit reversal — so it is pinned by construction: the key for a password
// must equal the password's bytes with each byte's bits reversed, and the
// response must equal two DES blocks under that key. One genuine
// known-answer does follow from the core vectors: an empty password is the
// all-zero key, so an all-zero challenge must answer 8CA64DE9C1B123A7 twice.
// The interoperability step against a real server is where the variant's
// direction (rather than its self-consistency) gets its independent check.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include "vnc/RfbDes.h"

using namespace amber::vnc;

namespace
{

void Hex(const char* hex, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        unsigned v = 0;
        sscanf_s(hex + i * 2, "%2x", &v);
        out[i] = static_cast<uint8_t>(v);
    }
}

std::string ToHex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; ++i)
    {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

void CheckDes(const char* key, const char* pt, const char* ct)
{
    uint8_t k[8], p[8], c[8];
    Hex(key, k, 8);
    Hex(pt, p, 8);
    DesEncryptBlock(k, p, c);
    CHECK(ToHex(c, 8) == ct);
}

} // namespace

TEST_CASE("DES core matches the published known-answer vectors", "[vnc][des]")
{
    CheckDes("133457799BBCDFF1", "0123456789ABCDEF", "85E813540F0AB405");
    CheckDes("0101010101010101", "8000000000000000", "95F8A5E5DD31D900");
    CheckDes("0000000000000000", "0000000000000000", "8CA64DE9C1B123A7");
    CheckDes("FFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFF", "7359B2163E4EDC58");
}

TEST_CASE("DES ignores the key's parity bits", "[vnc][des]")
{
    // 133457799BBCDFF1 with every parity (low) bit flipped must encrypt
    // identically: FIPS 46-3 discards those bits in PC-1.
    CheckDes("123556789ABDDEF0", "0123456789ABCDEF", "85E813540F0AB405");
}

TEST_CASE("DES encrypts in place", "[vnc][des]")
{
    uint8_t k[8], b[8];
    Hex("133457799BBCDFF1", k, 8);
    Hex("0123456789ABCDEF", b, 8);
    DesEncryptBlock(k, b, b);
    CHECK(ToHex(b, 8) == "85E813540F0AB405");
}

TEST_CASE("VNC key reverses the bits of each password byte", "[vnc][des]")
{
    uint8_t key[8];
    VncAuthKey("A", key);   // 0100 0001 -> 1000 0010
    CHECK(key[0] == 0x82);
    for (int i = 1; i < 8; ++i)
        CHECK(key[i] == 0);

    VncAuthKey("password", key);
    const uint8_t expect[8] = { 0x0E, 0x86, 0xCE, 0xCE, 0xEE, 0xF6, 0x4E, 0x26 };
    CHECK(std::memcmp(key, expect, 8) == 0);
}

TEST_CASE("VNC key truncates to eight bytes and zero-pads shorter passwords", "[vnc][des]")
{
    uint8_t a[8], b[8];
    VncAuthKey("12345678", a);
    VncAuthKey("123456789 and more", b);
    CHECK(std::memcmp(a, b, 8) == 0);

    uint8_t c[8];
    VncAuthKey("", c);
    for (int i = 0; i < 8; ++i)
        CHECK(c[i] == 0);
}

TEST_CASE("VNC response is two DES blocks under the reversed key", "[vnc][des]")
{
    uint8_t challenge[16];
    for (int i = 0; i < 16; ++i)
        challenge[i] = static_cast<uint8_t>(0x10 * i + 3);
    uint8_t response[16];
    VncAuthResponse("secret", challenge, response);

    uint8_t key[8], expect[16];
    VncAuthKey("secret", key);
    DesEncryptBlock(key, challenge, expect);
    DesEncryptBlock(key, challenge + 8, expect + 8);
    CHECK(std::memcmp(response, expect, 16) == 0);

    // and it may be computed in place
    uint8_t inplace[16];
    std::memcpy(inplace, challenge, 16);
    VncAuthResponse("secret", inplace, inplace);
    CHECK(std::memcmp(inplace, expect, 16) == 0);
}

TEST_CASE("VNC response for an empty password is the zero-key known answer", "[vnc][des]")
{
    uint8_t challenge[16] = {};
    uint8_t response[16];
    VncAuthResponse("", challenge, response);
    CHECK(ToHex(response, 8) == "8CA64DE9C1B123A7");
    CHECK(ToHex(response + 8, 8) == "8CA64DE9C1B123A7");
}

TEST_CASE("VNC responses differ by password and by challenge", "[vnc][des]")
{
    uint8_t ch1[16], ch2[16];
    for (int i = 0; i < 16; ++i)
    {
        ch1[i] = static_cast<uint8_t>(i);
        ch2[i] = static_cast<uint8_t>(i);
    }
    ch2[15] ^= 1;
    uint8_t r1[16], r2[16], r3[16];
    VncAuthResponse("alpha", ch1, r1);
    VncAuthResponse("alphb", ch1, r2);
    VncAuthResponse("alpha", ch2, r3);
    CHECK(std::memcmp(r1, r2, 16) != 0);
    CHECK(std::memcmp(r1, r3, 16) != 0);
    // a one-byte challenge change touches only the second block
    CHECK(std::memcmp(r1, r3, 8) == 0);
}
