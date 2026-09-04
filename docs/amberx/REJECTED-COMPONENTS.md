# AmberX — rejected and excluded components

What was considered for AmberXCore and turned away, with the reason. Some of
these are licence rejections; most are scope exclusions — recorded anyway,
because the prompt's rule is that anything compiled or shipped must be on the
allowlist, and the clearest way to keep the allowlist honest is to say what
was looked at and left out.

| Component | Where | Reason | Kind |
|---|---|---|---|
| **VcXsrv** — source, patches, build files, resources, binaries | not fetched | The prompt's hard boundary. Never cloned, opened or diffed; nothing in this branch's history references it. | provenance rule |
| **Cygwin/X** (`hw/xwin` as built against Cygwin) | present in the clone, not built | The server-side files are MIT, but the prompt requires a file-by-file audit with legal sign-off before any use, and the Cygwin runtime they were written against is GPLv3. Not audited; not used. This is the single most consequential exclusion — see PHASE-0-AUDIT.md finding P0-3. | prompt rule; pending audit |
| `hw/xfree86`, `hw/xquartz`, `hw/xnest`, `hw/vfb`, `hw/kdrive`, `hw/xwayland` | present, not built | Other DDXes. AmberWinDDX replaces them. | scope |
| `glx/`, `glamor/`, `dri3/`, `hw/xfree86/dri*` | present, not built | GLX/DRI/OpenGL are explicitly a later phase. | scope |
| xorgproto `glxproto` (SGI Free Software License B) | present, not used | Only needed by `glx/`. Permissive and OSI-approved, but not on the initial allowlist and not needed. | scope (licence noted) |
| `Xext/shm.c` (MIT-SHM) | present, not built | The prompt forbids advertising MIT-SHM to forwarded clients without safe semantics. Not compiled, so it cannot be advertised by accident. | prompt rule |
| `miext/sync/misyncshm.c` (SHM fences) | present, skipped by the probe | The shared-memory half of the SYNC extension; needs `mmap`. With MIT-SHM off it has no caller. Skipped explicitly rather than shimmed with a pretend `mmap`. | prompt rule |
| `Xext/xv*.c`, `Xext/xvmc.c` (XVideo) | present, not built | Out of scope for the first release. | scope |
| `Xext/panoramiX*.c` (Xinerama) | present, not built | Multi-monitor is done through RANDR, not Xinerama. | scope |
| `Xext/dpms.c`, `Xext/saver.c`, `Xext/xres.c`, `Xext/xf86bigfont.c` | present, not built | Not needed for the milestone. | scope |
| `record/`, `dbe/`, `Xext/xselinux*` | present, not built | Not needed; XSELinux is Linux-only. | scope |
| `os/` platform files — `connection.c`, `xstrans.c`, `access.c`, `xdmcp.c`, `xdmauth.c`, `rpcauth.c`, `busfault.c`, `backtrace.c`, `inputthread.c`, `ospoll.c`, `xserver_poll.c`, `WaitFor.c` | present, not built in the probe | The POSIX layer: sockets, signals, poll, DMCP, credentials. This is what the Windows port *replaces*, and it is where the survey found the Unix headers concentrated. Which of `os/`'s pure-C files (`xprintf.c`, `log.c`, `utils.c`, the `strl*`/`strndup` shims) are retained is decided in Phase 2. | replaced by AmberWinDDX |
| `xkb/` keymap *compilation* (`xkbcomp` invocation) | present | The server shells out to `xkbcomp`; AmberX may not spawn processes (Job Object limit). Keymap delivery needs its own design. | needs design |
| **meson build system** | present, not used | AmberSSH builds with CMake; the probe drives `cl.exe` directly and Phase 2 will add a CMake target over the approved file list. meson itself is Apache-2.0 and would be fine to use; it is a toolchain choice, not a licence one. | toolchain |
| Anything from Stack Overflow, gists, forums | — | Provenance rule. Nothing of the kind was consulted or copied. | provenance rule |

## Not rejected, but not yet accepted

`os/` as a whole is neither approved nor rejected. The probe (PHASE-1-GATE.md)
compiled all 228 allowlisted files **without** `os/`, so the core does not
need it to compile — but it needs it to *link*: every `os_*` symbol, plus the
POSIX calls behind the six shim headers, is unresolved. Which of `os/`'s
files are pure C and can be retained (`xprintf.c`, `log.c`, `utils.c`, the
`strl*`/`strndup`/`reallocarray`/`timingsafe_memcmp` shims) versus replaced
by AmberWinDDX is a Phase 2 decision, made from the linker's unresolved list.
It stays "needs review" in `LICENSE-MATRIX.md` until then.
