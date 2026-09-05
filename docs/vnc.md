# VNC in AmberSSH — the particle desktop

AmberSSH's sixth protocol. A VNC server's framebuffer is rendered as one GPU
particle per pixel (or two, three, four), each fixed to its source pixel and
coloured from it every frame, so the remote desktop is both a usable desktop
and the same particle field the terminal is made of.

This file says what is implemented, what has been verified and how, and what
has not. The two are kept apart throughout.

**Status (2026-09-05):** Phase 1 (RFB client and transport), Phase 2 (the
D3D12 particle desktop) and Phase 3 (VeNCrypt/TLS, the connection dialog's
VNC page, palette commands, clipboard both ways, SSH-closure handling) are
implemented. What is verified, and by what, is the table under
*Status: implemented vs verified* near the end; interoperability with a
real server remains unverified and Tight is not implemented.

## Architecture

```
  RFB server ──TCP or SSH forward──▶ VncSession (worker thread)
                                        │  RfbClient: handshake, decode
                                        │  Framebuffer (CPU, BGRA, worker-owned)
                                        │  DirtyRegion
                                        ▼
                               pending Damage  (rects + copied pixels, one lock, O(1) swap)
                                        │
                                        ▼
  render thread: DesktopParticles ── Upload (dirty rects, delta → energy)
                                  ── energy pass ── particle sim ── draw
                                        │
                                        ▼
                               scene target ── bloom ── composite ── swap chain
```

| piece | file | owns |
|---|---|---|
| DES / VNC authentication | `src/vnc/RfbDes.*` | the FIPS 46-3 core and the reversed-key variant |
| wire format | `src/vnc/RfbProtocol.*` | RFC 6143 codecs, a reader that consumes nothing on a short read |
| decoders | `src/vnc/RfbDecoders.*` | Raw, CopyRect, ZRLE, Hextile, Cursor; the framebuffer and dirty region |
| state machine | `src/vnc/RfbClient.*` | RFB 3.3 / 3.7 / 3.8, security None, VNC Authentication and VeNCrypt's steps, updates, pseudo-encodings |
| TLS | `src/vnc/RfbTls.*` | the VeNCrypt subtype policy, the OpenSSL client, certificate verdicts and the pin store |
| worker | `src/vnc/VncSession.*` | the socket, the client, the TLS layer and the certificate question, reconnect, the damage hand-off |
| keys | `src/vnc/Keysyms.*` | Windows virtual keys and code points to X11 keysyms |
| renderer | `src/render/desktop.*`, `shaders/desktop_*.hlsl`, `shaders/motion_fields.hlsli` | the particle desktop |
| the tab | `src/sessions/VncTab.h`, `src/app_vnc.cpp` | the tab's state, events, input, overlay |
| checks | `src/vnc/SelfCheckServer.*`, `src/app_vnccheck.cpp` | `--vnc-selfcheck`, `--vnc-bench` |

A VNC tab **is** a `Session` (`sessions/Session.h`) carrying a `VncTab`. That
is what lets the tab strip, panes, workspaces, quake mode and every other
tab-lifecycle path work unchanged; the session's terminal grid is simply
unused.

## Threading and ownership

**The worker thread** (`VncSession::Worker`) owns the socket, the RFB state
machine, the ZRLE zlib stream, the decoded framebuffer and the dirty region.
It never touches Direct3D. It waits on the socket and on a wake event — no
tick — and after each decode step publishes a *Damage*: the rectangles that
changed and their pixels, copied out under a lock held only for the copy.

**The render thread** owns the texture, the particle buffers and the energy
textures. `VncSession::TakeDamage` swaps the pending Damage out in O(1); a
large update never stalls a frame and a frame never stalls decoding.

**Order.** There is exactly one decoder and it runs in arrival order, so a
later CopyRect sees what earlier rectangles left, and ZRLE's
connection-spanning zlib stream is never reset per rectangle.

**Bounds.** The pending Damage is bounded at one framebuffer's worth of
pixels: past that it collapses to a single copy of the whole framebuffer,
which is also what the renderer's per-frame-slot staging buffer is sized for.
A network backlog therefore costs one full upload in a frame, never
unbounded memory or an unbounded frame.

