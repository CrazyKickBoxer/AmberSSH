# AmberSSH — Architecture

## Threads

**UI / render thread** owns the Win32 window, every DirectX 12 object, the
terminal presentation, input, and the frame loop. It never blocks on the
network.

**One SSH worker per session** owns its socket and libssh2 session. It talks to
the UI thread only through queues:

- UI → worker: outgoing bytes, resize requests, disconnect.
- Worker → UI: received bytes (`SpscRing`), status events, host-key prompts,
  errors (`SshEvent` deque under a mutex).

The worker uses libssh2 in non-blocking mode and waits on socket readiness
rather than spinning.

## Data flow

```
socket bytes
   -> SpscRing                       (worker -> UI, lock-free)
   -> VtParser::Feed                 (UTF-8 decode + escape state machine)
   -> Grid                           (cells, scrollback, alt screen, cursor)
   -> App::BuildVisualsFromGrid      (per-cell glyph id + brightness)
   -> ParticleRenderer::Update       (dirty-cell ranges only)
   -> compute dispatch               (particle physics, one dispatch)
   -> instanced draw                 (one DrawInstanced for all particles)
   -> bloom + composite              (FP16 scene target -> swap chain)
```

## Why the text model is separate from the particles

Particles are a *presentation* of the grid, never the source of truth. `Grid`
holds real `Cell` values, so selection, clipboard, scrollback and search operate
on text and stay correct no matter what the renderer is doing. This is what
makes copy/paste exact while the display is animated.

## Dirty-cell updates

`ParticleRenderer::Update` diffs the incoming `CellVisual` array against the
previous frame and uploads only changed *ranges* of `CellGpu` records. A cell
carries a `generation` and a `birth` timestamp; the compute shader uses those to
drive the glyph-change transition without any CPU per-particle work.

## Particle model

Each cell owns a fixed particle allocation. A particle's sub-index selects both
its glyph sample point and its **layer**:

| Layer | Role | Spread | Gain |
|---|---|---|---|
| 0 | diffuse background glow | widest | 0.30 |
| 1 | character body (carries legibility) | mid | 1.00 |
| 2 | sharp detail | tight | 0.85 |
| 3 | HDR sparkle (top ~18% only) | tightest | 1.60 |

Motion is a spring-damper toward the glyph home position plus curl-noise drift,
with optional style forces, a radial pointer force field, and a shockwave
impulse. The timestep is clamped so a paused debugger or a dragged window cannot
explode the integrator.

This four-layer arrangement, the dual-sine brightness animation, the force
field and the shockwave follow the design of
[amber-particle-ssh](https://github.com/CrazyKickBoxer/amber-particle-ssh)
(MIT). That project is Qt6 + OpenGL on Linux; this is an independent HLSL /
DirectX 12 implementation of the same ideas.

## CPU/GPU contract

`FrameCB` and `CellGpu` are mirrored between `src/common.h` and
`shaders/amber_common.hlsli`. Both are guarded by `static_assert` **and** by
`ShaderContractTests.cpp`, which parses the HLSL and compares its scalar count
to `sizeof()` on the C++ side. This exists because a forgotten mirror edit
produces silently wrong rendering rather than a compile error — it has already
caught one such bug (see the validation report).

## Modules

| Path | Responsibility |
|---|---|
| `src/dx/` | device, swap chain, frame contexts, shader loading |
| `src/render/` | particle system, bloom, composite, 2D primitives |
| `src/glyphs/` | DirectWrite rasterization, glyph atlas, particle sampling |
| `src/term/` | grid, VT parser, keyboard encoding |
| `src/ssh/` | libssh2 session and worker thread, ring buffer |
| `src/profiles/` | connection profiles, JSON store |
| `src/platform/` | paths, Credential Manager |
| `src/utility/` | secure strings |
| `src/ui/` | native Win32 connection dialog |
