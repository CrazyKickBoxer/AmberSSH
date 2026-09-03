# Remote display: X11 forwarding and Wayland RemoteApp

How to run graphical Linux programs from a remote host inside AmberSSH, and the
licensing position that makes each route safe to ship.

Two routes, for two different situations.

| | **X11 forwarding** | **Wayland RemoteApp** |
|---|---|---|
| Remote runs | anything X11 | Weston (headless, RDP backend) + Xwayland |
| Local needs | an X server you install | an RDP client |
| Transport | the SSH connection's own `x11` channels | RDP over a forwarded TCP port |
| Latency | poor — X11 is chatty and round-trip bound | good — RDP is designed for a WAN |
| Modern toolkits | works, but GTK/Qt over X11 across a WAN is painful | native |
| AmberSSH ships | nothing | nothing (or FreeRDP later) |

X11 forwarding is the compatible route: it works against any host with no
server-side setup. Wayland RemoteApp is the *good* route: it needs the remote
host prepared once, and after that it is the difference between usable and not.

---

## Licensing, stated once

**Running software on your own host is use, not distribution.** No licence in
this area restricts that. Everything in the "remote host" sections below can be
whatever licence it likes, because you are not conveying it to anyone.

That changes only if you later ship a setup script, container image or install
helper that *bundles* these components. Weston and Xwayland are MIT, so even
that stays clean; a GPL alternative such as `xrdp` would not.

**AmberSSH itself bundles no X server and no RDP client.** It detects what is
already installed and hands off to it. That is deliberate: it means the GPL on
VcXsrv and the non-free terms on current Xming never reach this codebase,
because nothing is redistributed. If an embedded RDP client is added later it
will be FreeRDP, which is Apache-2.0 and compatible with everything already
here.

This is an engineering summary, not legal advice. The decision to ship anything
bundled needs a lawyer.

---

## Route 1 — X11 forwarding

### Local: install an X server

AmberSSH does not ship one. Any of these work; it detects them and tells you
which it found.

| Server | Licence | Notes |
|---|---|---|
| **VcXsrv** | GPLv2 | The usual choice. Free, actively used, installs to `%ProgramFiles%\VcXsrv`. |
| **X410** | commercial | Microsoft Store. Best integration, costs money. |
| **GWSL** | — | Microsoft Store; a VcXsrv wrapper. |
| **Cygwin/X** | MIT server, GPLv3 runtime | Fine to install and use; only bundling would be a problem. |
| **MobaXterm** | freemium | Ships its own X server. |

Start it with access control **on**. Do not use `-ac`: that disables access
control entirely and lets anything on your machine connect to your display.
AmberSSH performs cookie authentication, so you do not need `-ac`.

VcXsrv, one display, no access control disabled:

```
"C:\Program Files\VcXsrv\vcxsrv.exe" -multiwindow -clipboard -wgl -displayfd 0
```

### Remote: nothing

X11 forwarding needs no server-side setup beyond `X11Forwarding yes` in
`/etc/ssh/sshd_config`, which is the default on most distributions. Check it:

```
sudo sshd -T | grep -i x11forwarding
```

### In AmberSSH

Connection profile → SSH → X11: tick **Enable X11 forwarding** and leave the
display as `localhost:0` unless you started your X server on another display.

AmberSSH generates a random MIT-MAGIC-COOKIE-1 per session, gives *that* cookie
to the remote host, and substitutes your real local cookie as each X11 channel
opens. The remote host never learns the cookie that actually authorises access
to your display — so a compromised remote host cannot reach your X server after
the session ends, and cannot reach it at all outside the channels SSH opened.

---

## Route 2 — Wayland RemoteApp over RDP

The remote host runs a headless Wayland compositor that speaks RDP. Xwayland
runs inside it so X11 programs work too. You reach it over a forwarded port.

This is the same architecture WSLg uses.

### Remote host, one-time setup

Debian/Ubuntu:

```
sudo apt install weston xwayland freerdp2-x11 openssl
```

Fedora/RHEL:

```
sudo dnf install weston weston-remoting xorg-x11-server-Xwayland freerdp
```

Weston's RDP backend needs a TLS certificate. A self-signed one is correct
here: the connection never leaves the loopback interface on either machine —
SSH is what actually authenticates and encrypts the hop between them.

