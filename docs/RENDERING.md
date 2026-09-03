# AmberSSH — Rendering

## Pipeline per frame

```
compute dispatch        particle physics for every particle (one dispatch)
instanced draw          one DrawInstanced for all terminal particles
prims draw              cell fills, underlines, tab bar, status text
bloom                   prefilter -> downsample chain -> upsample chain
composite               tonemap + scanlines/aberration/vignette -> swap chain
```

Terminal glyph particles cost **one draw call** regardless of grid size. There
is no per-cell, per-glyph or per-row draw.

## Glyph rasterization and particle sampling

DirectWrite rasterizes each glyph into a coverage mask
(`IDWriteGlyphRunAnalysis` → `CreateAlphaTexture`). From that mask the sampler
picks 32 well-spread points, weighted by coverage and chosen deterministically,
so a given glyph always produces the same constellation instead of reshuffling
every frame. Glyph 0 is the blank glyph and parks its particles at the cell
centre with zero brightness.

The atlas is not the visual output — it exists only to derive particle homes,
and separately to draw crisp UI text for the tab bar and status line.

## Four-layer particle model

Each particle's sub-index selects both a sample point and a layer:

| Layer | Role | Spread | Gain |
|---|---|---|---|
| 0 | diffuse background glow | widest | 0.30 |
| 1 | character body — carries legibility | mid | 1.00 |
| 2 | sharp detail | tight | 0.85 |
| 3 | HDR sparkle, top ~18% of particles only | tightest | 1.60 |

Densities above the 32-point template stride reuse points on different layers
with different spreads, which is the documented behaviour for glyphs with fewer
coverage samples than the requested particle count.

## Simulation

Semi-implicit integration toward the glyph home:

```
k      = max(springK, 1) * 4
accel  = k * (target - pos) - damping * vel + styleAccel
h      = min(dt, 0.05)          // clamp: a paused debugger must not explode it
vel   += accel * h
pos   += vel * h
```

`target` is the glyph home plus divergence-free curl-noise drift (~1.5 px), so
the text breathes without swirling. Brightness combines a slow pulse, a faster
low-amplitude flicker, per-particle phase offsets, and a birth fade-in.

Additional forces: style modes (twist / rain / quantum / magnetic), a radial
pointer force with smooth falloff, and an expanding shockwave ring. None of
them displace particles permanently — the spring always wins.

Glyph changes are driven by a per-cell `birth` timestamp and generation
counter: the GPU eases particles from the previous glyph's point set to the new
one over `transitionDur` (default 250 ms), with no CPU per-particle work.

## Selection

Selected cells set `kCellFlagSelected`. The simulation converts that into a
per-particle `tint` running across the selected region, and the pixel shader
swaps the amber ramp for the Miami Sunset ramp. Both ramps are evaluated in
linear light.

## Dirty-cell uploads

`ParticleRenderer::Update` diffs incoming cell visuals against a shadow copy
and uploads only changed *ranges* — up to 64 ranges per frame, after which a
single full upload is cheaper and is used instead. The whole terminal is never
uploaded every frame.

## Targets and synchronisation

Particles render into an `R16G16B16A16_FLOAT` scene target so bloom and the hot
glyph cores have headroom above SDR white; the swap chain itself may be SDR.
Presentation is flip-model (`FLIP_DISCARD`) with tearing where the system
allows it. Three frames in flight, each with its own allocator and upload ring;
fences are taken only where resource reuse requires them, never as a
whole-GPU wait in the update path.

## CPU/GPU contract

`FrameCB` (160 bytes) and `CellGpu` (24 bytes) are mirrored between
`src/common.h` and `shaders/amber_common.hlsli`, guarded by `static_assert` and
by `ShaderContractTests.cpp`, which parses the HLSL and compares its scalar
count to the C++ `sizeof`. This caught a real 152-vs-160 byte mismatch during
development.

## Not measured

No frame-timing capture has been taken on this machine. The 4K/120 FPS figure
in the specification is a target, not a measurement.