**libssh2.** For "VNC over SSH" the tab asks its SSH session to add a local
forward (`SshSession::AddForward("L0:host:port")`, thread-safe); the bound
port comes back as `SshEventType::ForwardUp` and is relayed to the tab on
the UI thread (`VncSession::OnForwardUp`). libssh2 is touched only by the
session's own thread; the VNC worker sees plain TCP to loopback.

**Lifetime.** Closing a tab disconnects the worker (joined) and then waits
for the GPU before the desktop's resources go with the session. Focus loss
and disconnect release every held key and button (`VncReleaseAll`).

## Protocol support (implemented)

* RFB **3.3, 3.7 and 3.8**, with their version-specific rules: the reply
  never exceeds the server's offer; 3.3 states one security type and takes
  no answer; None is followed by no SecurityResult before 3.8; a refusal
  reason exists only from 3.8. Each is a test (`tests/VncClientTests.cpp`).
* Security **None** and **VNC Authentication** (DES, reversed key bits, in
  C++ — no OpenSSL legacy provider).
* **VeNCrypt** (security type 19) over the existing OpenSSL, when the
  profile's Encryption is set to it (`src/vnc/RfbTls.h`). Version 0.2 is
  exchanged, the server's subtype list is filtered by a policy written
  here rather than taken from the server, TLS 1.2+ is negotiated on the
  same socket, and the subtype's own authentication runs inside it:

  | subtype | wire | taken? |
  |---|---|---|
  | X509None (260) | X.509 server certificate, no further auth | yes, when there is no password |
  | X509Vnc (261) | X.509, then the VNC challenge inside TLS | yes, preferred with a password |
  | X509Plain (262) | X.509, then username + password inside TLS | only when the profile names a user; X509Vnc is preferred |
  | TLSNone / TLSVnc / TLSPlain (257–259) | anonymous Diffie-Hellman | **no** — encrypted to whoever answered, not to the server |
  | Plain (256) | username + password in the clear | **no** |

  A profile that requires TLS takes type 19 or nothing: a plaintext type
  the server also offers is never chosen, a server without VeNCrypt (or
  an RFB 3.3 server, which has no list to find it in) is refused with the
  reason named, and a refusal is not retried. **A TLS-configured
  connection is never downgraded to an unencrypted one.**

  Certificates verify against the Windows ROOT and CA stores with the
  host name checked (SNI unless the host is an address). A chain that
  verifies connects silently. Anything else — a self-signed server, which
  is most VNC servers — is shown as an SSH-style host-key question: the
  leaf's `SHA256:` fingerprint, its subject and why it did not verify.
  Accepting pins the fingerprint in
  `%LOCALAPPDATA%\AmberSSH\vnc_known_hosts` (`host:port SHA256:…`, one per
  line, fingerprints only, never a certificate). A later connection whose
  certificate differs from the pin gets the changed-key alarm treatment,
  and rejecting it ends the attempt without a retry. The overlay's first
  line says `TLSv1.3 X509Vnc` or `plaintext`.
* Pixel format: 32 bpp, depth 24, little-endian, shifts 16/8/0 — BGRX on
  the wire, `0xFFRRGGBB` in memory with the alpha forced opaque.
