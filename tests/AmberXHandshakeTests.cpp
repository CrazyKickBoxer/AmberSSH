// AmberXHandshakeTests — the mutual startup handshake.
//
// The happy path is one test. Everything else is a way the handshake must
// FAIL, because a handshake is only as good as the things it refuses.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "amberx/control/Handshake.h"

using namespace amber::amberx;

namespace
{

std::vector<uint8_t> Fill(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }
std::vector<uint8_t> Secret(uint8_t v = 0x5E) { return Fill(kSecretBytes, v); }
std::vector<uint8_t> NA() { return Fill(kNonceBytes, 0xA1); }
std::vector<uint8_t> NB() { return Fill(kNonceBytes, 0xB2); }

// Runs the whole exchange with the given secrets on each side and returns
// whether BOTH sides reached Done. Frames are passed by value, exactly as
// they would arrive after a decode.
struct Run
{
    bool ctlDone = false, hostDone = false;
    bool ctlFailed = false, hostFailed = false;
};

Run Exchange(const std::vector<uint8_t>& ctlSecret,
             const std::vector<uint8_t>& hostSecret)
{
    ControllerHandshake c;
    HostHandshake h;
    Run r;
    Step s1 = c.Begin(ctlSecret, NA());
    h.Begin(hostSecret, NB());
    if (s1.state == Step::State::Failed || !s1.hasSend) { r.ctlFailed = true; return r; }
    Step s2 = h.OnFrame(s1.send);
    if (s2.state == Step::State::Failed || !s2.hasSend) { r.hostFailed = true; return r; }
    Step s3 = c.OnFrame(s2.send);
    if (s3.state == Step::State::Failed) { r.ctlFailed = true; return r; }
    Step s4 = h.OnFrame(s3.send);
    r.hostFailed = s4.state == Step::State::Failed;
    r.ctlDone = c.Done();
    r.hostDone = h.Done();
    return r;
}

} // namespace

TEST_CASE("The happy path completes on both sides", "[amberx][handshake]")
{
    const Run r = Exchange(Secret(), Secret());
    CHECK(r.ctlDone);
    CHECK(r.hostDone);
    CHECK_FALSE(r.ctlFailed);
    CHECK_FALSE(r.hostFailed);
}

TEST_CASE("The proofs are real MACs, and differ by direction", "[amberx][handshake]")
{
    const std::vector<uint8_t> hp = HostProof(Secret(), NA(), NB());
    const std::vector<uint8_t> cp = ControllerProof(Secret(), NA(), NB());
    REQUIRE(hp.size() == kProofBytes);
    REQUIRE(cp.size() == kProofBytes);
    // Same secret, same nonces, different label: a proof captured in one
    // direction must be useless in the other.
    CHECK(hp != cp);
    // Deterministic for the same inputs, different for a different secret or
    // a different nonce.
    CHECK(HostProof(Secret(), NA(), NB()) == hp);
    CHECK(HostProof(Secret(0x11), NA(), NB()) != hp);
    CHECK(HostProof(Secret(), Fill(kNonceBytes, 0xA2), NB()) != hp);
    CHECK(HostProof(Secret(), NA(), Fill(kNonceBytes, 0xB3)) != hp);
    // Wrong sizes produce nothing rather than a MAC over garbage.
    CHECK(HostProof(Fill(16, 1), NA(), NB()).empty());
    CHECK(HostProof(Secret(), Fill(8, 1), NB()).empty());
    CHECK(ControllerProof({}, NA(), NB()).empty());
}

TEST_CASE("A host with the wrong secret is refused by the controller", "[amberx][handshake]")
{
    // The squatter case: something answered on the pipe but never received
    // the per-launch secret. Its HelloAck proof cannot verify.
    const Run r = Exchange(Secret(0x5E), Secret(0x5F));
    CHECK(r.ctlFailed);
    CHECK_FALSE(r.ctlDone);
    CHECK_FALSE(r.hostDone);
}

TEST_CASE("A controller with the wrong secret is refused by the host", "[amberx][handshake]")
{
    // Drive it by hand so the host's own proof passes and the controller's
    // does not: the host must refuse at the AuthProof step specifically.
    ControllerHandshake c;
    HostHandshake h;
    Step s1 = c.Begin(Secret(0x01), NA());
    h.Begin(Secret(0x02), NB());
    Step s2 = h.OnFrame(s1.send);
    REQUIRE(s2.hasSend);
    // The controller will reject the host's proof (different secret) — so
    // fabricate what a controller WITH the host's secret would have sent,
    // except computed under the wrong one, to isolate the host's check.
    std::vector<uint8_t> echo, nb, hp;
    REQUIRE(ParseHelloAck(s2.send.payload, echo, nb, hp));
    Frame bad;
    bad.type = MsgType::AuthProof;
    bad.channel = kControlChannel;
    bad.payload = MakeAuthProof(ControllerProof(Secret(0x01), NA(), nb));
    Step s4 = h.OnFrame(bad);
    CHECK(s4.state == Step::State::Failed);
    CHECK(h.Failed());
}

