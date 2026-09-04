// Handshake.h — the mutual startup handshake between AmberSSH and AmberXHost.
//
// What it defends against, and what it does not.
//
// The named pipe's DACL is the primary boundary: only a process in this logon
// session can open it. This handshake is defence in depth behind that — it
// proves that whoever answered on the pipe holds the per-launch secret that
// AmberSSH handed its child through an inherited handle, and that the child
// in turn is talking to the parent that launched it. A pipe squatter who won
// the name, a stale host from another session, or a misconfigured DACL all
// fail here instead of being trusted.
//
// It does not defend against a process running as the same user with the
// same privileges: that process can already do anything AmberSSH can, and no
// handshake changes that.
//
// Pure. Both sides are state machines over Protocol frames, so every failure
// path can be driven in a test with no process on the other end — and the
// failure paths are the point.
//
//   Controller                        Host
//   ---------- Hello(nA) ----------->
//              <---- HelloAck(nA, nB, HMAC(S, "host"|nA|nB)) ----
//   ---------- AuthProof(HMAC(S, "ctl"|nA|nB)) ----->
//
// Direction labels in the MAC input mean a proof cannot be reflected back at
// the side that produced it.
#pragma once

#include <cstdint>
#include <vector>

#include "Protocol.h"

namespace amber::amberx
{

inline constexpr size_t kSecretBytes = 32;   // kProofBytes lives in Protocol.h

// The two proofs. Labels differ so neither can stand in for the other.
std::vector<uint8_t> HostProof(const std::vector<uint8_t>& secret,
                               const std::vector<uint8_t>& nonceA,
                               const std::vector<uint8_t>& nonceB);
std::vector<uint8_t> ControllerProof(const std::vector<uint8_t>& secret,
                                     const std::vector<uint8_t>& nonceA,
                                     const std::vector<uint8_t>& nonceB);

// What a step produced. `send` is non-empty when a frame must go out.
struct Step
{
    enum class State { Continue, Done, Failed };
    State state = State::Continue;
    Frame send;
    bool hasSend = false;
};

// AmberSSH's side. Begin() yields the Hello; feed every frame that arrives on
// the control channel to OnFrame() until it reports Done or Failed. Failed is
// terminal: the caller closes the pipe and kills the host. There is no retry,
// because a peer that failed a proof is not one to negotiate with.
class ControllerHandshake
{
public:
    Step Begin(const std::vector<uint8_t>& secret, const std::vector<uint8_t>& nonceA);
    Step OnFrame(const Frame& f);
    bool Done() const { return m_state == S::Done; }
    bool Failed() const { return m_state == S::Failed; }

private:
    enum class S { Init, AwaitAck, Done, Failed };
    S m_state = S::Init;
    std::vector<uint8_t> m_secret, m_nonceA;
    Step Fail();
};

// AmberXHost's side.
class HostHandshake
{
public:
    Step Begin(const std::vector<uint8_t>& secret, const std::vector<uint8_t>& nonceB);
    Step OnFrame(const Frame& f);
    bool Done() const { return m_state == S::Done; }
    bool Failed() const { return m_state == S::Failed; }

private:
    enum class S { Init, AwaitHello, AwaitProof, Done, Failed };
    S m_state = S::Init;
    std::vector<uint8_t> m_secret, m_nonceA, m_nonceB;
    Step Fail();
};

} // namespace amber::amberx