* Encodings **Raw, CopyRect, ZRLE, Hextile**; pseudo-encodings
  **DesktopSize** (acted on before the bounds check) and **Cursor**;
  **ContinuousUpdates** on the server's unprompted announcement, with the
  FramebufferUpdateRequest path otherwise (the next request is sent as soon
  as an update's rectangle count is read; at most one outstanding).
* Client messages SetPixelFormat, SetEncodings, FramebufferUpdateRequest,
  KeyEvent, PointerEvent, ClientCutText, EnableContinuousUpdates; server
  messages FramebufferUpdate, SetColourMapEntries (consumed), Bell,
  ServerCutText, EndOfContinuousUpdates.
* Every length and dimension is validated before allocation; rectangle
  bounds use 32-bit arithmetic; ZRLE's inflated size is bounded by what its
  tiles could be; palette indices, run lengths and subrects are checked.
  Malformed input fails the stream as `FailureKind::Protocol`.
* Reconnect: transport failures retry with exponential backoff (1 s
  doubling to 30 s, a bounded number of attempts), and every reconnection
  starts with a fresh full picture. A rejected password or a refusal is
  reported once and never retried.

Not implemented: **Tight** (optional in the brief; left out rather than
shipped unverified — a server offering only Tight and Raw is served in
Raw), RRE/CoRRE, the Extended Clipboard pseudo-encoding (cut text is
Latin-1 with LF endings, as the core protocol specifies; code points above
U+00FF become `?` outbound), and VeNCrypt's anonymous and plaintext
subtypes (by policy, above).

## The particle desktop

`particle_count = width × height × density`, density 1–4. The budget is
8 388 608 particles (3840×2160 at density 1 and 1920×1080 at density 4 both
fit) and a quarter of the adapter's memory at 16 bytes per particle. When the
request exceeds it, density comes down first; when density 1 still does not
fit, the desktop is **sampled**: every stride-th pixel per axis gets the
particle, `Layout.sampled` is set, and the effective resolution is on the
overlay (`SAMPLED 1/s -> WxH`). Nothing claims one particle per pixel that
is not.

Each particle's home is its pixel's centre on screen (with quadrant offsets
at density 2–4 so they tile rather than stack, and a jitter of up to 1.5 px
at solidity 0). Its colour is fetched from that pixel **every frame** in the
vertex shader; no colour is stored on a particle, so wherever the swarm has
carried one it shows the colour it will come home to.

**Solidity 0** — a loose swarm: a spring to home, curl-noise drift, the
selected motion style as a force field (all 23 styles, `motion_fields.hlsli`),
the pointer's push, shockwaves, audio, and a burst from its own pixel's
disturbance energy; drawn as soft additive discs.

**Solidity 1** — the faithful desktop: there is no integration at all. The
particle is *placed* on its home with zero velocity, every non-spring force
is multiplied by zero, it is drawn as a hard 1-px quad with premultiplied
"over" blending so nothing is summed, and the scene holds the framebuffer's
exact linear colour. At native scale this is verified pixel for pixel
(below). The composite's appearance controls — Paperwhite's greyscale, Pixel
Art's blocks, bloom, scanlines, vignette — still apply, because they are
explicitly selected effects; faithful means the *scene* is exact.

**Disturbance.** Each uploaded rectangle is compared against a CPU shadow of
the framebuffer; the largest channel delta, times the disturbance setting,
is written into an injection texture. A compute pass folds injections into
a persistent energy texture with exponential decay; the sim reads the energy
at each particle's source pixel and throws it outward by it.

**Uploads** take only the damage rectangles, through the frame's persistent
upload ring when they fit and through a per-frame-slot staging buffer sized
for one full framebuffer when they do not, with 256-byte row pitch, 512-byte
placement, tracked resource states, and reuse protected by the frame fence
(three frames in flight). A first picture, a resize and a recovery are full
uploads by design.

## Settings

**VNC** is the seventh connection type on the connection manager's Session
page, and **Connection > VNC** is its page. The page is built from the
same field table as every other page, so it takes every interface skin
without work of its own. Host and port are the Session page's; the
username there is what X509Plain would send.

| field | on the page | meaning | default |
|---|---|---|---|
| (password) | Password | VNC Authentication, or the VeNCrypt subtype's; remembered, it is the profile's ordinary Password secret in the Credential Manager, never in `profiles.json` | — |
| `vncViewOnly` | View only | no keys, pointer or clipboard go out | off |
| `vncViaProfileId` | Tunnel through a connected SSH session | that session's profile name (or id); blank = direct TCP | direct |
| `vncTls` | Encryption | None, or VeNCrypt/TLS with an X.509 certificate — required, never downgraded | None |
| `vncDensity` | Particles per pixel | 1–4; reduced past the GPU budget, and the overlay says so | 1 |
| `vncSolidity` | Solidity % | 0 swarm … 100 faithful | 100 |
| `vncParticleSize` | Particle size | 1–3 px | 1 |
| `vncDisturbance` | Disturbance % | burst strength, 0–200 | 100 |
| `vncEncodings` | Encodings | ZRLE first / Hextile first / Raw only | ZRLE first |
| `vncCursorMode` | Cursor | local particle cluster in the server's shape / server-drawn | local |
| `vncClipboard` | Clipboard | Disabled / Ask each transfer / Remote → local / Local → remote / Bidirectional | Ask |
| `vncDesktopSize` | Desktop size | the size asked of the server (below): the server's own / fit this window and follow it / 1280×720 / 1366×768 / 1600×900 / 1920×1080 / 2560×1440 / custom | fit the window |
| `vncCustomW`, `vncCustomH` | Custom width / height | for the custom choice | 1600 × 900 |
| `vncGlow` | Glow % | bloom on the desktop; the terminal's is 100 | 40 |
| `vncVividness` | Vividness % | saturation about each pixel's luminance and contrast about mid grey, in linear light; 100 = the decoded colours, and faithful mode pins it there | 130 |
| `vncMotion` | Motion speed % | the tempo of the swarm's drift, the motion style's field and the effects | 200 |
| `vncFxShock`, `vncFxEdge`, `vncFxHeat`, `vncFxMaterialise` | Effect: … | the four effects below; any on = not pixel-exact | on |

A desktop drawn below native size (the window is smaller than the
framebuffer) lands 1/scale² particles on each screen pixel; in the
additive (non-faithful) draw their colours are scaled by scale² so they
sum to the pixel, not past it, and no sprite is drawn under one screen
pixel. Without that a scaled desktop washes out.

Numbers typed outside their range are clamped where they are used. The
tab's copies of View only and the placement follow the palette toggles
below without changing the saved profile.

## Desktop size

The client offers **ExtendedDesktopSize** (−308) ahead of DesktopSize. A
server that speaks it (TigerVNC's Xvnc does; RealVNC and most others do
not) answers with its screen layout, which is also the sign that it takes
**SetDesktopSize** (message 251). The profile's Desktop size is then asked
for as soon as the connection is up, held until that layout is known, and
sent with the server's own first screen id and flags so the request is the
one the server expects. "Fit this window" asks for the content area's size
and asks again, after a 0.4 s pause, whenever the window's size changes, so
a drag asks once at its end. The answer is an ExtendedDesktopSize
rectangle: granted, the framebuffer resizes and a full picture is
requested; refused, the status is named on the notice strip (prohibited by
the administrator, out of resources, invalid layout) and the size stays. A
server without the extension gets one notice saying so; the picture is then
scaled to fit as before. Sizes are clamped to 320–8192 and made even.

## The composite for a desktop

The terminal's composite pass is tuned for glowing particles on black: a
partial filmic curve above 0.75, scanlines, a vignette, an exposure, and a
gamma-2.2 encode. Through it a document window's white comes out a dull
0.9 grey — the "washed out" look. A desktop tab takes its own path in the
same shader (`desktopMode`): bloom at the profile's Glow, then the colour
is clipped and encoded with the exact sRGB transfer function; no filmic
curve, scanlines or vignette, exposure 1. At faithful settings the back
buffer therefore holds the server's bytes, not only the scene target. The
draw clips every particle's base colour to 1 before the effects add to it,
so a white window cannot bloom on its own; only an edge's boost (×1.35 at
most) and the heat tint can cross the bloom threshold.