TEST_CASE("A reflected proof is refused", "[amberx][handshake]")
{
    // Send the host's own proof back to it as if it were the controller's.
    ControllerHandshake c;
    HostHandshake h;
    Step s1 = c.Begin(Secret(), NA());
    h.Begin(Secret(), NB());
    Step s2 = h.OnFrame(s1.send);
    std::vector<uint8_t> echo, nb, hp;
    REQUIRE(ParseHelloAck(s2.send.payload, echo, nb, hp));
    Frame reflect;
    reflect.type = MsgType::AuthProof;
    reflect.channel = kControlChannel;
    reflect.payload = MakeAuthProof(hp);
    CHECK(h.OnFrame(reflect).state == Step::State::Failed);
}

TEST_CASE("A HelloAck echoing the wrong nonce is refused", "[amberx][handshake]")
{
    // A replay from an earlier session: valid-looking, correct secret, but
    // bound to a Hello this controller never sent.
    ControllerHandshake c;
    c.Begin(Secret(), NA());
    const std::vector<uint8_t> staleA = Fill(kNonceBytes, 0x77);
    Frame ack;
    ack.type = MsgType::HelloAck;
    ack.channel = kControlChannel;
    ack.payload = MakeHelloAck(staleA, NB(), HostProof(Secret(), staleA, NB()));
    CHECK(c.OnFrame(ack).state == Step::State::Failed);
    CHECK(c.Failed());
}

TEST_CASE("The wrong message type at any state fails", "[amberx][handshake]")
{
    ControllerHandshake c;
    c.Begin(Secret(), NA());
    Frame wrong;
    wrong.type = MsgType::SetCookie;       // legal type, wrong moment
    wrong.channel = kControlChannel;
    wrong.payload = MakeSetCookie(Fill(16, 1), 0);
    CHECK(c.OnFrame(wrong).state == Step::State::Failed);

    HostHandshake h;
    h.Begin(Secret(), NB());
    Frame proofFirst;                       // AuthProof before Hello
    proofFirst.type = MsgType::AuthProof;
    proofFirst.channel = kControlChannel;
    proofFirst.payload = Fill(kProofBytes, 0);
    CHECK(h.OnFrame(proofFirst).state == Step::State::Failed);
}

TEST_CASE("A handshake frame on a data channel fails", "[amberx][handshake]")
{
    // Handshake messages are control-channel only. One arriving on a data
    // channel means either a confused peer or an attempt to smuggle it past
    // the control path.
    HostHandshake h;
    h.Begin(Secret(), NB());
    Frame hello;
    hello.type = MsgType::Hello;
    hello.channel = 7;
    hello.payload = MakeHello(NA());
    CHECK(h.OnFrame(hello).state == Step::State::Failed);
}

TEST_CASE("Failed is terminal", "[amberx][handshake]")
{
    ControllerHandshake c;
    c.Begin(Secret(), NA());
    Frame junk;
    junk.type = MsgType::Shutdown;
    junk.channel = kControlChannel;
    REQUIRE(c.OnFrame(junk).state == Step::State::Failed);
    // A correct HelloAck arriving AFTER a failure must not resurrect it.
    Frame good;
    good.type = MsgType::HelloAck;
    good.channel = kControlChannel;
    good.payload = MakeHelloAck(NA(), NB(), HostProof(Secret(), NA(), NB()));
    CHECK(c.OnFrame(good).state == Step::State::Failed);
    CHECK(c.Failed());
    CHECK_FALSE(c.Done());
}

TEST_CASE("Done is terminal too", "[amberx][handshake]")
{
    // After completion the handshake object must refuse everything: a second
    // HelloAck accepted after Done would let a peer rewrite the nonces.
    ControllerHandshake c;
    HostHandshake h;
    Step s1 = c.Begin(Secret(), NA());
    h.Begin(Secret(), NB());
    Step s2 = h.OnFrame(s1.send);
    Step s3 = c.OnFrame(s2.send);
    REQUIRE(c.Done());
    CHECK(c.OnFrame(s2.send).state == Step::State::Failed);
    Step s4 = h.OnFrame(s3.send);
    REQUIRE(h.Done());
    CHECK(h.OnFrame(s3.send).state == Step::State::Failed);
}

TEST_CASE("Begin refuses bad sizes and refuses to run twice", "[amberx][handshake]")
{
    ControllerHandshake c;
    CHECK(c.Begin(Fill(16, 1), NA()).state == Step::State::Failed);
    ControllerHandshake c2;
    CHECK(c2.Begin(Secret(), Fill(8, 1)).state == Step::State::Failed);
    ControllerHandshake c3;
    REQUIRE(c3.Begin(Secret(), NA()).state == Step::State::Continue);
    CHECK(c3.Begin(Secret(), NA()).state == Step::State::Failed);

    HostHandshake h;
    CHECK(h.Begin({}, NB()).state == Step::State::Failed);
}

TEST_CASE("Frames the handshake emits are encodable", "[amberx][handshake]")
{
    // The state machine builds Frames; those must pass Encode's own checks,
    // or the handshake could produce something the wire refuses.
    ControllerHandshake c;
    HostHandshake h;
    Step s1 = c.Begin(Secret(), NA());
    h.Begin(Secret(), NB());
    Step s2 = h.OnFrame(s1.send);
    Step s3 = c.OnFrame(s2.send);
    std::vector<uint8_t> wire;
    CHECK(Encode(s1.send, wire));
    CHECK(Encode(s2.send, wire));
    CHECK(Encode(s3.send, wire));
}
