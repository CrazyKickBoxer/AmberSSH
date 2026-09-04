# AmberX — what it does not do

Shipped in the release bundle, because a user deciding whether to rely on
this deserves the list before they find it out one application at a time.

AmberX is the X server built into AmberSSH. One process per SSH session, no
external X server, no separate installer. What follows is the boundary of it.

## Not implemented

| | |
|---|---|
| **GLX, OpenGL, DRI, DRI3, Vulkan** | No 3D of any kind. `glxgears` and anything else that needs a GL visual will fail to start, and the error will say so. AmberX is a 2D server. |
| **Audio** | No audio or microphone forwarding. A remote application that plays a sound plays it on the remote machine. |
| **Tabs and panes** | Forwarded windows are native Windows windows. The profile offers tab and pane modes; they are not built, they fall back to native windows, and the session log says so. |
| **Clipboard formats beyond text** | UTF-8 text only, and off by default. No images, no file lists, no HTML or RTF: each needs its own threat analysis and its own bounded parser. |
| **Wayland-native forwarding** | X11 only. Remote Wayland applications need XWayland on the remote side. |
| **Screen resolution changes by a client** | Windows owns the monitors. Every mode-setting request from every client is refused. |
| **`xkbcomp`** | No compiler is run at any time. The keyboard map is built from the live Windows layout, or read from a pre-compiled `.xkm` file if one is configured. |

## Limits you can run into

Defaults, in `src/amberx/server/amberlimits.h`, all per session:

| | |
|---|---|
| forwarded clients | 64 |
| windows | 4,096 per client, 16,384 in total |
| pixmaps | 8,192 pixels a side, 64 MiB each, 512 MiB in total |
| one property | 1 MiB |
| one request | 4 MiB, BIG-REQUESTS included |
| new atoms | 2,000 per client per 10 seconds |
| one clipboard transfer | 1 MiB, and 5 seconds |
| the host process | 1 GiB of memory, 50 % of one CPU, hard capped |

Hitting a limit gets the client a `BadAlloc` — the error it already knows how
to handle. It never ends the session and it can never reach AmberSSH, which
is a different process.

## Security, in short

The long version is `docs/amberx/THREAT-MODEL.md`. The parts that change how
you should use it:

- **Restricted is the default.** The session's cookie is registered with the
  X SECURITY extension as untrusted, and that is enforced: a forwarded client
  cannot write to the root window or to anything the server owns.
- **Trusted mode is real trust.** Every forwarded program can read every
  other forwarded program's input and windows. It exists, it needs an
  explicit opt-in with the hostname typed back, and every window carries an
  `X11 TRUSTED` strip while it is on.
- **Forwarded clients are not isolated from each other**, in either mode.
  That is X11's own model rather than an AmberX shortcut, and it is the
  single most important thing on this page: do not forward an application you
  distrust into a session alongside one you do.
- **The host runs with a restricted token at Low integrity**, with process
  mitigations and inside a Job Object that ends it when AmberSSH ends. It
  cannot reach your files, your other windows, or the clipboard.
- **The display has no network listener.** It is reachable only through the
  SSH session that owns it. There is no port to firewall.
- **Nothing is logged that should not be.** No cookies, no clipboard
  contents, no window contents, no X11 bytes. `%TEMP%\amberx-host.log` holds
  the server's own messages and counts.

## Not VcXsrv, not Cygwin/X, not Xming, not X410

AmberX shares no code with any of them and is not endorsed by any of them. It
is built from upstream X.Org sources — xserver, xorgproto, pixman, libXfont2,
libxkbfile — with an original Windows layer written for AmberSSH. Exactly
what was fetched, from where, and with which hashes is in
`docs/amberx/SOURCE-PROVENANCE.md`; every component and its licence is in
`docs/amberx/LICENSE-MATRIX.md`; the machine-readable inventory is
`amberx.spdx.json`.

## What has actually been tested

`docs/amberx/COMPATIBILITY.md` — and at the time this bundle was built, the
answer was **no real application yet**. AmberX has been driven by a protocol
test client, thoroughly, and by no toolkit. Treat it as what it is.
