// RfbDes.h — DES for VNC Authentication (RFC 6143 §7.2.2), and nothing else.
//
// VNC Authentication encrypts a 16-byte server challenge with DES in ECB
// mode, two blocks, keyed by the password. The key is not the password's
// bytes: every VNC implementation since the original AT&T one reverses the
// bit order within each byte first ('A' = 0100 0001 becomes 1000 0010),
// pads with zeros to eight bytes and ignores anything past the eighth. RFC
// 6143 does not say so; interoperating servers all require it, and vncfree's
// notes call it out. This is that variant, written here in C++ so the
// program does not depend on OpenSSL's legacy DES provider for a cipher it
// uses for sixteen bytes per connection.
//
// DES is FIPS 46-3. It is used here only because the protocol demands it;
// nothing in AmberSSH treats it as protection of anything.
#pragma once

#include <cstdint>
#include <string_view>

namespace amber::vnc
{

// One DES block, ECB, FIPS 46-3. Parity bits of the key are ignored, as the
// standard specifies. `in` and `out` may alias.
void DesEncryptBlock(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

// The VNC Authentication key for a password: each byte's bits reversed,
// truncated to eight bytes, zero-padded when shorter.
void VncAuthKey(std::string_view password, uint8_t key[8]);

// The response to a VNC Authentication challenge: the 16 challenge bytes as
// two DES blocks under VncAuthKey(password). `response` may alias `challenge`.
void VncAuthResponse(std::string_view password, const uint8_t challenge[16],
                     uint8_t response[16]);

} // namespace amber::vnc
