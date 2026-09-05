// RfbClient.cpp — the RFB client state machine. See the header for the rules
// it exists to get right.
#include "RfbClient.h"

#include <algorithm>
#include <cstring>

#include "RfbDes.h"
#include "RfbTls.h"

namespace amber::vnc
{
namespace
{

// A reason string from a server is displayed; it is not trusted. Printable
// ASCII only, bounded, so a hostile server cannot put control sequences or
// a novel into a status line.
std::string Sanitise(const uint8_t* p, size_t n)
{
    std::string s;
    const size_t cap = std::min<size_t>(n, 200);
    for (size_t i = 0; i < cap; ++i)
    {
        const uint8_t c = p[i];
        s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    }
    if (n > cap)
        s += "...";
    return s;
}

} // namespace

RfbClient::RfbClient(ClientOptions options) : m_opt(std::move(options)) {}

void RfbClient::Fail(FailureKind kind, std::string why)
{
    m_state = ClientState::Failed;
    m_failure = kind;
    m_error = std::move(why);
    m_in.clear();
    std::fill(m_opt.password.begin(), m_opt.password.end(), '\0');
    m_opt.password.clear();
}

bool RfbClient::Feed(const uint8_t* data, size_t n)
{
    if (m_state == ClientState::Failed)
        return false;
    m_in.insert(m_in.end(), data, data + n);
    size_t off = 0;
    for (;;)
    {
        Reader r(m_in.data() + off, m_in.size() - off);
        const ClientState before = m_state;
        Parse p;
        switch (m_state)
        {
        case ClientState::Version:          p = StepVersion(r); break;
        case ClientState::SecurityTypes:    p = StepSecurityTypes(r); break;
        case ClientState::SecurityReason:   p = StepSecurityReason(r); break;
        case ClientState::VncAuthChallenge: p = StepVncAuthChallenge(r); break;
        case ClientState::SecurityResult:   p = StepSecurityResult(r); break;
        case ClientState::VeNCryptVersion:  p = StepVeNCryptVersion(r); break;
        case ClientState::VeNCryptVersionAck: p = StepVeNCryptVersionAck(r); break;
        case ClientState::VeNCryptSubtypes: p = StepVeNCryptSubtypes(r); break;
        case ClientState::VeNCryptSubtypeAck: p = StepVeNCryptSubtypeAck(r); break;
        case ClientState::TlsHandshake:     p = Parse::NeedMore; break;   // parked: the caller runs TLS
        case ClientState::ServerInit:       p = StepServerInit(r); break;
        case ClientState::Ready:            p = StepReady(r); break;
        default:                            p = Parse::Bad; break;
        }
        off += r.Consumed();
        if (p == Parse::Bad)
        {
            if (m_state != ClientState::Failed)
                Fail(FailureKind::Protocol, "malformed data from the server");
            return false;
        }
        if (m_state == ClientState::Failed)
            return false;
        if (p == Parse::NeedMore)
            break;
        // Ok without progress would spin: a step must consume or advance
        if (r.Consumed() == 0 && m_state == before && !m_inUpdate)
            break;
    }
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

std::vector<uint8_t> RfbClient::TakeOutput()
{
    std::vector<uint8_t> out;
    out.swap(m_out);
    return out;
}

// ---- handshake -----------------------------------------------------------------
Parse RfbClient::StepVersion(Reader& r)
{
    const uint8_t* p = r.Peek(12);
    if (!p)
        return Parse::NeedMore;
    Version v;
    if (!ParseVersion(p, v))
    {
        Fail(FailureKind::Protocol, "not an RFB server");
        return Parse::Ok;
    }
    r.Skip(12);
    const int minor = ChooseMinor(v);
    if (minor < 0)
    {
        Fail(FailureKind::Protocol, "unsupported RFB version " + std::to_string(v.major) + "." +
                                        std::to_string(v.minor));
        return Parse::Ok;
    }
    m_minor = minor;
    const auto reply = VersionReply(minor);
    m_out.insert(m_out.end(), reply.begin(), reply.end());
    m_state = ClientState::SecurityTypes;
    return Parse::Ok;
}

Parse RfbClient::StepSecurityTypes(Reader& r)
{
    const size_t start = r.Consumed();
    if (m_minor == 3)
    {
        // 3.3: the server states one type in a u32 and takes no answer
        uint32_t type;
        if (!r.U32(type))
            return Parse::NeedMore;
        if (type == 0)
        {
            m_state = ClientState::SecurityReason;
            return Parse::Ok;
        }
        if (m_opt.tls)
        {
            // 3.3 has no security-type list, so it has no VeNCrypt either
            Fail(FailureKind::Authentication,
                 "this profile requires TLS and an RFB 3.3 server cannot offer it");
            return Parse::Ok;
        }
        if (type == SecNone && m_opt.allowNone)
        {
            AfterSecurity(SecNone);
            return Parse::Ok;
        }
        if (type == SecVncAuth && m_opt.allowVncAuth)
        {
            AfterSecurity(SecVncAuth);
            return Parse::Ok;
        }
        if (type == SecNone || type == SecVncAuth)
            Fail(FailureKind::Authentication, "the server's security type is not allowed by this profile");
        else
            Fail(FailureKind::Protocol, "unsupported security type " + std::to_string(type));
        return Parse::Ok;
    }

    uint8_t count;
    if (!r.U8(count))
        return Parse::NeedMore;
    if (count == 0)
    {
        m_state = ClientState::SecurityReason;
        return Parse::Ok;
    }
    const uint8_t* types = r.Peek(count);
    if (!types)
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    bool none = false, vnc = false, ven = false;
    for (uint8_t i = 0; i < count; ++i)
    {
        if (types[i] == SecNone) none = true;
        if (types[i] == SecVncAuth) vnc = true;
        if (types[i] == SecVeNCrypt) ven = true;
    }
    r.Skip(count);
    // TLS required: VeNCrypt or nothing. A plaintext type the server also
    // offers is not a fallback; it is what "never downgrade" means.
    if (m_opt.tls)
    {
        if (!ven)
        {
            Fail(FailureKind::Authentication,
                 "this profile requires TLS and the server does not offer VeNCrypt");
            return Parse::Ok;
        }
        m_out.push_back(SecVeNCrypt);
        m_security = SecVeNCrypt;
        m_state = ClientState::VeNCryptVersion;
        return Parse::Ok;
    }
    uint8_t choice = SecInvalid;
    // With a password in hand, VNC Authentication is what the user meant;
    // otherwise None is the only thing that can succeed.
    if (vnc && m_opt.allowVncAuth && !m_opt.password.empty())
        choice = SecVncAuth;
    else if (none && m_opt.allowNone)
        choice = SecNone;
    else if (vnc && m_opt.allowVncAuth)
        choice = SecVncAuth;
    if (choice == SecInvalid)
    {
        Fail(FailureKind::Authentication, "no security type in common with the server");
        return Parse::Ok;
    }
    m_out.push_back(choice);
    AfterSecurity(choice);
    return Parse::Ok;
}

void RfbClient::AfterSecurity(uint8_t type)
{
    m_security = type;
    if (type == SecVncAuth)
    {
        m_state = ClientState::VncAuthChallenge;
        return;
    }
    // None: a SecurityResult follows only from 3.8; before that the
    // initialisation phase starts at once and waiting for one deadlocks.
    if (m_minor >= 8)
        m_state = ClientState::SecurityResult;
    else
        SendClientInit();
}

Parse RfbClient::StepSecurityReason(Reader& r)
{
    const size_t start = r.Consumed();
    uint32_t len;
    if (!r.U32(len))
        return Parse::NeedMore;
    if (len > kMaxReason)
        return Parse::Bad;
    const uint8_t* p = r.Peek(len);
    if (!p)
    {
        r.Rewind(start);
        return Parse::NeedMore;
    }
    std::string why = Sanitise(p, len);
    r.Skip(len);
    Fail(FailureKind::Refused, why.empty() ? "the server refused the connection" : why);
    return Parse::Ok;
}

// ---- VeNCrypt (security type 19) --------------------------------------------------
// Version: the server states its highest (major, minor); the client answers
// with what it will speak. Anything below 0.2 is refused.
Parse RfbClient::StepVeNCryptVersion(Reader& r)
{
    const uint8_t* v = r.Peek(2);
    if (!v)
        return Parse::NeedMore;
    r.Skip(2);
    if (v[0] != 0 || v[1] < 2)
    {
        Fail(FailureKind::Protocol, "unsupported VeNCrypt version " + std::to_string(v[0]) + "." + std::to_string(v[1]));
        return Parse::Ok;
    }
    m_out.push_back(0);
    m_out.push_back(2);
    m_state = ClientState::VeNCryptVersionAck;
    return Parse::Ok;
}

Parse RfbClient::StepVeNCryptVersionAck(Reader& r)
{
    uint8_t ack;
    if (!r.U8(ack))
        return Parse::NeedMore;
    if (ack != 0)
    {
        Fail(FailureKind::Protocol, "the server refused VeNCrypt 0.2");
        return Parse::Ok;
    }
    m_state = ClientState::VeNCryptSubtypes;
    return Parse::Ok;
}

Parse RfbClient::StepVeNCryptSubtypes(Reader& r)
{
    const size_t start = r.Consumed();
    uint8_t n;
    if (!r.U8(n))
        return Parse::NeedMore;
    std::vector<uint32_t> offered;
    for (uint8_t i = 0; i < n; ++i)
    {
        uint32_t s;
        if (!r.U32(s))
        {
            r.Rewind(start);
            return Parse::NeedMore;
        }
        offered.push_back(s);
    }
    const uint32_t choice = VeNCryptChoose(offered, !m_opt.password.empty(), !m_opt.username.empty(),
                                           m_opt.allowPlainOverTls);
    if (choice == 0)
    {
        std::string names;
        for (uint32_t s : offered)
            names += std::string(names.empty() ? "" : ", ") + VeNCryptSubtypeName(s);
        Fail(FailureKind::Authentication,
             "no acceptable VeNCrypt subtype: the server offers " + (names.empty() ? std::string("none") : names) +
                 "; an X.509 certificate is required, anonymous TLS and plain passwords are refused");
        return Parse::Ok;
    }
    m_venSubtype = choice;
    Send({ static_cast<uint8_t>(choice >> 24), static_cast<uint8_t>(choice >> 16), static_cast<uint8_t>(choice >> 8),
           static_cast<uint8_t>(choice) });
    m_state = ClientState::VeNCryptSubtypeAck;
    return Parse::Ok;
}

Parse RfbClient::StepVeNCryptSubtypeAck(Reader& r)
{
    uint8_t ack;
    if (!r.U8(ack))
        return Parse::NeedMore;
    if (ack != 1)
    {
        Fail(FailureKind::Authentication, "the server refused the VeNCrypt subtype");
        return Parse::Ok;
    }
    // parked: the caller runs the TLS handshake on the socket now
    m_state = ClientState::TlsHandshake;
    return Parse::Ok;
}

void RfbClient::TlsEstablished()
{
    if (m_state != ClientState::TlsHandshake)
        return;
    m_encrypted = true;
    switch (m_venSubtype)
    {
    case VenX509Vnc:
        m_state = ClientState::VncAuthChallenge;
        break;
    case VenX509Plain:
    {
        // u32 username length, u32 password length, then both — inside TLS
        const std::string& u = m_opt.username;
        std::vector<uint8_t> m;
        auto put32 = [&](uint32_t x) {
            m.push_back(static_cast<uint8_t>(x >> 24)); m.push_back(static_cast<uint8_t>(x >> 16));
            m.push_back(static_cast<uint8_t>(x >> 8));  m.push_back(static_cast<uint8_t>(x));
        };
        put32(static_cast<uint32_t>(u.size()));
        put32(static_cast<uint32_t>(m_opt.password.size()));
        m.insert(m.end(), u.begin(), u.end());
        m.insert(m.end(), m_opt.password.begin(), m_opt.password.end());
        Send(m);
        std::fill(m.begin(), m.end(), uint8_t{ 0 });
        std::fill(m_opt.password.begin(), m_opt.password.end(), '\0');
        m_opt.password.clear();
        m_state = ClientState::SecurityResult;
        break;
    }
    case VenX509None:
    default:
        // VeNCrypt is a 3.7+ extension and every server that speaks it sends
        // a SecurityResult after the subtype's (empty) authentication
        m_state = ClientState::SecurityResult;
        break;
    }
}

Parse RfbClient::StepVncAuthChallenge(Reader& r)
{
    const uint8_t* ch = r.Peek(16);
    if (!ch)
        return Parse::NeedMore;
    uint8_t resp[16];
    VncAuthResponse(m_opt.password, ch, resp);
    std::fill(m_opt.password.begin(), m_opt.password.end(), '\0');
    m_opt.password.clear();
    m_out.insert(m_out.end(), resp, resp + 16);
    std::memset(resp, 0, sizeof resp);
    r.Skip(16);
    m_state = ClientState::SecurityResult;
    return Parse::Ok;
}

Parse RfbClient::StepSecurityResult(Reader& r)
{
    const size_t start = r.Consumed();
    uint32_t result;
    if (!r.U32(result))
        return Parse::NeedMore;
    if (result == 0)
    {
        SendClientInit();
        return Parse::Ok;
    }
    if (m_minor >= 8)
    {
        uint32_t len;
        if (!r.U32(len))
        {
            r.Rewind(start);
            return Parse::NeedMore;
        }
        if (len > kMaxReason)
            return Parse::Bad;
        const uint8_t* p = r.Peek(len);
        if (!p)
        {
            r.Rewind(start);
            return Parse::NeedMore;
        }
        std::string why = Sanitise(p, len);
        r.Skip(len);
        Fail(FailureKind::Authentication, why.empty() ? "authentication failed" : why);
        return Parse::Ok;
    }
    // before 3.8 there is no reason; older servers simply close after this
    Fail(FailureKind::Authentication, "authentication failed");
    return Parse::Ok;
}

void RfbClient::SendClientInit()
{
    Send(MsgClientInit(m_opt.shared));
    m_state = ClientState::ServerInit;
}

Parse RfbClient::StepServerInit(Reader& r)
{
    ServerInit init;
    const Parse p = ParseServerInit(r, init);
    if (p != Parse::Ok)
        return p;
    if (!m_fb.Resize(init.width, init.height))
        return Parse::Bad;
    m_init = std::move(init);
    m_dirty.Clear();
    m_dirty.Add({ 0, 0, m_init.width, m_init.height });
    m_zrle.Reset();
    Send(MsgSetPixelFormat(kRequestedFormat));
    OfferEncodings();
    m_state = ClientState::Ready;
    RequestUpdate(false, true);
    return Parse::Ok;
}

void RfbClient::OfferEncodings()
{
    std::vector<int32_t> enc = m_opt.encodings;
    if (m_opt.wantCursor)
        enc.push_back(EncPseudoCursor);
    if (m_opt.wantDesktopSize)
        enc.push_back(EncPseudoDesktopSize);
    if (m_opt.wantContinuousUpdates)
        enc.push_back(EncPseudoContinuousUpdates);
    Send(MsgSetEncodings(enc));
}

// ---- steady state -------------------------------------------------------------
void RfbClient::RequestUpdate(bool incremental, bool force)
{
    if (m_state != ClientState::Ready)
        return;
    if (!force && (m_outstanding >= 1 || (m_continuous && incremental)))
        return;
    Send(MsgFramebufferUpdateRequest(incremental, 0, 0, m_init.width, m_init.height));
    if (m_outstanding < 2)
        ++m_outstanding;
}

void RfbClient::SendKey(bool down, uint32_t keysym)
{
    if (m_state == ClientState::Ready)
        Send(MsgKeyEvent(down, keysym));
}

void RfbClient::SendPointer(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (m_state == ClientState::Ready)
        Send(MsgPointerEvent(buttons, x, y));
}

void RfbClient::SendCutText(std::string_view utf8)
{
    if (m_state == ClientState::Ready)
        Send(MsgClientCutText(Latin1FromUtf8(utf8)));
}

bool RfbClient::TakeResized()
{
    const bool r = m_resized;
    m_resized = false;
    return r;
}

bool RfbClient::TakeCursorChanged()
{
    const bool c = m_cursorChanged;
    m_cursorChanged = false;
    return c;
}

std::vector<std::string> RfbClient::TakeCutTexts()
{
    std::vector<std::string> out;
    out.swap(m_cutTexts);
    return out;
}

int RfbClient::TakeBells()
{
    const int b = m_bells;
    m_bells = 0;
    return b;
}

uint32_t RfbClient::TakeUpdatesCompleted()
{
    const uint32_t n = m_updatesCompleted;
    m_updatesCompleted = 0;
    return n;
}

Parse RfbClient::StepReady(Reader& r)
{
    if (m_inUpdate)
        return StepRectangle(r);
    const uint8_t* t = r.Peek(1);
    if (!t)
        return Parse::NeedMore;
    switch (*t)
    {
    case SFramebufferUpdate:
    {
        uint16_t n;
        const Parse p = ParseFramebufferUpdateHeader(r, n);
        if (p != Parse::Ok)
            return p;
        m_inUpdate = true;
        m_rectsLeft = n;
        m_haveRect = false;
        // This update answers the outstanding request. Ask for the next one
        // now, before decoding this one, so the server is always working on
        // the frame after the one on the wire (vncfree's latency note).
        if (!m_continuous)
        {
            m_outstanding = 0;
            RequestUpdate(true);
        }
        return StepRectangle(r);
    }
    case SSetColourMapEntries:
    {
        // not applicable to a true-colour client, but it must be consumed
        const uint8_t* h = r.Peek(6);
        if (!h)
            return Parse::NeedMore;
        const uint32_t n = (static_cast<uint32_t>(h[4]) << 8) | h[5];
        if (!r.Peek(6 + static_cast<size_t>(n) * 6))
            return Parse::NeedMore;
        r.Skip(6 + static_cast<size_t>(n) * 6);
        return Parse::Ok;
    }
    case SBell:
        r.Skip(1);
        ++m_bells;
        return Parse::Ok;
    case SServerCutText:
    {
        std::string latin1;
        const Parse p = ParseServerCutText(r, latin1);
        if (p != Parse::Ok)
            return p;
        m_cutTexts.push_back(Utf8FromLatin1(latin1));
        return Parse::Ok;
    }
    case SEndOfContinuousUpdates:
        r.Skip(1);
        if (!m_continuousAsked && m_opt.wantContinuousUpdates)
        {
            // Unprompted, this is the server saying it can: enable it. From
            // here the server sends changes as they happen and the client
            // stops asking.
            Send(MsgEnableContinuousUpdates(true, 0, 0, m_init.width, m_init.height));
            m_continuousAsked = true;
            m_continuous = true;
            m_outstanding = 0;
        }
        return Parse::Ok;
    default:
        return Parse::Bad;
    }
}

Parse RfbClient::StepRectangle(Reader& r)
{
    while (m_rectsLeft > 0)
    {
        if (!m_haveRect)
        {
            const Parse p = ParseRectHeader(r, m_rect);
            if (p != Parse::Ok)
                return p;
            m_haveRect = true;
            if (m_rect.encoding == EncHextile)
                m_hextile.BeginRect();
        }
        Decode d = Decode::Ok;
        const Rect rc{ m_rect.x, m_rect.y, m_rect.w, m_rect.h };
        switch (m_rect.encoding)
        {
        case EncPseudoDesktopSize:
            // Before the bounds check: the new size is outside the old buffer
            if (!m_fb.Resize(m_rect.w, m_rect.h))
                return Parse::Bad;
            m_init.width = m_rect.w;
            m_init.height = m_rect.h;
            m_resized = true;
            m_dirty.Clear();
            m_dirty.Add({ 0, 0, m_rect.w, m_rect.h });
            // the contents are black until a full picture arrives; ask for one
            RequestUpdate(false, true);
            if (m_continuous)
                Send(MsgEnableContinuousUpdates(true, 0, 0, m_rect.w, m_rect.h));
            break;
        case EncPseudoCursor:
            d = DecodeCursor(r, m_rect, m_cursor);
            if (d == Decode::Ok)
                m_cursorChanged = true;
            break;
        case EncRaw:
            d = DecodeRaw(r, m_fb, rc);
            break;
        case EncCopyRect:
            d = DecodeCopyRect(r, m_fb, rc);
            break;
        case EncZRLE:
            d = m_zrle.Decode(r, m_fb, rc);
            break;
        case EncHextile:
            d = m_hextile.Decode(r, m_fb, rc);
            break;
        default:
            return Parse::Bad;   // an encoding that was never offered
        }
        if (d == Decode::NeedMore)
            return Parse::NeedMore;
        if (d == Decode::Bad)
            return Parse::Bad;
        if (m_rect.encoding >= 0)
            m_dirty.Add(rc);
        m_haveRect = false;
        --m_rectsLeft;
    }
    m_inUpdate = false;
    ++m_updatesCompleted;
    return Parse::Ok;
}

} // namespace amber::vnc
