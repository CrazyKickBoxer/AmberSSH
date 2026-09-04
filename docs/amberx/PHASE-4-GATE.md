# AmberX — Phase 4 gate

Phase 4 is the direct libssh2 integration: AmberSSH's own session forwards
X11 channels to its own AmberXHost, no external X server and no second SSH
process. Most of the flow was wired when the transport was built (Stage 8
and the Phase 0 pass); this pass completed the identity path and the
transport checks the prompt lists. This is the report.

```
1. profile enables X11 → AmberX backend ........ DONE   profile x11Backend = 1 (ConnectionDialog "X server")
2. one isolated host per session ............... DONE   AmberXController in SshSession::ThreadMain, in a Job Object
3. 16-byte CSPRNG cookie per session ........... DONE   MakeCookieHex(); the same value is the "fake" cookie the remote sees
4. cookie installed over authenticated IPC ..... DONE   SetCookie after the HMAC handshake
5. libssh2 X11 callback registered first ....... DONE   before libssh2_channel_x11_req_ex
6. forwarding requested on the session channel . DONE
7. remote DISPLAY / xauth ...................... server side
8. unique local channel id per X11 channel ..... DONE   nextAmberxId, ChannelTable ownership on both ends
9. bytes pumped both ways ...................... DONE   PumpAmberX: 1 MiB watermarks, EAGAIN = backpressure, stop reading when the other side is full
10. disconnect tears everything down ........... DONE   Stop() → Shutdown frame → Job Object; verified: "restart after stop" with no stray process

Transport: bounded queues, watermarks, ordering, fragmentation ... PASS  (preview: a second client delivered one byte per frame)
Multiple simultaneous X11 channels .............................. PASS  (two clients, three managed windows, one closed under the other)
Private IPC controls ............................................ DONE   random name, DACL read back, mutual nonce handshake, version, max frame, type/channel validation, no object graphs, no Everyone
Host identity from the real session ............................. DONE   host · user, host-key sigil mnemonic, chrome skin → the strip

Remote xterm through AmberSSH's libssh2 connection ............... NOT RUN  needs the VM and a person at the keyboard (below)
Terminal input latency within budget ............................ NOT MEASURED
```

**Phase 4 gate: pass on mechanism; the live gate is handed to the user
with exact steps.** The controller's I/O stays synchronous inside the
session worker thread, where it is pumped alongside the terminal and the
tunnels with zero-timeout polls — the same pattern the TCP X11 path uses —
so the UI thread is never involved. That is the "existing session-worker
pattern" the prompt asks for; a separate asynchronous integration turned
out to be unnecessary.

## The live gate — how to run it

The core is opt-in, so the build that contains the server is the
`build-amberx` tree (`-DAMBERX_CORE=ON`), not `build`:

1. Run `build-amberx\Release\AmberSSH.exe` (not the one in `build\`).
2. Open the Fedora VM profile (192.168.0.18); under **SSH → X11** enable
   forwarding and choose **AmberX built-in (experimental)** as the X server.
3. Connect. The session log should say `AmberX host ready (1 ACE,
   access-allowed, current user only)`.
4. In the shell: `xeyes &`, `xclock &`, `xmessage hello &`, `xterm &`.
5. Each should appear as its own Windows window with the identity strip
   reading `192.168.0.18 · <user> · X11 FORWARDED · <sigil>`.
6. Close one from its native close button: the application should exit
   (it handles `WM_DELETE_WINDOW`) rather than be killed.
7. Disconnect: every window must vanish and `AmberXHost.exe` must be gone
   from Task Manager.

If a window appears but stays black, or an application dies with a
protocol error, `%TEMP%\amberx-host.log` has the server's side of it and
the session log the controller's; neither contains channel bytes.

## What changed in this pass

| file | what |
|---|---|
| `src/ssh/session.h` | `SshConfig::amberxIdentity`, `amberxSkin` |
| `src/ssh/session.cpp` | `Launch` built from the session: `host · user`, `SigilMnemonic(MakeSigil(fingerprint))` from the verified host key, the skin; mode label "X11 FORWARDED" until Phase 5 makes RESTRICTED/TRUSTED real |
| `src/app.cpp` | fills identity and `ChromeId()` into the config when the AmberX backend is chosen |
| `src/amberx/AmberXController.{h,cpp}` | `Launch::modeLabel` replaces a trust flag the server cannot yet honour; the preview restarts the host after stopping it |
| `src/amberx/PreviewClient.cpp` | a second client on channel 2 with one-byte frames; three managed windows; the second channel closed underneath |
| `src/amberx/host/{main.cpp,WinBackend.*}` | the mode word is displayed as given; coloured only when it is an enforced one |

## Decisions

- **The mode word tells the truth.** Until Phase 5, AmberX enforces no
  restricted/trusted distinction, so the strip says `X11 FORWARDED` rather
  than `RESTRICTED`. A badge that claims a sandbox that does not exist is
  worse than none.
- **The sigil on the strip is the same digest the safety dialog shows**
  (`MakeSigil` on the SHA256 fingerprint libssh2 reported), as a four-letter
  mnemonic. The drawn sigil belongs to Phase 7's UI work.
- **No second integration path.** The controller stays synchronous and is
  pumped by the worker; watermarks and EAGAIN handling are the existing
  tunnel code's.

## Limitations

- Not run against a real SSH server yet; the profile path from "connect"
  to "window on screen" has been exercised only up to the controller.
- Latency unmeasured; the pump processes at most 64 host frames per pass.
- One cookie per session; no `xauth`-style rotation.
- Reconnect (Session Guardian) restarts the session thread, which restarts
  the host; windows do not survive a reconnect (they cannot: the remote
  clients are gone).

## Gate verdict

**Phase 4: pass on mechanism; live gate pending the run above.**