## Effects

Four effects, each a check box on the VNC page (all on by default), each
driven by real signal — the input, the framebuffer, or the change in it —
never by random sparkle. **Any effect on takes the desktop out of the
exact-pixel contract**: the overlay's first line says `FX: shock edge
heat materialise` instead of `faithful`, and `--vnc-selfcheck`, which
measures that contract, runs with all four off (`AMBER_VNC_FX=1` leaves
them on for a look; its pixel checks then fail, as they must).

| effect | signal | what happens | where |
|---|---|---|---|
| shockwave | a button press inside the picture | a ring leaves the click at 900 px/s pushing particles as it passes, fading in about a second; a right click pulls inward instead | `VncTab::shock*` → `DesktopCB::shockX/Y/Time/Amp`, `desktop_sim.hlsl` |
| edge glow | the luminance gradient at each pixel, from its four neighbours in the framebuffer texture | particles on edges (window frames, title bars, text outlines) are brightened past 1, which bloom turns into a halo; flat fills are untouched | `desktop_draw.hlsl`, vertex stage, four extra loads per particle |
| heat | the disturbance energy texture, already injected from colour deltas | a changed pixel's particle runs warm (an amber tint) and lifts a few pixels, cooling with the energy; the sim also lets it burst a little even at solidity 100 | `desktop_draw.hlsl` reads energy as an SRV (t3); `desktop_sim.hlsl` |
| materialise | a new particle buffer (connect, resize) | particles start scattered and dim, fly home on a critically damped spring for 1.4 s while brightening, then the ordinary rules place them | `bornTime` in the CB, `kMaterialiseSeconds` |

At solidity 100 with an effect on, a particle that has all but returned
home (within a twentieth of a pixel, nearly at rest) is placed exactly, so
the desktop at rest is the desktop, not a blur of sub-pixel remainders;
the swarm forces (curl, motion style, the pointer's push) stay scaled by
(1 − solidity) and are zero there. Turn all four off for the pixel-exact
picture the acceptance tests verify.

## Redraw styles

How a changed region appears, chosen on the VNC page or from the menu.
Two textures make them possible: what each pixel was before its last
change (copied out of the framebuffer texture before each upload
overwrites it) and when it changed (a timestamp the energy pass writes for
every marked pixel). With the old colour, the new colour, the change age
and the particle's seed, each style is one small function
(`Redraw` in `desktop_draw.hlsl`), runs 0.32 s, and ends exactly on the
new colour. Any style but None is an effect: the picture is not
pixel-exact during those 0.32 s, and the self-check runs with None.

| style | what happens |
|---|---|
| None | the new pixels at once (the exact contract) |
| Burn | the old colour heats to a flame, chars to near-black, and the new picture appears through a ragged front the seed decides |
| Dissolve | each pixel flips from old to new at its own moment |
| Scan wipe | a bright line sweeps down each 48-row band with the new picture behind it |
| Emboss flash | the new region appears as its edges alone, a bright relief, then the flat colour floods in behind them |

## The VNC menu

A **VNC** menu sits beside Effects on the menu bar (and takes every skin
through the same owner-drawn menu path as the rest). Its entries change
the **active desktop tab's copy** of its profile and take effect on the
next frame; the connection manager holds the defaults that a new tab
starts from.

| entry | what it sets |
|---|---|
| Particle Speed: Slow / Normal / Fast / Faster / Fastest / Custom | Motion speed 50 / 100 / 200 / 400 / 800 %, or any value 25–800 from a prompt |
| Shockwave Style: Ring / Water Drop / Splash / Vortex | the click's shockwave (below) |
| Redraw Style: None / Burn / Dissolve / Scan Wipe / Emboss Flash | how a changed region appears (above) |
| Shockwave on Click, Edge Glow, Heat on Change, Materialise on Connect | the four effect switches |
| Desktop Size: the server's own / fit this window / presets / custom | asks the server at once; fit then follows the window |
| Refresh Screen, View Only, Send Ctrl+Alt+Del, Send Clipboard to Server | as in the palette |

Shockwave styles, all driven by a button press inside the picture (a right
click reverses the amplitude):

The shockwave is a **refraction of the picture**, not a movement of
particles: every particle stays on its pixel and, for the half second the
wave lasts, reads its colour from a displaced source pixel
(`ShockRefraction` in `desktop_draw.hlsl`). The interface itself ripples
the way a surface under water does, with no gap and no drawn edge, because
nothing moves except where the colour is read from. Below solidity 100 the
swarm also feels a soft push from the click, so the two agree. Everything
is brief — gone in under half a second — because a pattern that lingers
reads as the ghosting of an old passive-matrix LCD. The heat effect's
energy decays on the same principle (a third gone in a tenth of a second).

* **Ring** — one refractive crest and trough travelling out at 1400 px/s.
* **Water drop** — concentric ripples running outward and dying fast.
* **Splash** — a bulge that leans upward.
* **Vortex** — a swirl that unwinds.

A desktop resize is reported on the status line for 1.5 s, not as a notice
that stays.

## Commands (palette and menu)

| command | what it does |
|---|---|
| VNC: Refresh Screen | a non-incremental FramebufferUpdateRequest for the whole desktop |
| VNC: View Only (on/off) | flips the active tab's view-only, showing its current state; turning it on releases anything held |
| VNC: Send Ctrl+Alt+Del | Control_L, Alt_L, Delete pressed and released in order (the local keyboard cannot send it) |
| VNC: Send Clipboard to Server | the Windows clipboard as cut text, through the paste guard and the clipboard policy |

The tab is a `Session`, so tab close, drag, quake mode, workspaces and the
notice strip behave as for a terminal; splitting a VNC tab into panes is
refused (a desktop has no grid to split).

## Input (implemented)

Text comes from `WM_CHAR` as Unicode keysyms (Latin-1 as itself, otherwise
`0x01000000 + code point`), press and release together; Ctrl+letter sends
the letter with Control held. Keys that produce no text — navigation,
function, editing, modifiers — come from the virtual key (`Keysyms.cpp`),
are held, and are released with the keysym captured at press time.
`WM_KEYUP` exists for the desktop's sake. The application's Ctrl+Shift chords
and the Windows key stay with the application.

The pointer maps through the placement (native or letterboxed) into
framebuffer coordinates, clamped to the edge while a button is held; the
wheel is a press and release of buttons 4/5 per notch; motion is coalesced
by the worker so a slow link carries the latest position, not a backlog.

Focus loss (`WM_KILLFOCUS`), disconnect, and turning view-only on release
every held keysym and button, so a stuck Shift on the far side cannot
happen.

Clipboard, both directions, under the profile's policy (the same five
modes as `x11Clipboard`), text only, Latin-1 on the wire as the core
protocol specifies:

* **Remote → local**: ServerCutText lands on the Windows clipboard when the
  policy allows it (or the "ask" dialog says yes); it is never typed into
  anything. The text is remembered so it is not sent back as an echo.
* **Local → remote**: nothing watches the Windows clipboard. **Ctrl+V** on
  the desktop, or the palette's *Send Clipboard to Server*, reads it and
  runs the ordinary paste guard first (the same preview and the same
  broadcast confirmation a terminal paste gets); what passes goes out as
  ClientCutText under the policy, is capped at the protocol's 1 MiB, and is
  remembered so the server's echo of it is ignored. Ctrl+V then presses V
  with Control already held, so the far side pastes what it just received.
  Text that came *from* the server is not sent back to it.

There is no feedback loop by construction: the only outbound trigger is an
explicit user action, and both directions carry an echo guard.

## Verification

### Automated tests

`AmberTests.exe "[vnc]"` — 71 cases, 887 assertions as of Phase 3 (the
whole suite: 455 cases, 81 689 assertions, all passing):

* VeNCrypt (`tests/VncTlsTests.cpp`, `[tls]`): the subtype policy as a
  pure function; the client's steps byte for byte through the fake server
  for X509Vnc (parked at TLS, resumed with the challenge inside it),
  X509None, X509Plain allowed and refused, a server without VeNCrypt, a
  3.3 server, anonymous-only subtypes, version and subtype refusals, and
  the handshake one byte at a time; then OpenSSL for real on loopback
  against an in-process OpenSSL server with a certificate generated in the
  test — fingerprint, verdict, subject, protocol, bytes both ways, and a
  plaintext server as a clean failure; and finally the whole worker
  through a VeNCrypt X509Vnc server: the certificate question, acceptance
  and the pin written, a second connection pinned with no question, a
  third with a different certificate raising the alarm, refused, not
  retried and not pinned, plus a TLS-required profile against a
  plaintext-only server: refused once. The pin store is redirected to a
  scratch file for the test; the user's is never touched.

* DES against the published FIPS 46 / SP 800-17 known-answer vectors and
  the parity-bit property; the VNC key by its bit-reversal definition; the
  empty password as the zero-key known answer.
* Codecs with hand-assembled fixtures whose construction is in the test
  (the reference implementation ships no byte fixtures, and Hextile is not
  in it at all — that one is from RFC 6143 §7.7.4).
* Decoders: Raw (alpha forced), CopyRect overlap in both directions,
  Hextile subrects / coloured subrects / raw tiles / a tile arriving in
  pieces / an out-of-tile subrect refused, ZRLE all subencodings across
  rectangles on one stream and every unused code refused, Cursor mask
  alpha, dirty-region collapse.
* The client through every dialect, refusal, wrong password (3.8 with the
  reason, 3.7 without), no common type, unsupported version, all four
  encodings in one update, the pipelined request, DesktopSize, cursor, cut
  text, bell, colour map, ContinuousUpdates, and the whole session one byte
  at a time.
* The worker against a real loopback server: first picture, input crossing
  the socket, view-only, a drop with reconnect and a fresh picture, a
  rejected password with no retry, bounded retries against a dead port,
  the tunnel path.
* `ShaderContractTests`: `DesktopCB`'s scalar count matches its HLSL mirror;
  every motion style has a field.

### `--vnc-selfcheck`

```
build\Release\AmberSSH.exe --vnc-selfcheck      # report: %TEMP%\vnc-selfcheck.txt, exit 0/1
build\Debug\AmberSSH.exe   --vnc-selfcheck      # same, with the D3D12 debug layer's messages
```

An RFB server runs in-process (`SelfCheckServer`, loopback, RFB 3.8 / None)
serving a known 320×200 picture. A VNC tab at solidity 1, density 1, size 1
draws it. The check reads the **scene target** back — before bloom and
composite, in linear light — and compares every pixel with the decoded
colour after the sRGB curve, to within one 8-bit step; then it changes one
block (the picture must follow, nothing else may move, the energy texture
must be non-zero inside the block and zero outside, a key and a pointer
event must reach the server); resizes the desktop to 400×260 (buffers
rebuilt, the picture at the new size exact); runs 150 frames of a block
dragged across the screen at one update per request (when they stop, the
picture must be exact).

**Result, 2026-09-04, Release, NVIDIA GeForce RTX 2050, window 1924×1093:**
every phase `ok`; 64 000 and 104 000 pixels compared with **max deviation
0** in all four comparisons; energy 0.709 inside the changed block, 0.0000
outside; 728 sustained updates; one connection. **Debug build with the
debug layer and GPU-based validation: 0 error-severity messages.** The one
warning (`#820`, one per frame) is the scene target's pre-existing
clear-value mismatch, not part of this work.

