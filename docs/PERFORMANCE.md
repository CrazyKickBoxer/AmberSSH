# AmberSSH — Performance

## Target

3840×2160 at 120 FPS, with the particle simulation inside roughly 2–3 ms of GPU
time. This is an engineering target, not a guarantee on every GPU.

## Measured

**Nothing has been measured yet.** Frame timing requires running the GUI
against a live SSH session and reading the F3 overlay; no SSH server was
available in the build environment. The following table is deliberately empty
rather than filled with plausible-looking numbers:

| Metric | Value |
|---|---|
| GPU | not measured |
| Resolution | not measured |
| Particle count | not measured |
| Average frame time | not measured |
| 95th / 99th percentile | not measured |
| Compute time | not measured |
| Particle draw time | not measured |
| Bloom + composite time | not measured |

The renderer is instrumented — `F3` shows FPS, frame time, particle count,
dirty cells per frame, grid size, and the active font — so these can be filled
in as soon as a session can be opened.

## Design budget

At 80×25 with 32 particles per cell the system simulates 64,000 particles. The
grid auto-shrinks its row count if a window size would push past ~320,000
particles, so an oversized window degrades gracefully instead of stalling.

## Optimisation order

1. Input responsiveness
2. SSH throughput
3. Terminal correctness
4. Text clarity
5. Post-processing quality
6. Particle density
7. Animation complexity

Terminal bytes are never dropped to maintain frame rate.

## What the frame loop already avoids

- No per-particle CPU work — all physics is one compute dispatch.
- No per-cell or per-glyph draw calls — one instanced draw.
- No full terminal upload per frame — dirty ranges only.
- No GPU wait in the normal update path.
- No descriptor or resource creation per frame.
- No runtime shader compilation when the build-time DXIL is present.

## Adaptive behaviour

- Minimised: zero GPU dispatches; the network is still drained so the ring
  buffer cannot back up.
- Unfocused: frame rate drops to ~10 FPS.
- Particle glow size scales with cell size so text stays legible at any DPI.
