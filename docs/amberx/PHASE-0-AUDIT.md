# AmberX — Phase 0 audit

Repository and feasibility audit, per the AmberX implementation prompt. No
production code has been written; this document exists to make the Phase 1
decision on evidence rather than assumption.

**Status: half complete.** The AmberSSH-side audit below is finished and
verified against the real tree at commit `f5e6cfd`. The upstream X.Org
feasibility audit is **not started** and cannot be, for a reason recorded in
§6.

---

## 1. AmberSSH integration points

Verified by reading the tree, not inferred. 109 source files, ~39,000 lines.

### 1.1 libssh2 session ownership and threading

| Question | Answer | Evidence |
|---|---|---|
| Where is a session created? | One `std::thread` per session, dispatched on `cfg.protocol` | `session.cpp:601-607` |
| Which entry point is SSH? | `SshSession::ThreadMain` | `session.cpp:607` |
| Blocking or nonblocking? | **Both, in phases** | see below |
| Where is it freed? | `libssh2_session_free` on both the failure and the normal path | `session.cpp:742`, `1385` |
| How is it joined? | `m_thread.join()` in `Disconnect()` | `session.cpp:680` |

The phasing matters for AmberX and is easy to get wrong:

```
libssh2_session_init()                     session.cpp:777
libssh2_session_set_blocking(session, 1)   session.cpp:783   ← handshake + auth
  ... known_hosts, host-key prompt, authentication, PTY, shell ...
libssh2_session_set_blocking(session, 0)   session.cpp:1149  ← the pump loop
```

So the connection is **blocking through authentication and nonblocking for the
session loop**. AmberX's channel pumping lives entirely in the nonblocking
phase, and `LIBSSH2_ERROR_EAGAIN` there is backpressure, exactly as the prompt
requires.

### 1.2 Callbacks

Exactly one callback is registered anywhere in the codebase:

```cpp
libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_X11, …)   session.cpp:1101
```

It runs **on the network thread, inside a libssh2 call**, and does the minimum
possible: pushes the new channel onto a `thread_local` vector
(`tlX11Pending`) which the pump loop drains. That is the correct shape and
AmberX should preserve it — the callback must not touch AmberX state directly.

### 1.3 Channel pumping — and the first real gap

`PumpTunnels` (`session.cpp:349`) services every forwarded channel, X11
included. Per-tunnel state is `FwdTunnel`, holding:

```cpp
std::vector<uint8_t> toChannel;  // socket → channel backlog
std::vector<uint8_t> toSock;     // channel → socket backlog
```

**These are unbounded.** They grow with whatever the peer sends and there is no
watermark, no pause, and no cap. For terminal-scale forwarding that has never
mattered. For X11 it does: a remote client that renders faster than the local
side consumes will grow `toSock` without limit, and the prompt's Phase 4
explicitly requires bounded queues with high/low watermarks.

> **Finding P0-1 (must fix before Phase 4).** `FwdTunnel::toChannel` and
> `::toSock` need caps and a stop-reading-the-other-side rule. This is a change
> to existing, working forwarding code and needs its own regression test.

### 1.4 Existing X11 code and its completeness

977 lines across `src/remote/`, added in Stage 8:

| File | Does | State |
|---|---|---|
| `XAuth.{h,cpp}` | cookie generation, `.Xauthority` parse, setup-packet parse and cookie substitution | complete, 13 test cases |
| `RemoteDisplay.{h,cpp}` | X server detection/ranking, launch args, RemoteApp plan | complete, 13 test cases |
| `session.cpp` X11 path | `x11-req` with fake cookie, per-channel substitution, connect to `127.0.0.1:6000` | complete |

This is the **client half** — it carries X11 to a server someone else provides.
None of it is a server, and `ConnectX11Display` (`session.cpp:529`) opens a
socket to `127.0.0.1:6000` expecting something to be listening.

