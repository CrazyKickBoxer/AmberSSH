// RfbTls.cpp — VeNCrypt's subtype policy and the OpenSSL client. See the
// header for the policy; this file is the plumbing.
#include "RfbTls.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

// wincrypt.h defines these as macros that collide with OpenSSL's names
#undef X509_NAME
#undef X509_CERT_PAIR
#undef X509_EXTENSIONS
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace amber::vnc
{

const char* VeNCryptSubtypeName(uint32_t s)
{
    switch (s)
    {
    case VenPlain:     return "Plain";
    case VenTlsNone:   return "TLSNone";
    case VenTlsVnc:    return "TLSVnc";
    case VenTlsPlain:  return "TLSPlain";
    case VenX509None:  return "X509None";
    case VenX509Vnc:   return "X509Vnc";
    case VenX509Plain: return "X509Plain";
    }
    return "unknown";
}

bool VeNCryptAcceptable(uint32_t subtype, bool allowPlainOverTls)
{
    return subtype == VenX509None || subtype == VenX509Vnc || (subtype == VenX509Plain && allowPlainOverTls);
}

uint32_t VeNCryptChoose(const std::vector<uint32_t>& offered, bool havePassword, bool haveUsername,
                        bool allowPlainOverTls)
{
    auto has = [&](uint32_t s) {
        for (uint32_t o : offered)
            if (o == s)
                return true;
        return false;
    };
    if (havePassword && has(VenX509Vnc))
        return VenX509Vnc;
    if (allowPlainOverTls && haveUsername && havePassword && has(VenX509Plain))
        return VenX509Plain;
    if (has(VenX509None))
        return VenX509None;
    if (has(VenX509Vnc))
        return VenX509Vnc;   // an empty password may still be what the server wants
    return 0;
}

// ---- the root store ------------------------------------------------------------
namespace
{

void AddWindowsStore(X509_STORE* store, const wchar_t* name)
{
    HCERTSTORE h = CertOpenSystemStoreW(0, name);
    if (!h)
        return;
    PCCERT_CONTEXT c = nullptr;
    while ((c = CertEnumCertificatesInStore(h, c)) != nullptr)
    {
        const unsigned char* p = c->pbCertEncoded;
        X509* x = d2i_X509(nullptr, &p, static_cast<long>(c->cbCertEncoded));
        if (x)
        {
            X509_STORE_add_cert(store, x);   // duplicates are reported, not fatal
            X509_free(x);
        }
    }
    CertCloseStore(h, 0);
}

bool LooksLikeAddress(const std::string& s)
{
    in_addr a4;
    in6_addr a6;
    return inet_pton(AF_INET, s.c_str(), &a4) == 1 || inet_pton(AF_INET6, s.c_str(), &a6) == 1;
}

std::string OpenSslError()
{
    char buf[256] = {};
    const unsigned long e = ERR_get_error();
    if (e == 0)
        return "TLS failed";
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

std::wstring g_pinPathOverride;

std::wstring PinPath()
{
    if (!g_pinPathOverride.empty())
        return g_pinPathOverride;
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
    {
        dir = std::wstring(base) + L"\\AmberSSH";
        CoTaskMemFree(base);
    }
    if (dir.empty())
        return {};
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\vnc_known_hosts";
}

} // namespace

// ---- TlsClient --------------------------------------------------------------------
TlsClient::TlsClient() = default;

TlsClient::~TlsClient()
{
    Close();
}

void TlsClient::Close()
{
    if (m_ssl)
    {
        SSL_free(static_cast<SSL*>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_ctx)
    {
        SSL_CTX_free(static_cast<SSL_CTX*>(m_ctx));
        m_ctx = nullptr;
    }
}

bool TlsClient::Connect(uintptr_t sock, const std::string& serverName, int timeoutMs,
                        const std::atomic<bool>& stop, std::string& err)
{
    Close();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
    {
        err = OpenSslError();
        return false;
    }
    m_ctx = ctx;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    // Verification is performed and recorded, not enforced by the handshake:
    // an untrusted certificate still completes so that its fingerprint can
    // be shown and, if the user says so, pinned.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    AddWindowsStore(store, L"ROOT");
    AddWindowsStore(store, L"CA");

    SSL* ssl = SSL_new(ctx);
    if (!ssl)
    {
        err = OpenSslError();
        return false;
    }
    m_ssl = ssl;
    if (!serverName.empty())
    {
        if (!LooksLikeAddress(serverName))
            SSL_set_tlsext_host_name(ssl, serverName.c_str());
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        SSL_set1_host(ssl, serverName.c_str());
    }
    SSL_set_fd(ssl, static_cast<int>(sock));

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;)
    {
        if (stop.load())
        {
            err = "cancelled";
            return false;
        }
        const int rc = SSL_connect(ssl);
        if (rc == 1)
            break;
        const int e = SSL_get_error(ssl, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
        {
            err = "TLS handshake failed: " + OpenSslError();
            return false;
        }
        if (GetTickCount64() >= deadline)
        {
            err = "TLS handshake timed out";
            return false;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(static_cast<SOCKET>(sock), &fds);
        timeval tv{ 0, 100000 };
        if (e == SSL_ERROR_WANT_READ)
            select(0, &fds, nullptr, nullptr, &tv);
        else
            select(0, nullptr, &fds, nullptr, &tv);
    }

    m_protocol = SSL_get_version(ssl);
    X509* peer = SSL_get1_peer_certificate(ssl);
    if (!peer)
    {
        err = "the server presented no certificate";
        return false;
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (X509_digest(peer, EVP_sha256(), md, &len) == 1)
    {
        static const char* hex = "0123456789ABCDEF";
        m_fingerprint = "SHA256:";
        for (unsigned int i = 0; i < len; ++i)
        {
            m_fingerprint.push_back(hex[md[i] >> 4]);
            m_fingerprint.push_back(hex[md[i] & 15]);
            if (i + 1 < len)
                m_fingerprint.push_back(':');
        }
    }
    char subj[256] = {};
    X509_NAME_oneline(X509_get_subject_name(peer), subj, sizeof subj);
    m_subject = subj;
    X509_free(peer);

    const long v = SSL_get_verify_result(ssl);
    if (v == X509_V_OK)
        m_verify = Verify::Trusted;
    else
    {
        m_verify = Verify::Untrusted;
        m_verifyError = X509_verify_cert_error_string(v);
    }
    return true;
}

int TlsClient::Read(uint8_t* buf, size_t n)
{
    if (!m_ssl)
        return -1;
    SSL* ssl = static_cast<SSL*>(m_ssl);
    const int r = SSL_read(ssl, buf, static_cast<int>(n));
    if (r > 0)
        return r;
    const int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        return 0;
    ERR_clear_error();
    return -1;
}

int TlsClient::Write(const uint8_t* buf, size_t n)
{
    if (!m_ssl)
        return -1;
    SSL* ssl = static_cast<SSL*>(m_ssl);
    const int r = SSL_write(ssl, buf, static_cast<int>(n));
    if (r > 0)
        return r;
    const int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
        return 0;
    ERR_clear_error();
    return -1;
}

// ---- pins -------------------------------------------------------------------------------
std::string LoadCertificatePin(const std::string& hostPort)
{
    const std::wstring path = PinPath();
    if (path.empty())
        return {};
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        if (line.substr(0, sp) == hostPort)
            return line.substr(sp + 1);
    }
    return {};
}

bool SaveCertificatePin(const std::string& hostPort, const std::string& fingerprint)
{
    const std::wstring path = PinPath();
    if (path.empty())
        return false;
    // rewrite without any old line for this host, then append the new one
    std::vector<std::string> keep;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t sp = line.find(' ');
            if (sp != std::string::npos && line.substr(0, sp) == hostPort)
                continue;
            if (!line.empty())
                keep.push_back(line);
        }
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    for (const std::string& l : keep)
        out << l << "\n";
    out << hostPort << " " << fingerprint << "\n";
    return true;
}

} // namespace amber::vnc

namespace amber::vnc
{
void SetCertificatePinPathForTests(const std::wstring& path)
{
    g_pinPathOverride = path;
}
} // namespace amber::vnc
