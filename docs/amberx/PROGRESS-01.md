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

---

# Addendum — pass 02: P0-2

## Files changed

```
src/app.cpp             MaskForStorage(); line-buffered masking in LogFiltered;
                        journal writes masked at both call sites; raw-log warning
src/sessions/Session.h  logMaskBuf — the partial line held between reads
```

## The decision that shaped it

**The journal masks regardless of the screen cloak.** Those are different
concerns: the cloak toggle is about who can see the window right now; the
journal is about what lands on disk and stays there. A password typed on a
command line is a durable secret at rest whether or not anyone was screen
sharing when it was typed, so journal writes always pass through
`MaskForStorage()`.

Address and home-directory masking stay off in that path — they are not
secrets, and masking a working directory would gut the journal's usefulness.

**Session logs mask only when the cloak is on**, because logging is an explicit
user request for a recording and the cloak is the recording-safety feature.

**Raw logging is not masked and now says so.** A raw log is byte-exact by
definition; masking would corrupt the escape sequences it exists to preserve.
Enabling raw logging with the cloak on produces a status line saying the file
is unmasked, rather than letting the cloak's presence imply a protection the
file does not have.

**Masking is line-buffered.** The detectors reason about a whole line, so a
secret split across two socket reads would otherwise be written in halves that
each look innocent. A line that never ends is masked and flushed at 64 KiB
rather than growing without bound or being written in the clear to make room.

## Tests and results

`All tests passed (67908 assertions in 363 test cases)` — unchanged. The
masking function itself is covered by the existing 20 `[cloak]` cases; the two
journal call sites are two-line changes onto it.

## What could NOT be verified, and a new finding

The end-to-end check failed to exercise the path at all, and the reason is a
pre-existing bug rather than anything in this change.

A local Windows PowerShell session was driven with
`echo DB_PASSWORD=hunter2correct AKIAIOSFODNN7EXAMPLE`. The command ran, OSC 7
and the block gutter confirmed shell integration was live — and **no journal
entry was written**, masked or otherwise. The journal file's mtime never moved.

> **Finding P0-4.** `ConPty.cpp:290` emits OSC 133 `D`, `A` and `B` for
> `pwsh`/`powershell`, but **never `C`**. `pendingCmd` is lifted at the `C`
> mark, so it stays empty, so the journal guard `!s.pendingCmd.empty()` is
> never true. Local PowerShell sessions therefore produce **no journal
> entries, no captured command text, and no command blocks with output** —
> today, independently of AmberX.
>
> The bash/zsh integration two lines below does emit `C` via `PS0` and is
> unaffected. A correct PowerShell fix needs the mark emitted when Enter is
> pressed — a `PSReadLine` key handler — not from the prompt function, which
> runs before anything is typed.

So the journal masking is **implemented and unexercised**. It is a two-line
change calling a well-tested function, which is the best that can be said for
it until P0-4 is fixed or the Fedora VM is used with the bash integration.

## Phase gate

Not applicable; no phase gate attempted.

## Not done this pass

**The AmberXHost skeleton.** No process, no named pipe, no DACL, no handshake,
no Job Object supervision. `AmberXControl` still has nothing on the other end.
