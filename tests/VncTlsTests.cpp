// VncTlsTests.cpp — VeNCrypt and the TLS under it.
//
// Three layers, each verified on its own terms:
//
//   * the subtype policy (RfbTls.h): what this client will and will not
//     take from a server's list, as a pure function;
//   * the RFB client's VeNCrypt steps, byte for byte through the fake
//     server, parked at the TLS handshake and resumed after it, for every
//     accepted subtype and every refusal;
//   * the OpenSSL client against an in-process OpenSSL server on loopback,
//     with a certificate generated here: the fingerprint it reports, the
//     verdict it reaches, the bytes it moves — and then the whole worker
//     (VncSession) through it: the certificate question, the pin, the
//     changed-certificate alarm, and no retry after a refusal.
//
// The pin store is pointed at a scratch file for the duration; the user's
// vnc_known_hosts is never read or written by a test.
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#undef X509_NAME
#undef X509_CERT_PAIR
#undef X509_EXTENSIONS
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "FakeRfbServer.h"
#include "vnc/RfbClient.h"
#include "vnc/RfbDes.h"
#include "vnc/RfbTls.h"
#include "vnc/VncSession.h"

using namespace amber::vnc;
using fakerfb::Bytes;
using fakerfb::Server;

namespace
{

Bytes Take(RfbClient& c) { return c.TakeOutput(); }
bool Feed(RfbClient& c, const Bytes& b) { return c.Feed(b.data(), b.size()); }
Bytes U32(uint32_t x)
{
    Bytes v;
    fakerfb::U32(v, x);
    return v;
}

// Brings a TLS-required client through the plaintext part of VeNCrypt to
// the parked state, offered `subtypes`. Returns the subtype it chose.
uint32_t ToTlsHandshake(RfbClient& c, Server& s, const std::vector<uint32_t>& subtypes)
{
    REQUIRE(Feed(c, s.Greeting()));
    Take(c);
    REQUIRE(Feed(c, s.SecurityOffer({ SecNone, SecVncAuth, SecVeNCrypt })));
    REQUIRE(Take(c) == Bytes{ SecVeNCrypt });   // never a plaintext type
    REQUIRE(c.State() == ClientState::VeNCryptVersion);
    REQUIRE(Feed(c, { 0, 2 }));
    REQUIRE(Take(c) == Bytes{ 0, 2 });
    REQUIRE(c.State() == ClientState::VeNCryptVersionAck);
    REQUIRE(Feed(c, { 0 }));
    REQUIRE(c.State() == ClientState::VeNCryptSubtypes);
    Bytes offer = { static_cast<uint8_t>(subtypes.size()) };
    for (uint32_t st : subtypes)
        fakerfb::U32(offer, st);
    const bool ok = Feed(c, offer);   // false on the feed that fails the client
    if (c.State() == ClientState::Failed)
    {
        REQUIRE_FALSE(ok);
        return 0;
    }
    REQUIRE(ok);
    const Bytes choice = Take(c);
    REQUIRE(choice.size() == 4);
    REQUIRE(c.State() == ClientState::VeNCryptSubtypeAck);
    REQUIRE(Feed(c, { 1 }));
    REQUIRE(c.NeedsTls());
    REQUIRE(Take(c).empty());
    return fakerfb::R32(choice.data());
}

// ---- loopback plumbing ------------------------------------------------------------
struct Wsa
{
    Wsa() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
    ~Wsa() { WSACleanup(); }
};

bool ReadExact(SOCKET s, uint8_t* out, size_t n, int timeoutMs = 5000)
{
    size_t got = 0;
    while (got < n)
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(0, &r, nullptr, nullptr, &tv) <= 0)
            return false;
        const int k = recv(s, reinterpret_cast<char*>(out + got), static_cast<int>(n - got), 0);
        if (k <= 0)
            return false;
        got += static_cast<size_t>(k);
    }
    return true;
}

void WriteAll(SOCKET s, const Bytes& b)
{
    size_t off = 0;
    while (off < b.size())
    {
        const int k = send(s, reinterpret_cast<const char*>(b.data() + off), static_cast<int>(b.size() - off), 0);
        if (k <= 0)
            return;
        off += static_cast<size_t>(k);
    }
}

