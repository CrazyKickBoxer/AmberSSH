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
No font files are redistributed. AmberSSH uses fonts already installed on the
system, preferring JetBrains Mono, then Cascadia Mono, then Consolas.
