# AmberX progress report 02 — P0-4 and the host skeleton

Per the prompt's working rules. **Still not a completed phase**, and still no
X11. What now exists is the complete process boundary the server will sit
inside, proven end to end between two real processes.

## Files changed

```
src/platform/ConPty.cpp               P0-4: OSC 133 C via PSReadLine; marks returned in-prompt
src/utility/Hash.{h,cpp}              HmacSha256 (bcrypt, HMAC flag)
src/amberx/control/Protocol.{h,cpp}   HelloAck carries a proof; AuthProof, HostStatus, HostError
src/amberx/control/Handshake.{h,cpp}  mutual HMAC handshake, both sides, pure — new
src/amberx/control/Pipe.{h,cpp}       DACL'd named pipe + framed overlapped I/O — new
src/amberx/host/main.cpp              AmberXHost.exe — new
src/amberx/AmberXController.{h,cpp}   launch, job, secret delivery, handshake, preview — new
src/main.cpp                          --preview-amberx
tests/AmberXHandshakeTests.cpp        12 cases — new
tests/AmberXProtocolTests.cpp         updated for the proof-carrying HelloAck
CMakeLists.txt                        AmberXHost target
```

## Architecture decisions

**1. The pipe's DACL is verified, not requested.** `CreateServerPipe` builds
`D:P(A;;GA;;;<user SID>)`, creates the pipe, then reads the DACL back off the
object with `GetSecurityInfo` and checks: exactly one ACE, access-allowed,
`EqualSid` to the current user. Anything else closes the pipe before a client
can connect. The preview prints what it found. "We asked for a private pipe"
and "we have one" are different claims; only the second is made.

**2. The secret never touches a command line.** It travels on an anonymous
pipe whose read end alone is inherited, via an explicit
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` — so no other handle this process holds
leaks into the child either. The host reads 32 bytes and closes the handle.
A process listing shows a pipe name and a handle number, nothing usable.

**3. The host is never outside the Job.** Created `CREATE_SUSPENDED`,
assigned to a job with `KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS` = 1 and
`DIE_ON_UNHANDLED_EXCEPTION`, then resumed. It cannot spawn children, it dies
when AmberSSH does, and it cannot leave a crash dialog behind. Belt and
braces: it also watches its parent's process handle and exits on its own.

**4. The handshake is mutual and direction-bound.** HMAC-SHA256 over
`label | nonceA | nonceB` with different labels each way, so a proof captured
in one direction is useless in the other. Echoed nonces bind each reply to
the specific Hello that prompted it. `Failed` and `Done` are both terminal:
nothing after either is accepted, so a peer cannot rewrite the session's
nonces after the fact. Both sides zero their copy of the secret on
completion or failure.

**5. The client end refuses impersonation.** `ConnectClientPipe` opens with
`SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS`, so a server on that name —
squatter or not — can never act as the host process.

**6. `Bad` closes the pipe.** Framed I/O never attempts to resynchronise.

**7. Controller I/O is synchronous, on purpose, for now.** It is exercised by
the preview on a throwaway path. Wiring it into the SSH worker asynchronously
— replacing `ConnectX11Display`'s TCP socket — is the next increment and a
deliberate separate change to shipping code.

## P0-4

The PowerShell integration emitted `D`, `A`, `B` and never `C`, so
`pendingCmd` was never lifted and PowerShell sessions produced no journal
entries, no command text and no block summaries. Fixed with a PSReadLine
`Enter` handler emitting `C` before `AcceptLine()` — the pattern VS Code and
Windows Terminal use — installed only when the module is present.

Adding `C` exposed an older ordering bug: the marks were `Write-Host`ed
immediately while the prompt text was *returned* and written afterwards, so
`B` landed at column 0 and the lifted command included the prompt. The blast
radius would then have parsed `PS C:\Users\me> rm -rf /` as the program
`ps` and flagged nothing. All marks are now returned inside the prompt string
(as starship does), giving the wire order `A`, prompt, `B`.

**Verified live.** A PowerShell command containing a password assignment and
an AWS key produced a journal entry reading exactly
`echo DB_PASSWORD=[redacted] [redacted]` — prompt excluded, both secrets
masked on disk, exit status and duration recorded. The command block gained
its full summary (`ok  73ms  139 B  cwd`) that it had never had before. That
also retires the "implemented and unexercised" caveat on P0-2.

## Tests added and exact results

12 handshake cases; the protocol suite updated. Full suite:

```
All tests passed (67961 assertions in 375 test cases)
```

Handshake cases pin down: the happy path; proofs are real MACs that differ by
direction, secret and each nonce; a host with the wrong secret is refused; a
controller with the wrong secret is refused *at the proof step specifically*;
a reflected proof is refused; a HelloAck echoing a stale nonce is refused;
the wrong type at any state fails; a handshake frame on a data channel fails;
Failed is terminal; Done is terminal; bad sizes and double-Begin are refused;
every emitted frame passes `Encode`'s own checks.

## What was seen running

`AmberSSH.exe --preview-amberx`, two real processes:

```
start + handshake            ok
pipe DACL (read back)        ok  1 ACE, access-allowed, current user only
host alive                   ok
set cookie                   ok
status after cookie          ok  cookieSet=1
open channel 1               ok
status after open            ok  open=1
send 4096 bytes              ok
send 4096 more               ok
data on closed channel refused ok
close channel 1              ok
status after close           ok  open=0 bytesIn=8192
double close refused         ok
control-channel data unencodable ok
host exited after shutdown   ok
PREVIEW PASSED
```

`tasklist` afterwards shows no `AmberXHost.exe`: the Job Object cleaned up.

## Known limitations

- **There is no X server.** The host counts bytes on channels and does
  nothing with them. It is the stub where the server goes.
- The controller has never been driven from the SSH worker; `ConnectX11Display`
  still opens a TCP socket to a local X server.
- The handshake's shared secret is same-user-visible in principle (the DACL
  already admits this user); it defends against other users, squatters and
  misconfiguration, not against a process running as you.
- `--preview-amberx` proves the transport on this machine only. Nothing here
  has run on a second Windows install or a second user account.
- No AppContainer or restricted-token confinement yet — the Job Object is the
  first rung of the prompt's Phase 5 ladder, not the last.

## Security findings

- The pipe DACL is enforced by read-back (decision 1). Before that change it
  was requested and trusted.
- P0-4's ordering bug was a latent **risk-analyser bypass** on PowerShell
  sessions: exact-mode command text that began with the prompt parsed as a
  harmless program. It could not fire before because `C` was never emitted;
  it would have fired the moment `C` was added without the ordering fix.

## Licence and provenance

No third-party source imported. Everything is original. Licence-gate files
remain deliberately uncreated.

## Rollback

`git revert` of this commit. `AmberXHost` is a separate target and
`add_dependencies` is the only coupling; removing the `src/amberx/` lines from
`CMakeLists.txt` compiles it all out. The ConPty change is self-contained.

## Phase gate

Not attempted. This is the transport under Phase 4, built before Phases 1–3
because it does not depend on X.Org — which is still the decision that
matters, and still needs authorisation to fetch the source.

## Next, in order

1. **Route X11 channels through the controller** behind a profile flag: the
   SSH worker hands `x11` channels to AmberXHost instead of a TCP socket.
   Requires making the controller's I/O asynchronous on the worker thread.
2. **Then the upstream decision** — fetch pinned X.Org, and attempt Phase 1's
   gate: DIX+MI+fb compiling under an approved toolchain against a stub DDX.
   If it fails, everything above is still a correct, useful process boundary
   for whatever server does go in it.