struct Listener
{
    SOCKET lst = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::atomic<int> accepted{ 0 };
    std::atomic<bool> stop{ false };

    void Start(std::function<void(SOCKET, int)> handler)
    {
        lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(lst, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        REQUIRE(listen(lst, 4) == 0);
        int len = sizeof a;
        getsockname(lst, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this, handler] {
            for (;;)
            {
                fd_set r;
                FD_ZERO(&r);
                FD_SET(lst, &r);
                timeval tv{ 0, 100000 };
                if (select(0, &r, nullptr, nullptr, &tv) <= 0)
                {
                    if (stop.load())
                        return;
                    continue;
                }
                const SOCKET c = accept(lst, nullptr, nullptr);
                if (c == INVALID_SOCKET)
                    return;
                const int idx = accepted.fetch_add(1);
                handler(c, idx);
                closesocket(c);
            }
        });
    }
    void Stop()
    {
        stop.store(true);
        if (th.joinable())
            th.join();
        if (lst != INVALID_SOCKET)
            closesocket(lst);
    }
    ~Listener() { Stop(); }
};

SOCKET ConnectLoopback(int port)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

// ---- a certificate made here, and the server side of TLS -------------------------
struct SelfSigned
{
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;

    explicit SelfSigned(const char* cn, long serial)
    {
        key = EVP_EC_gen("P-256");
        REQUIRE(key != nullptr);
        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* n = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
        X509_set_issuer_name(cert, n);
        REQUIRE(X509_sign(cert, key, EVP_sha256()) > 0);
    }
    ~SelfSigned()
    {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    std::string Fingerprint() const
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        REQUIRE(X509_digest(cert, EVP_sha256(), md, &len) == 1);
        static const char* hex = "0123456789ABCDEF";
        std::string fp = "SHA256:";
        for (unsigned int i = 0; i < len; ++i)
        {
            fp.push_back(hex[md[i] >> 4]);
            fp.push_back(hex[md[i] & 15]);
            if (i + 1 < len)
                fp.push_back(':');
        }
        return fp;
    }
};

// The server's TLS over an accepted (blocking) socket.
struct TlsServerSide
{
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;

    bool Accept(SOCKET s, const SelfSigned& id)
    {
        ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate(ctx, id.cert) != 1 || SSL_CTX_use_PrivateKey(ctx, id.key) != 1)
            return false;
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, static_cast<int>(s));
        return SSL_accept(ssl) == 1;
    }
    bool ReadExact(uint8_t* out, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            const int k = SSL_read(ssl, out + got, static_cast<int>(n - got));
            if (k <= 0)
                return false;
            got += static_cast<size_t>(k);
        }
        return true;
    }
    Bytes Read(size_t n)
    {
        Bytes b(n);
        if (!ReadExact(b.data(), n))
            b.clear();
        return b;
    }
    void Write(const Bytes& b)
    {
        size_t off = 0;
        while (off < b.size())
        {
            const int k = SSL_write(ssl, b.data() + off, static_cast<int>(b.size() - off));
            if (k <= 0)
                return;
            off += static_cast<size_t>(k);
        }
    }
    // One steady-state client message, type byte first; empty on error.
    Bytes ReadClientMessage()
    {
        uint8_t t;
        if (!ReadExact(&t, 1))
            return {};
        Bytes m = { t };
        auto more = [&](size_t n) {
            Bytes b(n);
            if (!ReadExact(b.data(), n))
                return false;
            m.insert(m.end(), b.begin(), b.end());
            return true;
        };
        switch (t)
        {
        case CSetPixelFormat: if (!more(19)) return {}; break;
        case CSetEncodings:
        {
            if (!more(3)) return {};
            const uint16_t n = fakerfb::R16(&m[2]);
            if (!more(static_cast<size_t>(n) * 4)) return {};
            break;
        }
        case CFramebufferUpdateRequest: if (!more(9)) return {}; break;
        case CKeyEvent: if (!more(7)) return {}; break;
        case CPointerEvent: if (!more(5)) return {}; break;
        case CClientCutText:
        {
            if (!more(7)) return {};
            const uint32_t n = fakerfb::R32(&m[4]);
            if (!more(n)) return {};
            break;
        }
        case CEnableContinuousUpdates: if (!more(9)) return {}; break;
        default: return {};
        }
        return m;
    }
    ~TlsServerSide()
    {
        if (ssl)
        {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx)
            SSL_CTX_free(ctx);
    }
};

