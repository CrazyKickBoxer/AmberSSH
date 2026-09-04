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
| `os/` platform files — `connection.c`, `io.c`, `access.c`, `client.c`, `osinit.c`, `utils.c`, `log.c`, `auth.c`, `WaitFor.c`, `xstrans.c`, `xdmcp.c`, `xdmauth.c`, `rpcauth.c`, `busfault.c`, `backtrace.c`, `inputthread.c`, `ospoll.c`, `xserver_poll.c` | present, not built | The POSIX layer: sockets, signals, poll, XDMCP, credentials, an auth file, a host list. Replaced by `src/amberx/server/os_*.c` (Phase 2), which is smaller because there is nothing to listen on. `ReadRequestFromClient`'s framing and `WriteToClient`'s coalescing were carried over from `io.c` (MIT) with the socket calls replaced; that derivation is stated in the file. | replaced by AmberWinOS |
| `os/xsha1.c` | present, not built | Its CryptoAPI backend includes `<windows.h>` through `X11/Xwindows.h` inside an X translation unit, and X's `#define None` breaks `winnt.h`. `x_sha1_*` is implemented on the Windows side over BCrypt instead. | port rule (no `<windows.h>` in X units) |
| `os/strcasecmp.c` | present, not built | BSD `u_char`. Two one-line CRT mappings in `os_misc.c`. | trivial |
| `xkb/ddxLoad.c` | present, **excluded by the allowlist gate** | Compiles a keymap by spawning `xkbcomp`; the host cannot spawn (Job Object limit of one process) and must not. Replaced by `src/amberx/server/ddx_keymap.c`: an `.xkm` file read with the server's own `XkmReadFile`, or a built-in US map constructed directly in `XkbDesc` structures. `XkbCompileKeymapFromString` is refused. | policy |
| libXfont2 `FreeType/` | present, not built | Needs FreeType, a dependency and a further licence, for scalable fonts. The built-in bitmap fonts are all the core needs to start; scalable fonts are a later decision. | scope |
| libXfont2 `fc/` (font-server client) | present, not built | Sockets to a font server. AmberX opens no connections. | prompt rule |
| libXfont2 `fontfile/catalogue.c`, `util/realpath.c` | present, not built | Font catalogue directories via `readlink`/`realpath`. Not needed. | scope |
| libXfont2 `fontfile/bunzip2.c` | present, not built | bzip2-compressed font files; would add libbz2. gzip **is** built (`gunzip.c` + zlib) because the built-in fonts are stored gzipped. | scope |
| pixman SIMD (`pixman-mmx.c`, `-sse2.c`, `-ssse3.c`, and the ARM/MIPS/PPC/RISC-V files' fast paths) | present, dispatchers compiled without `USE_*` | Per-file `/arch` flags and a runtime CPU dispatcher: an optimisation to measure in a later phase. The portable C paths are the baseline. | performance, later |
| **meson build system** | present, not used | AmberSSH builds with CMake; the probe drives `cl.exe` directly and Phase 2 will add a CMake target over the approved file list. meson itself is Apache-2.0 and would be fine to use; it is a toolchain choice, not a licence one. | toolchain |
| Anything from Stack Overflow, gists, forums | — | Provenance rule. Nothing of the kind was consulted or copied. | provenance rule |

## The `os/` decision (Phase 2)

Made from the linker's unresolved list (PHASE-2-SPEC.md). **Retained,
unmodified**: `mitauth.c`, `oscolor.c`, `xprintf.c`, `strlcpy.c`,
`strlcat.c`, `strndup.c`, `reallocarray.c`, `timingsafe_memcmp.c` — pure C
with no platform in them. **Replaced** by `src/amberx/server/`: everything
that talks to sockets, signals, files, processes or credentials, listed in
the table above. The retained set is its own allowlist in
`third_party/amberx/CMakeLists.txt` (`AMBERX_OS_RETAINED`).