```
mkdir -p ~/.config/weston
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout ~/.config/weston/tls.key \
  -out ~/.config/weston/tls.crt \
  -subj "/CN=localhost"
chmod 600 ~/.config/weston/tls.key
```

`~/.config/weston.ini`:

```
[core]
backend=rdp-backend.so
shell=kiosk-shell.so
xwayland=true

[rdp]
refresh-rate=60
tls-cert=/home/YOU/.config/weston/tls.crt
tls-key=/home/YOU/.config/weston/tls.key
```

`kiosk-shell` gives one application per surface, which is what you want for
RemoteApp. Use `desktop-shell` instead if you want a full desktop in a window.

### The binding rule, which matters more than anything else here

Weston's RDP backend **must** listen on the loopback interface only:

```
weston --backend=rdp-backend.so --address=127.0.0.1 --port=3389
```

`--address=127.0.0.1` is not optional. RDP exposed on a public IP is among the
most heavily scanned surfaces on the internet, and a VPS has a public IP by
definition. The only path in should be your SSH tunnel.

Confirm it after starting:

```
ss -ltnp | grep 3389
```

You want `127.0.0.1:3389`. If you see `0.0.0.0:3389` or `*:3389`, stop and fix
it before connecting from anywhere.

Belt and braces, if the VPS has a firewall:

```
sudo ufw deny 3389/tcp
```

### Starting it

Foreground, for the first test:

```
weston --backend=rdp-backend.so --address=127.0.0.1 --port=3389 \
       --shell=kiosk-shell.so --xwayland
```

As a user service, once it works — `~/.config/systemd/user/weston-rdp.service`:

```
[Unit]
Description=Headless Weston for RemoteApp over SSH

[Service]
ExecStart=/usr/bin/weston --backend=rdp-backend.so --address=127.0.0.1 \
          --port=3389 --shell=kiosk-shell.so --xwayland
Restart=on-failure
Environment=XDG_RUNTIME_DIR=%t

[Install]
WantedBy=default.target
```

```
systemctl --user daemon-reload
systemctl --user enable --now weston-rdp
loginctl enable-linger $USER      # survives logout
```

### Local: the tunnel and the client

AmberSSH sets up the forward for you (Remote Display → Wayland RemoteApp), but
the equivalent by hand is:

```
ssh -L 127.0.0.1:13389:127.0.0.1:3389 you@your-vps
```

Then connect an RDP client to `127.0.0.1:13389`. `mstsc.exe` ships with Windows
and needs no licensing consideration; connecting a Windows client to a
non-Microsoft RDP server does not involve RDS CALs, which apply only to clients
connecting *to* Windows Server's Remote Desktop Session Host role.

### Launching an application

Inside the Weston session:

```
WAYLAND_DISPLAY=wayland-0 gedit          # native Wayland
DISPLAY=:0 xterm                         # X11, via Xwayland
```

---

## Verify RAIL before building on it

**RAIL** (Remote Application Integrated Locally) is the RDP feature that gives
each remote program its own window on your desktop, rather than one rectangle
containing a whole Linux desktop. It is the difference between "RemoteApp" as a
feature and "a desktop in a box".

Microsoft added RAIL support to Weston for WSLg and some of that was upstreamed,
but whether the Weston in your VPS's repositories has working RAIL is not
something to assume. Check it before designing around it:

1. Start Weston with `kiosk-shell` as above.
2. Tunnel and connect with `mstsc.exe`.
3. Launch a program inside the session.

If it appears as its own window on your Windows desktop, RAIL works. If it
appears inside a rectangle that also contains a Weston background, it does not,
and you have a desktop-in-a-window — still useful, but a different feature.

An afternoon spent here saves redesigning later.

---

## Security summary

| Rule | Why |
|---|---|
| Weston binds `127.0.0.1` only | a VPS has a public IP; RDP on one is scanned constantly |
| Local forward binds `127.0.0.1` only | otherwise everyone on your LAN reaches your VPS's display |
| Never start the X server with `-ac` | it disables access control for every process on your machine |
| The remote never sees your real X cookie | AmberSSH sends a per-session fake and substitutes the real one locally |
| Self-signed TLS on the RDP backend is fine | the hop is loopback-to-loopback; SSH is the real transport security |

Never expose either port beyond loopback "just to test". That is how it stays
exposed.