// The pin store on a scratch file for one test.
struct ScratchPins
{
    std::wstring path;
    ScratchPins()
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        path = std::wstring(tmp) + L"amber-vnc-pins-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()) + L".txt";
        SetCertificatePinPathForTests(path);
    }
    ~ScratchPins()
    {
        SetCertificatePinPathForTests({});
        DeleteFileW(path.c_str());
    }
    std::string Contents() const
    {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};

bool WaitFor(std::function<bool()> pred, int timeoutMs = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

} // namespace

// ==== the policy ======================================================================
TEST_CASE("VeNCrypt: only the X.509 subtypes are acceptable", "[vnc][tls]")
{
    REQUIRE(VeNCryptAcceptable(VenX509None, false));
    REQUIRE(VeNCryptAcceptable(VenX509Vnc, false));
    REQUIRE_FALSE(VeNCryptAcceptable(VenX509Plain, false));
    REQUIRE(VeNCryptAcceptable(VenX509Plain, true));
    // anonymous TLS: encrypted to whoever answered
    REQUIRE_FALSE(VeNCryptAcceptable(VenTlsNone, true));
    REQUIRE_FALSE(VeNCryptAcceptable(VenTlsVnc, true));
    REQUIRE_FALSE(VeNCryptAcceptable(VenTlsPlain, true));
    // a password in the clear
    REQUIRE_FALSE(VeNCryptAcceptable(VenPlain, true));
    REQUIRE_FALSE(VeNCryptAcceptable(0, true));
    REQUIRE_FALSE(VeNCryptAcceptable(999, true));
}

TEST_CASE("VeNCrypt: the choice prefers the password challenge, then none, then plain if allowed", "[vnc][tls]")
{
    const std::vector<uint32_t> all = { VenPlain, VenTlsNone, VenTlsVnc, VenTlsPlain, VenX509None, VenX509Vnc,
                                        VenX509Plain };
    REQUIRE(VeNCryptChoose(all, true, true, true) == VenX509Vnc);
    REQUIRE(VeNCryptChoose(all, false, true, true) == VenX509None);
    REQUIRE(VeNCryptChoose({ VenX509Plain, VenTlsVnc }, true, true, true) == VenX509Plain);
    REQUIRE(VeNCryptChoose({ VenX509Plain }, true, true, false) == 0);   // plain not allowed
    REQUIRE(VeNCryptChoose({ VenX509Plain }, true, false, true) == 0);   // no username to send
    REQUIRE(VeNCryptChoose({ VenX509Vnc }, false, false, false) == VenX509Vnc);   // an empty password is still an answer
    REQUIRE(VeNCryptChoose({ VenTlsNone, VenTlsVnc, VenTlsPlain, VenPlain }, true, true, true) == 0);
    REQUIRE(VeNCryptChoose({}, true, true, true) == 0);
    REQUIRE(std::string(VeNCryptSubtypeName(VenX509Vnc)) == "X509Vnc");
    REQUIRE(std::string(VeNCryptSubtypeName(VenTlsPlain)) == "TLSPlain");
}