**AmberX replaces that socket with an AmberXHost transport.** The cookie work
is directly reusable: AmberSSH already generates a per-session fake cookie and
already owns the substitution point. Phase 4's step 3–4 are largely done.

Per `docs/STAGE8-REMOTE-DISPLAY-REPORT.md`, none of it has run against a real X
server or a real X11 channel.

### 1.5 Process launching, Job Objects, IPC

| Primitive | Present? | Where |
|---|---|---|
| `CreateProcessW` | yes, two call sites | `ConPty.cpp:72`, `:427` |
| Job Object | **yes, with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`** | `ConPty.cpp:448-454` |
| Anonymous pipes | yes | `ConPty.cpp:60`, `:319-320` |
| Named pipes | **no** | — |
| Crash reporting | **no** | — |

The Job Object pattern is already proven here for exactly the purpose AmberX
needs — closing a tab takes the child process tree with it. AmberXHost
supervision should reuse it rather than invent one.

`AmberXControl` is new ground: there is no named-pipe infrastructure, no frame
protocol, and no IPC handshake anywhere in the tree.

### 1.6 DirectX 12 ownership

```cpp
ComPtr<ID3D12Device>        m_device;        dx/device.h:72
ComPtr<ID3D12CommandQueue>  m_directQueue;   dx/device.h:73
ComPtr<ID3D12CommandQueue>  m_copyQueue;     dx/device.h:74
ComPtr<IDXGISwapChain4>     m_swapchain;     dx/device.h:75
```

One device, one swap chain, bound to the single main HWND. Consequences for
AmberX:

- **AmberXHost cannot share this swap chain.** It is owned by the terminal
  process and tied to one window.
- Native-window mode (Phase 3) needs AmberXHost to create its own device and
  present to its own HWNDs. That is clean and has no interaction with the
  terminal renderer.
- Pane-embedding mode (Phase 7) needs the shared-texture design the prompt
  already specifies. The existing **copy queue** is the right place to schedule
  those uploads.

### 1.7 Session, pane, tab and workspace model

```cpp
std::vector<std::unique_ptr<Session>> extraPanes;   // ids 1..N at index id-1
PaneLayout layout;
PaneId focus = 0;
```

The root `Session` **is** pane 0 (`Session.h:250-258`). A tab is a `Session`
tree; `PaneLayout` is a binary split tree keyed by pane id. AmberView must bind
an AmberXHost to a **tab**, not a pane — panes come and go within a tab and
share one SSH connection.

### 1.8 Settings versioning

`ConnectionProfile::kSchemaVersion`, read and written as `schemaVersion` in
`ProfileStore.cpp:481-505`. Migration exists and Phase 7's new profile fields
should ride it.

### 1.9 Security prompts and identity rendering

`src/ui/SafetyDialog.cpp` — painted, skin-aware modals for host-key
confirmation (with the sigil) and destructive-command confirmation. Phase 3's
anti-spoofing host badge should reuse `DrawSigil()`, which is already exported
for exactly this "same figure everywhere" reason.

### 1.10 Logging and redaction — second gap

`CloakText()` / `MaskLine()` exist (`app.cpp:12085`) and are applied to the
terminal draw path. Grepping the journal and logging paths finds **no other
call sites**.

> **Finding P0-2.** The command journal and session logs do not currently pass
> through the cloak. Prompt working-rule 9 forbids logging authorization
> cookies and clipboard payloads; AmberX will introduce both. The redaction
> point needs to move to the logging boundary before Phase 5.

---

## 2. Upstream feasibility

**Not started.** See §6.

---

## 3. Windows port strategy

The prompt asks for a recommendation between three options. On the AmberSSH-side
evidence alone, and on public knowledge of the X.Org tree rather than an audit
of it:

**Option A (upstream DIX/MI + new `hw/amberwin` DDX) remains the right
direction**, for the reason the prompt gives: RENDER, XKB and XI2 are where
compatibility actually lives, and reimplementing them from specification
(Option B) is where a from-scratch server dies.

But the audit must record the risk that Option A carries, because it is not the
DDX:

> **Finding P0-3 (the principal technical risk).** The X.Org server's
> device-independent code is not written to build on Windows without a POSIX
> compatibility layer. That is *why* Cygwin/X exists and why VcXsrv is a
> substantial project rather than a packaging exercise. DIX, MI and fb assume
> POSIX file, signal, socket and threading primitives, and the build system is
> Unix-oriented. Bringing DIX+MI+fb up under MSVC or clang-cl is a porting
> project **before any AmberX-specific code is written**, and it is the single
> largest unknown in the plan.

The prompt's rule against Cygwin/X material makes this harder, not easier: the
existing permissively-licensed Windows port work for X.Org is largely *in*
`hw/xwin`, whose usability here depends on the file-by-file audit the prompt
requires and which has not been done.

That is not an argument against Option A. It is an argument for Phase 1 being
scoped as "prove DIX+MI+fb compiles and links on Windows with the approved
toolchain, against a stub DDX" and for treating that as a genuine gate that can
fail.

---

## 4. What this audit changes about the plan

1. **Phase 4 has a prerequisite.** Finding P0-1 — bounded channel queues — is
   a change to shipping code and should be done and tested first.
2. **Phase 1's gate should be sharper.** "Reproducible clean build" should mean
   *DIX/MI/fb compiling against a stub DDX*, because that is the finding that
   decides whether Option A is viable at all.
3. **Phase 5's redaction requirement has a prior gap** (P0-2) that exists today,
   independent of AmberX.
4. **Cookie work is largely done.** Phase 4 steps 3–4 map onto existing,
   tested code.
5. **Job Object supervision is a solved problem here** and should be lifted
   from `ConPty`, not rewritten.

---

## 5. Provenance status

No third-party source has been imported. No VcXsrv material exists in the
working tree, the build cache, or the history — trivially true, since nothing
has been fetched.

The license-gate artefacts the prompt requires are **not created**, because
there is nothing yet to record:

```
docs/amberx/LICENSE-MATRIX.md          not created — no components imported
docs/amberx/SOURCE-PROVENANCE.md       not created — no components imported
docs/amberx/REJECTED-COMPONENTS.md     not created — no components evaluated
third_party/amberx/…                   not created
cmake/AmberXLicenseGate.cmake          not created
```

Creating them empty would be worse than not creating them: a license matrix
with no entries reads like a completed gate.

---

## 6. Why the upstream half is not done

Phase 0 requires auditing the current stable X.Org `xserver` source to identify
the minimal approved source set and every dependency's licence. That requires
**fetching the X.Org server source tree**, which is a download I have not made
because it needs explicit authorisation, and because the audit it enables is
large: the tree is on the order of several hundred thousand lines across
hundreds of files, each needing a licence determination for the matrix.

There is a second reason to pause here rather than push on. The prompt's
Definition of Done describes a product that runs GTK, Qt and Tk applications
correctly. Reaching that bar means RENDER, XKB, XI2, XFIXES, DAMAGE, RANDR and
SYNC all behaving well enough for real toolkits, plus a window manager, plus
the port in Finding P0-3. On the estimate I gave earlier in this project, that
is a multi-person-year effort, and the phasing in the prompt — while correctly
ordered — does not change the total.

That is not a reason to refuse the work. It is a reason for the next decision
to be made deliberately:

**What I need from you to continue:**

1. **Authorisation to fetch** the upstream X.Org `xserver` source at a pinned
   release, plus `xorgproto`, `pixman`, `libXfont2` and the other candidates,
   for licence audit. Nothing gets compiled at this stage.
2. **A scope decision.** The smallest thing that proves or kills Option A is
   Phase 1's build gate: DIX+MI+fb compiling on Windows against a stub DDX. I
   would recommend making *that* the next deliverable and nothing else, because
   if it fails the rest of the plan does not matter, and if it succeeds every
   later estimate gets more honest.

Findings P0-1 and P0-2 are independent of that decision and can be fixed now.
