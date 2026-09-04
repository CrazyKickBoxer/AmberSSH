#include "Handshake.h"

#include "../../utility/Hash.h"

namespace amber::amberx
{

namespace
{

// The MAC input is label | nonceA | nonceB. The label is what makes the two
// proofs different values over the same nonces, so a proof captured in one
// direction is useless in the other.
std::vector<uint8_t> Transcript(const char* label,
                                const std::vector<uint8_t>& nonceA,
                                const std::vector<uint8_t>& nonceB)
{
    std::vector<uint8_t> t(label, label + std::char_traits<char>::length(label));
    t.insert(t.end(), nonceA.begin(), nonceA.end());
    t.insert(t.end(), nonceB.begin(), nonceB.end());
    return t;
}

bool SizesOk(const std::vector<uint8_t>& secret, const std::vector<uint8_t>& a,
             const std::vector<uint8_t>& b)
{
    return secret.size() == kSecretBytes && a.size() == kNonceBytes &&
           b.size() == kNonceBytes;
}

Frame Control(MsgType t, std::vector<uint8_t> payload)
{
    Frame f;
    f.type = t;
    f.channel = kControlChannel;
    f.payload = std::move(payload);
    return f;
}

} // namespace

std::vector<uint8_t> HostProof(const std::vector<uint8_t>& secret,
                               const std::vector<uint8_t>& nonceA,
                               const std::vector<uint8_t>& nonceB)
{
    if (!SizesOk(secret, nonceA, nonceB))
        return {};
    return HmacSha256(secret, Transcript("amberx-host-v1", nonceA, nonceB));
}

std::vector<uint8_t> ControllerProof(const std::vector<uint8_t>& secret,
                                     const std::vector<uint8_t>& nonceA,
                                     const std::vector<uint8_t>& nonceB)
{
    if (!SizesOk(secret, nonceA, nonceB))
        return {};
    return HmacSha256(secret, Transcript("amberx-ctl-v1", nonceA, nonceB));
}

// ------------------------------------------------------------ controller
Step ControllerHandshake::Fail()
{
    m_state = S::Failed;
    // Forget the secret on failure: a state machine that is finished with a
    // key should not keep it around to be read later.
    m_secret.assign(m_secret.size(), 0);
    m_secret.clear();
    Step s;
    s.state = Step::State::Failed;
    return s;
}

Step ControllerHandshake::Begin(const std::vector<uint8_t>& secret,
                                const std::vector<uint8_t>& nonceA)
{
    if (m_state != S::Init || secret.size() != kSecretBytes ||
        nonceA.size() != kNonceBytes)
        return Fail();
    m_secret = secret;
    m_nonceA = nonceA;
    m_state = S::AwaitAck;
    Step s;
    s.send = Control(MsgType::Hello, MakeHello(nonceA));
    s.hasSend = true;
    return s;
}

Step ControllerHandshake::OnFrame(const Frame& f)
{
    // Anything after Done or Failed is a protocol error. A handshake that
    // accepted a second HelloAck after completing would let a peer rewrite
    // the session's nonces.
    if (m_state != S::AwaitAck)
        return Fail();
    if (f.type != MsgType::HelloAck || f.channel != kControlChannel)
        return Fail();

    std::vector<uint8_t> echo, nonceB, proof;
    if (!ParseHelloAck(f.payload, echo, nonceB, proof))
        return Fail();
    // The echoed nonce binds this reply to THIS Hello. A replay from another
    // session carries a different nonceA and fails here.
    if (!ConstantTimeEqual(echo, m_nonceA))
        return Fail();
    const std::vector<uint8_t> expect = HostProof(m_secret, m_nonceA, nonceB);
    if (!ConstantTimeEqual(proof, expect))
        return Fail();

    // The host has proved it holds the secret. Prove it back.
    Step s;
    s.send = Control(MsgType::AuthProof,
                     MakeAuthProof(ControllerProof(m_secret, m_nonceA, nonceB)));
    s.hasSend = true;
    s.state = Step::State::Done;
    m_state = S::Done;
    m_secret.assign(m_secret.size(), 0);
    m_secret.clear();
    return s;
}

// ------------------------------------------------------------------ host
Step HostHandshake::Fail()
{
    m_state = S::Failed;
    m_secret.assign(m_secret.size(), 0);
    m_secret.clear();
    Step s;
    s.state = Step::State::Failed;
    return s;
}

Step HostHandshake::Begin(const std::vector<uint8_t>& secret,
                          const std::vector<uint8_t>& nonceB)
{
    if (m_state != S::Init || secret.size() != kSecretBytes ||
        nonceB.size() != kNonceBytes)
        return Fail();
    m_secret = secret;
    m_nonceB = nonceB;
    m_state = S::AwaitHello;
    Step s;          // nothing to send: the controller speaks first
    return s;
}

Step HostHandshake::OnFrame(const Frame& f)
{
    if (f.channel != kControlChannel)
        return Fail();

    if (m_state == S::AwaitHello)
    {
        if (f.type != MsgType::Hello)
            return Fail();
        if (!ParseHello(f.payload, m_nonceA))
            return Fail();
        Step s;
        s.send = Control(MsgType::HelloAck,
                         MakeHelloAck(m_nonceA, m_nonceB,
                                      HostProof(m_secret, m_nonceA, m_nonceB)));
        s.hasSend = true;
        m_state = S::AwaitProof;
        return s;
    }

    if (m_state == S::AwaitProof)
    {
        if (f.type != MsgType::AuthProof)
            return Fail();
        std::vector<uint8_t> proof;
        if (!ParseAuthProof(f.payload, proof))
            return Fail();
        const std::vector<uint8_t> expect =
            ControllerProof(m_secret, m_nonceA, m_nonceB);
        if (!ConstantTimeEqual(proof, expect))
            return Fail();
        Step s;
        s.state = Step::State::Done;
        m_state = S::Done;
        m_secret.assign(m_secret.size(), 0);
        m_secret.clear();
        return s;
    }

    return Fail();      // Init, Done or Failed: nothing is acceptable
}

} // namespace amber::amberx