// ==== the client's steps ==============================================================
TEST_CASE("VeNCrypt X509Vnc: the handshake parks at TLS and resumes with the challenge inside it", "[vnc][tls][client]")
{
    Server s;
    ClientOptions o;
    o.tls = true;
    o.password = "secret";
    RfbClient c(o);
    REQUIRE(ToTlsHandshake(c, s, { VenTlsVnc, VenX509None, VenX509Vnc }) == VenX509Vnc);
    REQUIRE(c.VeNCryptSubtype() == VenX509Vnc);
    REQUIRE_FALSE(c.Encrypted());

    // nothing is parsed while parked: bytes fed now are the server's TLS
    // records, which the caller reads off the socket itself
    REQUIRE(c.NeedsTls());

    c.TlsEstablished();
    REQUIRE(c.Encrypted());
    REQUIRE(c.State() == ClientState::VncAuthChallenge);
    REQUIRE(Feed(c, s.Challenge()));
    uint8_t want[16];
    VncAuthResponse("secret", s.challenge, want);
    REQUIRE(Take(c) == Bytes(want, want + 16));
    REQUIRE(c.State() == ClientState::SecurityResult);
    REQUIRE(Feed(c, s.SecurityResult(true)));
    REQUIRE(Take(c) == Bytes{ 1 });   // ClientInit, shared
    REQUIRE(Feed(c, s.ServerInit()));
    REQUIRE(c.State() == ClientState::Ready);
    REQUIRE(c.NegotiatedMinor() == 8);
}

TEST_CASE("VeNCrypt X509None: no authentication inside TLS, straight to SecurityResult", "[vnc][tls][client]")
{
    Server s;
    ClientOptions o;
    o.tls = true;   // no password
    RfbClient c(o);
    REQUIRE(ToTlsHandshake(c, s, { VenX509Vnc, VenX509None }) == VenX509None);
    c.TlsEstablished();
    REQUIRE(c.State() == ClientState::SecurityResult);
    REQUIRE(Take(c).empty());
    REQUIRE(Feed(c, s.SecurityResult(true)));
    REQUIRE(Take(c) == Bytes{ 1 });
}

TEST_CASE("VeNCrypt X509Plain: username and password go out inside TLS, only when allowed", "[vnc][tls][client]")
{
    Server s;
    SECTION("allowed and named")
    {
        ClientOptions o;
        o.tls = true;
        o.password = "pw";
        o.username = "user";
        o.allowPlainOverTls = true;
        RfbClient c(o);
        REQUIRE(ToTlsHandshake(c, s, { VenX509Plain }) == VenX509Plain);
        c.TlsEstablished();
        Bytes want = U32(4);
        Bytes pw = U32(2);
        want.insert(want.end(), pw.begin(), pw.end());
        want.insert(want.end(), { 'u', 's', 'e', 'r', 'p', 'w' });
        REQUIRE(Take(c) == want);
        REQUIRE(c.State() == ClientState::SecurityResult);
        REQUIRE_FALSE(Feed(c, s.SecurityResult(false, "no such user")));
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(c.Failure() == FailureKind::Authentication);
        REQUIRE(c.Error() == "no such user");
    }
    SECTION("not allowed: refused, nothing chosen")
    {
        ClientOptions o;
        o.tls = true;
        o.password = "pw";
        o.username = "user";
        RfbClient c(o);
        REQUIRE(ToTlsHandshake(c, s, { VenX509Plain }) == 0);
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(c.Failure() == FailureKind::Authentication);
        REQUIRE(Take(c).empty());
        REQUIRE(c.Error().find("X509Plain") != std::string::npos);
    }
}

TEST_CASE("TLS required: a server without VeNCrypt, or a 3.3 server, is refused — never plaintext", "[vnc][tls][client]")
{
    SECTION("3.8, None and VNC Authentication only")
    {
        Server s;
        ClientOptions o;
        o.tls = true;
        o.password = "secret";
        RfbClient c(o);
        REQUIRE(Feed(c, s.Greeting()));
        Take(c);
        REQUIRE_FALSE(Feed(c, s.SecurityOffer({ SecNone, SecVncAuth })));   // false: it failed on this feed
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(c.Failure() == FailureKind::Authentication);
        REQUIRE(Take(c).empty());   // no choice went out
    }
    SECTION("3.3 has no list to find VeNCrypt in")
    {
        Server s;
        s.minor = 3;
        ClientOptions o;
        o.tls = true;
        RfbClient c(o);
        REQUIRE(Feed(c, s.Greeting()));
        Take(c);
        REQUIRE_FALSE(Feed(c, s.SecurityOffer({ SecNone })));
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(Take(c).empty());
    }
    SECTION("only anonymous subtypes")
    {
        Server s;
        ClientOptions o;
        o.tls = true;
        o.password = "secret";
        RfbClient c(o);
        REQUIRE(ToTlsHandshake(c, s, { VenTlsNone, VenTlsVnc, VenTlsPlain, VenPlain }) == 0);
        REQUIRE(c.Failure() == FailureKind::Authentication);
        REQUIRE(Take(c).empty());
        REQUIRE(c.Error().find("TLSVnc") != std::string::npos);
    }
}