What it does **not** check, deliberately: the composite stage (the scene is
compared, not the swap chain), scaled placement, and anything a real server
does differently from this one.

### `--vnc-bench`

```
build\Release\AmberSSH.exe --vnc-bench          # report: %TEMP%\vnc-bench.txt
```

The same harness serving **3840×2160**, density 1 — **8 294 400 particles**
— measured over three loads of 240 frames each, vsync off: a static
desktop; a 400×300 block dragged across the screen at one update per
request; the whole frame recoloured every request ("video"). Frame times
are the application's own frame clock; GPU times are timestamp queries;
decode is the worker's CPU time per wall second.

**Result, 2026-09-04, NVIDIA GeForce RTX 2050 (3962 MiB), window
1924×1093 so the 4K framebuffer is scaled 0.486 to fit:**

| load | avg | fps | min | max | gpu frame | upload | sim | draw | decode | net |
|---|---|---|---|---|---|---|---|---|---|---|
| static desktop | 15.64 ms | 63.9 | 14.88 | 16.11 | 15.60 | 0.00 | 3.90 | 11.21 | 7.0 ms/s | 0 |
| dragging a 400×300 block | 16.41 ms | 60.9 | 7.80 | 28.39 | 16.36 | 0.80 | 3.86 | 11.21 | 88.7 ms/s | 268 MiB/s |
| video: whole 4K frame every request | 18.23 ms | 54.9 | 0.35 | 70.23 | 15.36 | 0.54 | 3.10 | 11.21 | 117.7 ms/s | 289 MiB/s |

