// RfbTls.h — VeNCrypt (RFB security type 19) and the TLS it runs on.
//
// VeNCrypt wraps the rest of the RFB handshake in TLS: after the client
// picks type 19 the two sides agree a VeNCrypt version and a subtype, TLS
// is negotiated on the same socket, and the subtype's own authentication
// (none, the VNC challenge, or a username and password) runs inside it.
//
// Which subtypes this client will take is a policy, and it is written here
// rather than left to the server's list:
//
//   * X509None, X509Vnc, X509Plain (260-262): the server presents an X.509
//     certificate. That is the only way TLS authenticates the far side, so
//     these are the only ones accepted.
//   * TLSNone, TLSVnc, TLSPlain (257-259): anonymous Diffie-Hellman. The
//     traffic is encrypted and there is no way whatsoever to tell whether
//     it is encrypted to the server or to someone relaying for it. Refused.
//   * Plain (256): a username and password in the clear. Refused.
//
// A profile that asks for TLS never falls back to an unencrypted type, and
// a server that offers no acceptable subtype is refused with a message
// that names the reason, not retried.
//
// Certificates are verified against the Windows root store, with the host
// name checked. When the chain does not verify — a self-signed server,
// which is most VNC servers — the leaf's SHA-256 fingerprint is shown to
// the user the way an SSH host key is, and on acceptance pinned; a later
// connection showing a different certificate is the changed-key alarm,
// not a warning that can be clicked past. The pins live beside the SSH
// known hosts and hold fingerprints only.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace amber::vnc
{

enum VeNCryptSubtype : uint32_t
{
    VenPlain = 256,
    VenTlsNone = 257,
    VenTlsVnc = 258,
    VenTlsPlain = 259,
    VenX509None = 260,
    VenX509Vnc = 261,
    VenX509Plain = 262,
};
const char* VeNCryptSubtypeName(uint32_t subtype);

// The policy above, as a predicate.
bool VeNCryptAcceptable(uint32_t subtype, bool allowPlainOverTls);
// The choice among what the server offered: X509Vnc when a password is in
// hand, X509None otherwise, X509Plain only when allowed and a username is
// set. 0 when nothing acceptable was offered.
uint32_t VeNCryptChoose(const std::vector<uint32_t>& offered, bool havePassword, bool haveUsername,
                        bool allowPlainOverTls);

// A TLS client over an already-connected, non-blocking socket.
class TlsClient
{
public:
    TlsClient();
    ~TlsClient();
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    enum class Verify
    {
        NotStarted,
        Trusted,    // the chain verifies against the root store and the name matches
        Untrusted,  // handshake done, chain or name did not verify — ask, and pin
    };

    // Runs the handshake. Blocks up to timeoutMs, polling `stop`. The
    // server name goes out as SNI (unless it is an address) and is what
    // the certificate is checked against.
    bool Connect(uintptr_t sock, const std::string& serverName, int timeoutMs,
                 const std::atomic<bool>& stop, std::string& err);
    Verify VerifyResult() const { return m_verify; }
    const std::string& VerifyError() const { return m_verifyError; }
    // "SHA256:<hex>" of the leaf certificate's DER, or "" before Connect.
    const std::string& Fingerprint() const { return m_fingerprint; }
    const std::string& Subject() const { return m_subject; }
    const std::string& Protocol() const { return m_protocol; }   // e.g. "TLSv1.3"

    // Non-blocking, on the socket the handshake used: > 0 bytes moved,
    // 0 = would block (wait on the socket), -1 = closed or failed.
    int Read(uint8_t* buf, size_t n);
    int Write(const uint8_t* buf, size_t n);
    bool Active() const { return m_ssl != nullptr; }
    void Close();

private:
    void* m_ctx = nullptr;   // SSL_CTX*
    void* m_ssl = nullptr;   // SSL*
    Verify m_verify = Verify::NotStarted;
    std::string m_verifyError, m_fingerprint, m_subject, m_protocol;
};

// The pin store: %LOCALAPPDATA%\AmberSSH\vnc_known_hosts, one "host:port
// SHA256:hex" per line. Fingerprints only; never a certificate.
std::string LoadCertificatePin(const std::string& hostPort);
bool SaveCertificatePin(const std::string& hostPort, const std::string& fingerprint);
// Tests point the store at a scratch file so they never touch the user's;
// empty restores the default.
void SetCertificatePinPathForTests(const std::wstring& path);

} // namespace amber::vnc