TEST_CASE("VeNCrypt refusals: an old version, a refused version, a refused subtype", "[vnc][tls][client]")
{
    Server s;
    auto start = [&](RfbClient& c) {
        REQUIRE(Feed(c, s.Greeting()));
        Take(c);
        REQUIRE(Feed(c, s.SecurityOffer({ SecVeNCrypt })));
        Take(c);
    };
    SECTION("version 0.1")
    {
        RfbClient c(ClientOptions{ .password = "x", .tls = true });
        start(c);
        REQUIRE_FALSE(Feed(c, { 0, 1 }));   // false: it failed on this feed
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(c.Failure() == FailureKind::Protocol);
    }
    SECTION("version refused")
    {
        RfbClient c(ClientOptions{ .password = "x", .tls = true });
        start(c);
        REQUIRE(Feed(c, { 0, 2 }));
        Take(c);
        REQUIRE_FALSE(Feed(c, { 255 }));
        REQUIRE(c.State() == ClientState::Failed);
    }
    SECTION("subtype refused")
    {
        RfbClient c(ClientOptions{ .password = "x", .tls = true });
        start(c);
        REQUIRE(Feed(c, { 0, 2 }));
        Take(c);
        REQUIRE(Feed(c, { 0 }));
        Bytes offer = { 1 };
        fakerfb::U32(offer, VenX509Vnc);
        REQUIRE(Feed(c, offer));
        REQUIRE(Take(c) == U32(VenX509Vnc));
        REQUIRE_FALSE(Feed(c, { 0 }));
        REQUIRE(c.State() == ClientState::Failed);
        REQUIRE(c.Failure() == FailureKind::Authentication);
    }
}

TEST_CASE("VeNCrypt: fed one byte at a time", "[vnc][tls][client]")
{
    Server s;
    ClientOptions o;
    o.tls = true;
    o.password = "secret";
    RfbClient c(o);
    Bytes stream = s.Greeting();
    Bytes offer = s.SecurityOffer({ SecVeNCrypt });
    stream.insert(stream.end(), offer.begin(), offer.end());
    stream.insert(stream.end(), { 0, 2, 0, 2 });
    fakerfb::U32(stream, VenX509None);
    fakerfb::U32(stream, VenX509Vnc);
    stream.push_back(1);
    Bytes sent;
    for (uint8_t b : stream)
    {
        REQUIRE(c.Feed(&b, 1));
        const Bytes out = Take(c);
        sent.insert(sent.end(), out.begin(), out.end());
    }
    REQUIRE(c.NeedsTls());
    Bytes want(s.Greeting());
    want.push_back(SecVeNCrypt);
    want.insert(want.end(), { 0, 2 });
    Bytes choice = U32(VenX509Vnc);
    want.insert(want.end(), choice.begin(), choice.end());
    REQUIRE(sent == want);
}

TEST_CASE("without TLS required, a server offering VeNCrypt beside None still connects in plaintext", "[vnc][tls][client]")
{
    // the profile did not ask for encryption: the policy is the profile's,
    // and the overlay says "plaintext"
    Server s;
    RfbClient c(ClientOptions{});
    REQUIRE(Feed(c, s.Greeting()));
    Take(c);
    REQUIRE(Feed(c, s.SecurityOffer({ SecVeNCrypt, SecNone })));
    REQUIRE(Take(c) == Bytes{ SecNone });
    REQUIRE_FALSE(c.Encrypted());
}

