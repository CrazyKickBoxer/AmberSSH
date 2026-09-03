# Third-party notices

AmberSSH links or derives from the following components.

## libssh2
SSH2 protocol implementation. BSD-3-Clause.

## OpenSSL
Cryptography backing libssh2. Apache-2.0.

## nlohmann/json
JSON parsing for profile and settings storage. MIT.

## Catch2
Unit-test framework (test binary only, not shipped in the runtime package). BSL-1.0.

## Dear ImGui
Debug overlay. MIT.
*Being phased out: the primary user interface is native Win32.*

## DirectX Shader Compiler (DXC)
Compiles the HLSL shaders at build time. LLVM Release License / MIT.

## amber-particle-ssh — design reference
<https://github.com/CrazyKickBoxer/amber-particle-ssh> (MIT).

The four-layer particle model, dual-sine brightness animation, pointer force
field, shockwave impulse, and the spring/drag/timestep-clamp constants in
`shaders/particle_sim.hlsl` follow that project's design. The referenced project
is GLSL/OpenGL under Qt6; AmberSSH's implementation is independent HLSL for
DirectX 12 written against its own particle and cell layout. No source was
copied.

## Fonts
Eleven open-source faces are redistributed in `exe\fonts` and loaded into a
private DirectWrite collection: JetBrains Mono, Fira Code, Hack, IBM Plex Mono,
Source Code Pro, Ubuntu Mono, Space Mono, Share Tech Mono, Syne Mono, Orbitron
and Michroma.

They are under the SIL Open Font License 1.1, the Ubuntu Font Licence 1.0 and
the Hack Open Font License. Each licence requires that it accompany the font;
the terms and the attributions are in **`fonts/LICENSES.md`**, which is
deployed alongside them.

None of these licences reaches AmberSSH itself — the OFL covers the font files
and derivatives of them, not software that displays text with them.

Faces offered in the picker that are not in that directory are either already
installed on the system (Cascadia Mono, Consolas) or fetched on demand from
their upstream project when the user selects one. Nothing fetched that way is
redistributed by AmberSSH.

## X servers and RDP clients — deliberately not bundled

AmberSSH ships **no X server and no RDP client.** Remote display works by
detecting what the user has already installed and handing off to it:

| Component | Terms | How AmberSSH uses it |
|---|---|---|
| VcXsrv | GPLv2 | detected and launched as a separate program |
| X410 | commercial | detected by being already running |
| Xming | current releases are not free software | detected only |
| Cygwin/X | MIT server on a GPLv3 runtime | detected and launched separately |
| `mstsc.exe` | a Windows component | launched with a generated `.rdp` file |
| Weston, Xwayland | MIT | run on the user's own remote host |

Running a program is use, not distribution, and launching a separate process
that communicates over a documented protocol does not make it part of this
work. That is why the GPL on VcXsrv and the terms on current Xming do not
apply to AmberSSH. See `docs/REMOTE-DISPLAY.md`.

Should an RDP client ever be embedded rather than launched, it will be FreeRDP
(Apache-2.0), which is compatible with everything above.
