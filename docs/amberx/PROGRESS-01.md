# AmberX progress report 01

Per the prompt's working rules, every implementation pass reports in this
shape. **This is not a completed phase.** It is Finding P0-1 fixed and one
piece of Phase 4 built early, because that piece is independent of X.Org and
therefore not blocked on the upstream decision.

## Files changed

```
src/ssh/session.cpp                    bounded per-channel backlogs (P0-1)
src/amberx/control/Protocol.h          AmberXControl framing — new
src/amberx/control/Protocol.cpp        new
tests/AmberXProtocolTests.cpp          15 cases — new
CMakeLists.txt, tests/CMakeLists.txt   wiring
```

## Architecture decisions

**1. P0-1 is solved by declining to read, not by dropping bytes.**

`FwdTunnel`'s two backlogs now have a 1 MiB high-water mark each. On reaching
it the pump stops *reading the producing side* rather than discarding or
truncating. That makes the flow control the transport's own — TCP's receive
window in one direction, libssh2's channel window in the other — so the remote
peer is slowed rather than the data being lost. Dropping X11 bytes would
desynchronise the protocol stream and corrupt the connection; refusing to read
them cannot.

The cap is per channel, not per session, and is reached only when a producer
outruns a consumer. Terminal-scale forwarding never approaches it.

**2. AmberXControl is pure.**

No Windows types, no pipes, no sockets. It turns bytes into decisions and back.
That is what allows the dangerous half — a length field a remote X client can
influence — to be tested exhaustively with no process on the other end. The
pipe, its DACL and its nonce exchange live in the host and controller, which
are not yet written.

**3. Validate before allocating, always.**

`Decode` checks magic, version, type, length cap and channel/type pairing
*before* it touches the payload or reserves space for it. A frame claiming
4 GiB costs a header comparison to reject.

**4. `Bad` is fatal, not recoverable.**

There is no resynchronisation path. A framed stream whose framing is wrong
cannot be realigned safely, and attempting it is how a parser gets walked into
someone else's buffer. The caller closes the pipe.

**5. Channel/type separation is enforced in both directions.**

A `SetCookie` on a data channel and X11 bytes on the control channel are both
refused, on encode *and* decode. Without that, a stream of remote-originated
bytes could impersonate a control message.

## Licence and provenance changes

**None.** No third-party source has been imported. Every line added is original
AmberSSH code. The licence-gate artefacts remain deliberately uncreated —
see PHASE-0-AUDIT.md §5 for why an empty matrix is worse than none.

## Tests added and exact results

15 new cases, 309 assertions. Full suite:

```
All tests passed (67908 assertions in 363 test cases)
```

up from 348 / 67,599.

What the new cases pin down:

- A frame round-trips exactly; an empty payload is legal.
- **Every prefix** of a frame returns `NeedMore` and consumes nothing.
- Oversized lengths (`kMaxPayload+1`, 16 MiB, 2 GiB, 4 GiB) are refused from
  the header alone; exactly at the cap is still legal.
- Wrong magic, wrong version, version 0 — all fatal.
- Unknown types (0, 9, 64, 0xFFFF) refused rather than skipped.
- A type on the wrong channel kind is refused on both encode and decode.
- Frames decode back to back from one buffer with exact consumption.
- Nonce comparison is constant-time and refuses two empty values.
- Handshake and cookie payloads are exact-length-or-nothing, in both
  directions, short and long.
- A channel must be opened before use; reopen and double-close are errors;
  the 64-channel cap holds.

## Known limitations

- **There is no X server.** Nothing here renders, and nothing here speaks X11.
- **AmberXHost does not exist.** No process, no pipe, no supervision.
- The protocol has never carried a byte between two processes; it is proven
  only against its own encoder and hand-built headers.
- `AuthProof`, `HostStatus` and `HostError` have a type but no payload
  definition yet.
- Finding P0-2 (journal and logs bypass the privacy cloak) is **not fixed**.

## Performance measurements

None taken, and none are meaningful yet — no data path exists end to end. The
P0-1 change adds one integer comparison per pump iteration per direction and
cannot regress throughput below the high-water mark.

## Security findings

- P0-1 closed: a forwarded channel could previously grow its backlog without
  limit. Reachable today through ordinary port forwarding, not only X11.
- The channel/type separation described above was added after noticing that
  without it, `ChannelData` and `SetCookie` were distinguishable only by a
  field the peer controls.

## Rollback

```
git revert <this commit>
```

The P0-1 change is the only edit to shipping code and is self-contained. The
`src/amberx/` tree is additive; removing it from `CMakeLists.txt` compiles it
out entirely with no other effect.

## Phase gate

**Not applicable — no phase gate was attempted.** Phase 1's gate requires a
reproducible upstream import and a build of DIX/MI/fb, neither of which has
begun. This pass deliberately did work that is *not* gated on the upstream
decision, so that decision can be made unhurried.

## Next, in order

1. **P0-2** — move redaction to the logging boundary. Independent of AmberX,
   and required by working-rule 9 before AmberX introduces cookies and
   clipboard payloads to the log path.
2. **AmberXHost skeleton** — process, Job Object supervision lifted from
   `ConPty`, named pipe with a random endpoint and an explicit DACL, and the
   nonce handshake this protocol already defines. Still no X11: the host can
   log what it receives and prove the transport end to end.
3. **Route X11 channels to it** behind a feature flag, replacing
   `ConnectX11Display`'s TCP socket. At that point the whole path exists with a
   stub where the server goes.
4. **Then, and only then, the upstream decision** — because by that point the
   only unknown left is the one that actually matters: whether X.Org's
   DIX/MI/fb will build on Windows with an approved toolchain (Finding P0-3).