// ==== OpenSSL on loopback =============================================================
TEST_CASE("TlsClient: handshake with a self-signed server, fingerprint, verdict, bytes both ways", "[vnc][tls][net]")
{
    Wsa wsa;
    SelfSigned id("vnc.test", 1);
    Listener lst;
    std::string serverGot;
    lst.Start([&](SOCKET s, int) {
        TlsServerSide t;
        if (!t.Accept(s, id))
            return;
        const Bytes in = t.Read(4);
        serverGot.assign(in.begin(), in.end());
        t.Write({ 'p', 'o', 'n', 'g' });
    });

    const SOCKET cs = ConnectLoopback(lst.port);
    std::atomic<bool> stop{ false };
    TlsClient tls;
    std::string err;
    REQUIRE(tls.Connect(static_cast<uintptr_t>(cs), "127.0.0.1", 5000, stop, err));
    INFO(err);
    REQUIRE(tls.Active());
    REQUIRE(tls.VerifyResult() == TlsClient::Verify::Untrusted);   // nobody signed it
    REQUIRE_FALSE(tls.VerifyError().empty());
    REQUIRE(tls.Fingerprint() == id.Fingerprint());
    REQUIRE(tls.Subject().find("CN=vnc.test") != std::string::npos);
    REQUIRE(tls.Protocol().rfind("TLSv1.", 0) == 0);

    const uint8_t ping[4] = { 'p', 'i', 'n', 'g' };
    size_t off = 0;
    while (off < 4)
    {
        const int n = tls.Write(ping + off, 4 - off);
        REQUIRE(n >= 0);
        off += static_cast<size_t>(n);
    }
    uint8_t buf[4];
    size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got < 4 && std::chrono::steady_clock::now() < deadline)
    {
        const int n = tls.Read(buf + got, 4 - got);
        REQUIRE(n >= 0);
        if (n == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        got += static_cast<size_t>(n);
    }
    REQUIRE(got == 4);
    REQUIRE(std::string(buf, buf + 4) == "pong");
    REQUIRE(WaitFor([&] { return serverGot == "ping"; }));
    tls.Close();
    REQUIRE_FALSE(tls.Active());
    closesocket(cs);
}

TEST_CASE("TlsClient: a plaintext server is a handshake failure, not a hang", "[vnc][tls][net]")
{
    Wsa wsa;
    Listener lst;
    lst.Start([&](SOCKET s, int) {
        WriteAll(s, { 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '8', '\n' });
        uint8_t sink[64];
        ReadExact(s, sink, 1, 2000);
    });
    const SOCKET cs = ConnectLoopback(lst.port);
    std::atomic<bool> stop{ false };
    TlsClient tls;
    std::string err;
    REQUIRE_FALSE(tls.Connect(static_cast<uintptr_t>(cs), "127.0.0.1", 3000, stop, err));
    REQUIRE(err.find("TLS") != std::string::npos);
    closesocket(cs);
}

