# Stage 8 — X11 forwarding and Wayland RemoteApp

Implementation report. What was built, the licensing position it was built to,
what is proven by test, what was seen on screen, and — at length — what has
never been run against a real remote host.

## 1. The licensing decision, because it shaped the architecture

AmberSSH has no `LICENSE` file, which by default means all rights reserved, and
its dependency set is deliberately all-permissive: BSD-3, Apache-2.0, MIT,
BSL-1.0. No copyleft anywhere.

That ruled out the obvious implementation. Bundling VcXsrv (GPLv2) or building
against Cygwin's runtime (GPLv3 with an exception that only covers open-source
code) would put copyleft into a proprietary product. Current Xming is not free
software at all.

So the architecture is **detect and hand off, bundle nothing**:

| | how it works | why it is clean |
|---|---|---|
| X11 | detects an installed X server, launches it as a separate program | running a program is use, not distribution; two processes talking over a documented protocol are not one work |
| Wayland | tunnels to Weston's RDP backend on the user's own host, hands the tunnel to `mstsc.exe` | `mstsc` is a Windows component; Weston and Xwayland run on the user's machine, which is use |

Should an RDP client ever be embedded rather than launched, it will be FreeRDP
(Apache-2.0), which is compatible with everything already here. `THIRD-PARTY-
NOTICES.md` records all of this.

**A compliance gap was found and fixed on the way.** The notices file claimed
"no font files are redistributed", but eleven TTFs are shipped in `exe\fonts`.
They are OFL-1.1, the Ubuntu Font Licence and the Hack licence — all of which
permit redistribution and all of which require the licence to travel with the
font. `fonts/LICENSES.md` now does that and is deployed alongside them.

## 2. What was built

### `src/remote/XAuth.{h,cpp}` — untrusted X11 forwarding

The pre-existing X11 support passed `nullptr` for the authorisation protocol
and cookie. That works only against an X server with access control disabled,
and it means the remote host holds whatever credential does open the display.

This module implements what OpenSSH does. A **fake** cookie is generated per
session and given to the remote host; the **real** cookie, read from
`.Xauthority`, is substituted into each X11 connection as it is proxied. A
compromised remote host therefore never learns the credential that opens the
display — during the session or after it.

Every step is a pure function of bytes: display parsing, cookie generation,
`.Xauthority` parsing, X11 setup-packet parsing and rewriting. Design points
that are each a test:

* **A connection that fails the check is closed, never forwarded.** An unknown
  auth protocol, a wrong-length cookie, a cookie that does not match — all
  rejected. The dangerous reading of "no cookie" is "no check needed", and it
  is explicitly not taken: with no fake cookie, nothing authenticates.
* **A display number is rejected, not clamped.** `localhost:60000` would resolve
  to port 66000; treating it as display 0 would do exactly what an attacker
  intended.
* **Cookie comparison is constant-time**, and two empty cookies do not match.
* **A truncated `.Xauthority` keeps what it could read.** Half-written files are
  normal when an X server is starting and must not take the session down.
* **With no local cookie the fake is still verified** and the packet forwarded
  unchanged for the X server's own access control to judge. Weaker, reported to
  the user, and still better than the alternative — many Windows X servers are
  run with access control off and refusing outright would break them.

### `src/remote/RemoteDisplay.{h,cpp}` — detection and the RemoteApp plan

Which X server is installed, which is running, what to launch it with, what
port to forward, what to run on the far end, what to hand the client.

The rule the tests are really about: **neither end ever listens on anything but
loopback.** The forward spec is written with an explicit `L127.0.0.1:` bind
address — the syntax permits omitting it, and omitting it is how a tunnel ends
up reachable from the whole LAN. The remote command always carries
`--address=127.0.0.1`, because a VPS has a public IP by definition and RDP on a
public IP is among the most scanned surfaces on the internet.

Launch arguments deliberately never include `-ac`, which would disable access
control for every process on the machine and discard the entire point of the
cookie work above.

### `SshSession::AddForward`

A local forward can now be raised on a running session, so RemoteApp does not
require a reconnect. It only ever adds — nothing here can remove a forward the
user configured — and the listener still binds loopback.

### UI

File → Remote Display: find X servers, start one, RemoteApp (start Weston),
RemoteApp (tunnel only), and the setup guide. All five in the command palette.
The About box now carries attribution, since most of what AmberSSH links
requires it and that is where a user looks.

### `docs/REMOTE-DISPLAY.md`

The VPS-side runbook: Weston with the RDP backend, the TLS certificate, the
`weston.ini`, the systemd user service, the binding rule with the `ss -ltnp`
check to confirm it, and how to verify RAIL before designing around it.

## 3. What is proven by test

348 test cases and 67,599 assertions, up from 322 / 47,067. The two new files
contribute 26 cases.

**XAuth** — display parsing including the refusals; 200 generated cookies, none
repeating; hex conversion all-or-nothing; constant-time comparison rejecting
the empty cookie; `.Xauthority` parsing, per-display matching, and *every*
truncation of a two-entry file; setup-header parsing in both byte orders and at
every length below 12; the real cookie replacing the fake with the fake proven
absent from the output; five distinct rejection cases; every prefix of a setup
packet returning NeedMore rather than deciding on incomplete input; trailing
pipelined request bytes riding along rather than being dropped.

**RemoteDisplay** — a running server outranking one on disk; stable ranking; one
entry per server however many install roots matched; something listening with
nothing installed still reported (the only way X410 can be found); every server
carrying a name and its terms; launch arguments proven not to contain `-ac`;
the plan proven to bind loopback on both ends; the client proven to be pointed
at the tunnel and never at the host; bad ports refused with a reason and
producing no client arguments at all.

## 4. What was seen on screen

* **X server detection ran live** in a ConPTY session and correctly reported
  "No X server found. Install one (VcXsrv, X410, Cygwin/X) — AmberSSH does not
  bundle one", which is true on this machine.
* The About box was captured with its credits on Classic and LCARS; the panel
  was resized twice to stop the credits overlapping the copyright line.
* `fonts/LICENSES.md` was confirmed to deploy to `build/Release/fonts/`
  alongside the TTFs.

## 5. What is NOT proven

* **There is no SSH server and no Linux host on this machine, so no X11 channel
  has ever been forwarded and no RDP tunnel has ever carried a frame.** The
  cookie substitution is proven against synthetic setup packets, not against a
  real `xterm`. The RemoteApp path is proven to construct the right tunnel,
  command and client invocation; whether Weston accepts them has not been
  observed.
* **No X server is installed here**, so detection is proven only in its negative
  case and by unit tests over synthetic candidate lists. The ranking, the launch
  arguments and the `ShellExecuteW` call have never launched anything.
* **RAIL is unverified.** Whether upstream Weston's RDP backend gives seamless
  per-application windows rather than a desktop in a rectangle is the open
  question, and the runbook says to settle it with `mstsc.exe` before building
  further. Nothing in this stage depends on the answer, but the feature's value
  does.
* **`AddForward` has never raised a tunnel on a live session.** The code path is
  small and mirrors the existing startup path, and it has not run.
* **The `.Xauthority` search is untested against a real file.** `LoadXAuthority
  File` checks `%XAUTHORITY%`, then `%HOME%`, then `%USERPROFILE%`; which of
  those a given Windows X server actually writes has not been observed, only
  read about.
* The remote command is sent as a shell line into the session, so it inherits
  whatever shell the user has. It is deliberately visible in the scrollback
  rather than executed invisibly, but it has not been tested against a shell
  that mangles it.
* Nothing here has been tested at a DPI other than this machine's.