**The 120 fps target at 4K is not met on this hardware.** The bottleneck is
the draw pass: 11.2 ms for 8.3 million instanced 1-px quads (with the
framebuffer scaled to 0.486 they are sub-pixel, which costs rasterisation
without saving fill). The simulation is 3.9 ms; uploads are under 1 ms even
at 289 MiB/s of full-frame video; the worker's decode reaches 118 ms/s of
CPU on one core for Raw video, well under saturation. A 120 fps budget is
8.3 ms for the whole frame; the draw alone needs to fall by a factor of
about three. The obvious levers, none of them taken yet: draw faithful
pixels at solidity 1 as a single textured quad (the particles are all at
home, so the result is identical) and only draw particles that are off
home; a point-list topology instead of six-vertex quads; and a budget that
scales density down on a measured frame-time overrun. The numbers above are
what shipped; the levers are recorded so the next step is honest about what
it changes.

Measured, not assumed: the input-to-picture figure on the overlay is
labelled *approximate* — it is the time from the last input sent to the
next completed framebuffer update, which measures a round trip only on a
server that reacts to the input.

### Interoperability

**Unverified.** No TigerVNC server and no `vncfree-server.exe` were
available on the build machine (and vncfree's server needs a Rust
toolchain, which is out of scope). Every real-server check — TigerVNC's
3.8, ZRLE and Hextile, cursor shapes, DesktopSize on a resolution change —
remains to be run. To run it against the Fedora guest used for AmberX:

```
sudo dnf install -y tigervnc-server && vncpasswd && vncserver :1 -localhost no
```

then connect to `192.168.0.18:5901` directly and through the SSH session.

The same applies to every Phase 3 behaviour that needs a real far side:
input through a real server, the server cursor shape, DesktopSize on a
real resolution change, the tunnel through a real SSH session and what
happens when that session closes, clipboard against a real server, and
VeNCrypt against a real X.509-capable server (TigerVNC's `-SecurityTypes
X509Vnc`). Each is implemented and exercised only by the in-process
fakes described above.

## Status: implemented vs verified

| behaviour | implemented | verified by |
|---|---|---|
| RFB 3.3/3.7/3.8, None, VNC Auth | yes | fake-server client tests, loopback session tests, self-check |
| Raw, CopyRect, ZRLE, Hextile, DesktopSize, Cursor, ContinuousUpdates | yes | fixture and client tests, self-check (Raw/ZRLE/Hextile served) |
| VeNCrypt X509None/X509Vnc/X509Plain, pin, alarm, no downgrade | yes | `[tls]` tests incl. OpenSSL loopback; **no real server** |
| direct TCP, reconnect with backoff, no retry of a refusal | yes | session tests |
| SSH tunnel via `direct-tcpip` forward | yes | session test of the tunnel path; **no real SSH session** |
| SSH session closing takes its VNC tabs down | yes | by inspection only |
| particle desktop, faithful contract, disturbance, resize, sustained load | yes | `--vnc-selfcheck` (passes on this build) |
| 120 fps at 4K | measured, **not met** on RTX 2050 | `--vnc-bench` (Phase 2 numbers above) |
| keyboard, pointer, wheel, release on focus loss | yes | session tests for the wire; self-check for one key and one pointer event |
| clipboard both ways with policy and echo guards | yes | by inspection only (no test drives the Windows clipboard) |
| connection dialog page, palette commands, view-only toggle, Ctrl+Alt+Del | yes | built and launched; by inspection only |
| Tight encoding | **no** | — |
| interoperability with TigerVNC / vncfree-server | — | **observed, not instrumented**: on 2026-09-05 the user connected to a Fedora guest running TigerVNC with an XFCE session and showed the desktop drawn (a screenshot: Thunar, panel, wallpaper, scaled below native). Which encodings the server chose, DesktopSize, cursor shapes and VeNCrypt against it are still unrecorded; vncfree-server remains untried |

## Known limitations

* No Tight encoding; servers that offer only Tight and Raw fall back to Raw.
* VeNCrypt's anonymous TLS and plaintext subtypes are refused by policy;
  a server that offers only those cannot be used with TLS required, and
  the message says which subtypes it offered.
* A CA-verified certificate supersedes an older self-signed pin silently
  (the CA's word is taken over the pin); only an unverifiable certificate
  that differs from the pin raises the alarm.
* When the SSH session a VNC tab tunnels through closes or errors, the VNC
  tab is disconnected and says why; it is not reconnected when Guardian
  brings the SSH session back — reopen it.
* The view-only toggle in the palette changes the tab, not the saved
  profile.
* Cut text is Latin-1 only, as the core protocol specifies.
* The 120 fps target at 4K is not met on the test hardware (above).
* Faithful means the scene is exact; the composite's selected effects still
  apply, and only native scale is verified pixel for pixel.
* Sampled fallback (framebuffers past the budget at density 1) is
  implemented and reported but has not been exercised by the self-check.
* Interoperability with real servers is unverified (above).