// ==== the worker through VeNCrypt =====================================================
// A VeNCrypt X509Vnc server: plaintext to the subtype ack, TLS with `id`,
// then the challenge, the result, ServerInit and one Raw full update
// inside it. Records the client's key events until the connection ends.
static void ServeVeNCrypt(SOCKET s, const SelfSigned& id, const std::string& password, uint16_t w, uint16_t h,
                          uint32_t colour, std::vector<uint32_t>* keysyms, std::atomic<bool>* authOk)
{
    Server srv;
    srv.width = w;
    srv.height = h;
    WriteAll(s, srv.Greeting());
    uint8_t ver[12];
    if (!ReadExact(s, ver, 12))
        return;
    WriteAll(s, srv.SecurityOffer({ SecNone, SecVeNCrypt }));
    uint8_t choice;
    if (!ReadExact(s, &choice, 1) || choice != SecVeNCrypt)
        return;
    WriteAll(s, { 0, 2 });
    uint8_t cver[2];
    if (!ReadExact(s, cver, 2) || cver[0] != 0 || cver[1] != 2)
        return;
    Bytes offer = { 0, 2 };   // ack, count
    fakerfb::U32(offer, VenX509None);
    fakerfb::U32(offer, VenX509Vnc);
    WriteAll(s, offer);
    uint8_t sub[4];
    if (!ReadExact(s, sub, 4) || fakerfb::R32(sub) != VenX509Vnc)
        return;
    WriteAll(s, { 1 });

    TlsServerSide t;
    if (!t.Accept(s, id))
        return;
    t.Write(srv.Challenge());
    const Bytes resp = t.Read(16);
    uint8_t want[16];
    VncAuthResponse(password, srv.challenge, want);
    if (resp != Bytes(want, want + 16))
    {
        t.Write(srv.SecurityResult(false, "wrong password"));
        return;
    }
    if (authOk)
        authOk->store(true);
    t.Write(srv.SecurityResult(true));
    if (t.Read(1).empty())
        return;
    t.Write(srv.ServerInit());
    bool sentPicture = false;
    for (;;)
    {
        const Bytes m = t.ReadClientMessage();
        if (m.empty())
            return;
        if (m[0] == CFramebufferUpdateRequest && !sentPicture)
        {
            sentPicture = true;
            Bytes upd = Server::UpdateHeader(1);
            Bytes rect = Server::RectHeader(0, 0, w, h, EncRaw);
            upd.insert(upd.end(), rect.begin(), rect.end());
            for (uint32_t i = 0; i < static_cast<uint32_t>(w) * h; ++i)
            {
                upd.push_back(static_cast<uint8_t>(colour));         // B
                upd.push_back(static_cast<uint8_t>(colour >> 8));    // G
                upd.push_back(static_cast<uint8_t>(colour >> 16));   // R
                upd.push_back(0);
            }
            t.Write(upd);
        }
        if (m[0] == CKeyEvent && keysyms)
            keysyms->push_back(fakerfb::R32(&m[4]));
    }
}

TEST_CASE("VncSession over VeNCrypt: the certificate question, the pin, the changed-certificate alarm", "[vnc][tls][net][session]")
{
    Wsa wsa;
    ScratchPins pins;
    SelfSigned certA("desk-a", 10);
    SelfSigned certB("desk-b", 11);
    std::atomic<bool> authOk{ false };
    std::vector<uint32_t> keysyms;
    Listener lst;
    lst.Start([&](SOCKET s, int idx) {
        // the third connection presents a different certificate
        ServeVeNCrypt(s, idx < 2 ? certA : certB, "secret", 8, 6, 0x00FF8040u, &keysyms, &authOk);
    });

    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = lst.port;
    cfg.password = "secret";
    cfg.tls = true;
    cfg.maxReconnects = 1;
    const std::string hostPort = "127.0.0.1:" + std::to_string(lst.port);

    SECTION("first sight: asked, accepted, pinned; the picture arrives inside TLS")
    {
        VncSession v;
        REQUIRE(v.Start(cfg));
        VncEvent ev;
        bool asked = false;
        REQUIRE(WaitFor([&] {
            while (v.PollEvent(ev))
            {
                if (ev.type == VncEvent::Type::CertPrompt)
                {
                    asked = true;
                    REQUIRE_FALSE(ev.flag);   // nothing pinned yet: a question, not an alarm
                    REQUIRE(ev.text.find(certA.Fingerprint()) != std::string::npos);
                    REQUIRE(ev.text.find("CN=desk-a") != std::string::npos);
                    REQUIRE(ev.text.find("Not verified") != std::string::npos);
                    v.AnswerCertificate(true);
                }
                if (ev.type == VncEvent::Type::Connected)
                    return true;
                if (ev.type == VncEvent::Type::Error || ev.type == VncEvent::Type::AuthFailed)
                    FAIL(ev.text);
            }
            return false;
        }));
        REQUIRE(asked);
        REQUIRE(authOk.load());
        REQUIRE(v.State() == VncState::Connected);
        const VncStats st = v.GetStats();
        REQUIRE(st.encrypted);
        REQUIRE(st.venSubtype == VenX509Vnc);
        REQUIRE(st.tlsProtocol.rfind("TLSv1.", 0) == 0);

        Damage d;
        REQUIRE(WaitFor([&] { return v.TakeDamage(d); }));
        REQUIRE(d.width == 8);
        REQUIRE(d.height == 6);
        REQUIRE(d.full);
        REQUIRE(d.pixels.size() == 48);
        REQUIRE(d.pixels[0] == 0xFFFF8040u);
        REQUIRE(d.pixels[47] == 0xFFFF8040u);

        // input goes out inside TLS too
        v.SendKey(true, 0x61);
        v.SendKey(false, 0x61);
        REQUIRE(WaitFor([&] { return keysyms.size() >= 2; }));
        REQUIRE(keysyms[0] == 0x61);

        v.Disconnect();
        const std::string file = pins.Contents();
        REQUIRE(file.find(hostPort + " " + certA.Fingerprint()) != std::string::npos);

        // second sight: pinned, so no question
        VncSession v2;
        REQUIRE(v2.Start(cfg));
        bool askedAgain = false;
        REQUIRE(WaitFor([&] {
            while (v2.PollEvent(ev))
            {
                if (ev.type == VncEvent::Type::CertPrompt)
                    askedAgain = true;
                if (ev.type == VncEvent::Type::Connected)
                    return true;
            }
            return false;
        }));
        REQUIRE_FALSE(askedAgain);
        v2.Disconnect();

        // third: a different certificate is the alarm; refused, and not retried
        VncSession v3;
        REQUIRE(v3.Start(cfg));
        bool alarm = false;
        bool errored = false;
        REQUIRE(WaitFor([&] {
            while (v3.PollEvent(ev))
            {
                if (ev.type == VncEvent::Type::CertPrompt)
                {
                    alarm = ev.flag;
                    REQUIRE(ev.text.find(certB.Fingerprint()) != std::string::npos);
                    REQUIRE(ev.text.find("CHANGED") != std::string::npos);
                    v3.AnswerCertificate(false);
                }
                if (ev.type == VncEvent::Type::Error)
                {
                    errored = true;
                    REQUIRE(ev.text.find("not accepted") != std::string::npos);
                    return true;
                }
                if (ev.type == VncEvent::Type::Connected)
                    FAIL("connected through a certificate the user refused");
            }
            return false;
        }));
        REQUIRE(alarm);
        REQUIRE(errored);
        REQUIRE(WaitFor([&] { return !v3.Running(); }));
        REQUIRE(v3.State() == VncState::Error);
        const int seen = lst.accepted.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // longer than the first backoff
        REQUIRE(lst.accepted.load() == seen);   // no retry after a refusal
        REQUIRE(pins.Contents().find(certB.Fingerprint()) == std::string::npos);   // and no pin for it
        v3.Disconnect();
    }
    lst.Stop();
}

TEST_CASE("VncSession with TLS required refuses a plaintext-only server and does not retry", "[vnc][tls][net][session]")
{
    Wsa wsa;
    Listener lst;
    lst.Start([&](SOCKET s, int) {
        Server srv;
        WriteAll(s, srv.Greeting());
        uint8_t ver[12];
        if (!ReadExact(s, ver, 12))
            return;
        WriteAll(s, srv.SecurityOffer({ SecNone, SecVncAuth }));
        uint8_t sink;
        ReadExact(s, &sink, 1, 1000);   // nothing should arrive
    });
    VncConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = lst.port;
    cfg.password = "secret";
    cfg.tls = true;
    cfg.maxReconnects = 3;
    VncSession v;
    REQUIRE(v.Start(cfg));
    VncEvent ev;
    bool refused = false;
    REQUIRE(WaitFor([&] {
        while (v.PollEvent(ev))
        {
            if (ev.type == VncEvent::Type::AuthFailed || ev.type == VncEvent::Type::Error)
            {
                refused = true;
                REQUIRE(ev.text.find("VeNCrypt") != std::string::npos);
                return true;
            }
            if (ev.type == VncEvent::Type::Connected)
                FAIL("connected in plaintext with TLS required");
        }
        return false;
    }));
    REQUIRE(refused);
    REQUIRE(WaitFor([&] { return !v.Running(); }));
    REQUIRE(lst.accepted.load() == 1);
    v.Disconnect();
    lst.Stop();
}
