// particles.h — GPU particle system: state buffers, dirty-cell uploads,
// compute simulation dispatch, additive instanced draw.
#pragma once

#include "../common.h"
#include "motion_styles.h"
#include "../dx/device.h"
#include "../dx/shaders.h"
#include "../glyphs/sampler.h"

struct ParticleTunables
{
    float curlAmp = 1.0f;         // px — nixie gas drift amplitude
    float springK = 90.0f;
    float damping = 11.0f;
    float glowSize = 6.0f;        // base half-size px (scaled by font size)
    float transitionDur = 0.25f;  // s — glyph morph time
    float breatheAmp = 0.08f;
    float noiseSpeed = 0.35f;
    float noiseScale = 0.035f;

    // --- four-layer particle model (after amber-particle-ssh) --------------
    // Index into kMotionStyles below (0 Direct, 1 Nebula Twist, ...).
    uint32_t animStyle = 0;
    // Accessibility: replace depth, flashes and rotation with a short Direct
    // morph. Held here rather than applied at the call sites so that every
    // style — present and future — inherits it from one place: the uploaded
    // style becomes Direct and the bulk reveal wave is switched off.
    bool reducedMotion = false;
    float effectSpeed = 1.0f;     // global motion tempo multiplier
    float dragAmt = 0.25f;        // style integrator drag: 0 flowy .. 1 sticky
    float trailScale = 0.035f;    // velocity-streak length in seconds (0 = off)
    // Live particles per cell (8..kMaxParticlesPerCell). Glyph templates are
    // an 8x16 coverage grid — at 128 every glyph pixel has its own particle
    // and strokes render continuously, like a bitmap font.
    uint32_t particlesPerCell = 128;
    float spreadRadius = 0.11f;   // layer spread, fraction of the cell's short side
    float shimmerSpeed = 4.0f;    // dual-sine pulse rate
    float twinkleAmp = 0.10f;     // slow pulse amplitude
    float flickerAmp = 0.04f;     // fast flicker amplitude

    // --- pointer force field ----------------------------------------------
    float mouseX = -1e6f, mouseY = -1e6f;   // px; off-screen disables
    float mouseRadius = 120.0f;
    float mouseForce = 0.0f;                 // 0 disables; ~2500 is a firm push

    // --- shockwave impulse -------------------------------------------------
    float shockX = 0.0f, shockY = 0.0f;
    float shockTime = -1.0f;                 // <0 disables; else time of trigger

    // Accumulated cursor-orbit angle; the app advances it only while the
    // cursor is moving so an idle cursor's border swirl coasts to a halt.
    float cursorPhase = 0.0f;
    // 1 while the cursor travels (short grace), easing to 0 when it parks.
    float cursorActivity = 0.0f;
    float cursorShape = 0.0f;     // 0 block, 1 underline, 2 bar (Appearance page)
    float cursorBlink = 1.0f;     // 0 = steady cursor

    // Panel repulsion: the rectangle of an opaque overlay. The field is
    // pushed out of it and gathers along its edge. Width 0 disables it.
    float panelX = 0.0f, panelY = 0.0f, panelW = 0.0f, panelH = 0.0f;
    float panelMargin = 6.0f;

    // How an erased letter leaves: 0 fade, 1 ash, 2 smoke, 3 sand, 4 shatter.
    uint32_t departStyle = 0;
    // Phosphor warm-up (0 cold .. 1 at temperature) and depth parallax tilt.
    float warmup = 1.0f;
    float tiltX = 0.0f, tiltY = 0.0f;
    // One-frame impulse when the window is dragged or resized.
    float sloshX = 0.0f, sloshY = 0.0f;

    // Light ("paper") mode: 1 selects the dark-ink alpha-blend draw path.
    float lightMode = 0.0f;
    float inkColor[3] = { 0.06f, 0.05f, 0.04f };   // linear dark ink
    float heatAmp = 0.0f;      // activity heat map: extra glow on fresh cells
    float heatTau = 2.5f;      // its decay time (short = diff glow in TUIs)
    float phosphorDecay = 0.0f;   // >0: slow light decay (phosphor persistence)
    float ghostAmp = 0.0f;     // latency ghosting: cursor smear 0..1
    float audioWind = 0.0f;    // audio-reactive turbulence 0..1
    float rainMode = 0.0f;     // screensaver digital-rain blend 0..1

    // Active theme's intensity ramp (5 linear-light stops); defaults to the
    // Amber Nixie phosphor. Mirrors amber::gThemeStops.
    float ramp[5][3] = {
        { 0.0430f, 0.0187f, 0.0f }, { 0.1960f, 0.0889f, 0.0f },
        { 1.0f, 0.4423f, 0.0f },    { 1.0f, 0.6730f, 0.0660f },
        { 1.0f, 0.8990f, 0.5610f },
    };
};

struct GridMetrics
{
    float cellW = 10, cellH = 20;
    float originX = 8, originY = 8;
    uint32_t cols = 80, rows = 24;
};

class ParticleRenderer
{
public:
    bool Init(Device& dev, ShaderCompiler& sc);

    // (Re)allocate for a grid size; resets particle state when growing.
    void EnsureGrid(uint32_t cols, uint32_t rows);

    // Re-upload glyph target points when the sampler added glyphs. Runs on the
    // copy queue between frames; the direct queue waits on its fence.
    void SyncGlyphPoints(GlyphSampler& sampler);

    // Call after composing visuals, before emitting the crisp core: assigns
    // this frame's birth times. Individual edits are born immediately; a
    // genuine scroll (content shifted by one row) snaps silently; any other
    // bulk repaint (alt-screen paint, clear+redraw) gets a top-to-bottom
    // reveal wave so full screens draw with the motion effects.
    void PlanBirths(const std::vector<CellVisual>& visuals, float time);
    float PlannedBirth(uint32_t cell) const
    {
        return cell < m_birthPlan.size() ? m_birthPlan[cell] : -100.0f;
    }
    // Most recent reveal wave (bulk non-scroll repaint): when it started and
    // seconds per row. The Mothership style darkens rows ahead of the front —
    // the ship's shadow creeping over the city before it arrives.
    // True while this cell's current glyph generation is a quiet in-place
    // replacement (kCellFlagQuiet) — the crisp core must not twirl/tumble.
    bool CellQuiet(uint32_t cell) const
    {
        return cell < m_shadow.size() &&
               (m_shadow[cell].flags & kCellFlagQuiet) != 0u;
    }
    float WaveStart() const { return m_waveStart; }
    float WaveRowDelay() const { return m_waveRowDelay; }

    // Diff cells against the GPU shadow, upload changed ranges, dispatch sim.
    void Simulate(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                  const std::vector<CellVisual>& visuals, uint32_t cursorIndex,
                  float time, float dt, const GridMetrics& gm,
                  float screenW, float screenH, bool hdr);

    // Additive instanced quads into the scene target (after Simulate).
    void Draw(ID3D12GraphicsCommandList* cl);

    ParticleTunables tun;
    uint32_t DirtyCellsLastFrame() const { return m_dirtyLast; }
    uint32_t ParticleCount() const { return m_particleCount; }

    // Re-seeds every particle at its glyph home on the next update.
    // Used when switching tabs so the new screen does not fly in from
    // the previous tab's positions.
    void Reset() { m_resetPending = true; m_resetKeepBright = false; }
    // Session-switch cube: the arriving screen must be fully formed on its
    // first frame — re-seed at brightness and settle every cell instantly.
    void ResetInstant() { m_resetPending = true; m_resetKeepBright = true; m_instantNext = true; }
    // Frame constants uploaded by Simulate — shared with the prim passes.
    D3D12_GPU_VIRTUAL_ADDRESS FrameCbGpu() const { return m_lastCbGpu; }
    // A fresh FrameCB for a frame that does not simulate the glyph field (a
    // VNC desktop tab): the last one Simulate built, with the fields a frame
    // owns — time, size, grid — brought up to date, uploaded into THIS
    // frame's ring. The overlays draw against it. Without it FrameCbGpu()
    // would point into a ring the frame no longer owns, or one that was
    // re-created on a resize: the debug layer's "root descriptor on a
    // deleted resource".
    D3D12_GPU_VIRTUAL_ADDRESS UploadFrameCbOnly(FrameContext& frame, const GridMetrics& gm,
                                                float screenW, float screenH, float time, float dt,
                                                bool hdr);

private:
    void CreateGridBuffers();

    Device* m_dev = nullptr;

    ComPtr<ID3D12RootSignature> m_simRS;
    ComPtr<ID3D12PipelineState> m_simPSO;
    ComPtr<ID3D12RootSignature> m_drawRS;
    ComPtr<ID3D12PipelineState> m_drawPSO;
    ComPtr<ID3D12PipelineState> m_drawPSOLight;   // dark ink, alpha-over

    ComPtr<ID3D12Resource> m_particles;    // UAV, GPU only
    ComPtr<ID3D12Resource> m_cells;        // SRV, CPU-updated dirty ranges
    ComPtr<ID3D12Resource> m_points;       // SRV, glyph target points
    uint64_t m_pointsCapacity = 0;         // glyph count capacity

    // Copy-queue machinery for glyph point uploads.
    ComPtr<ID3D12CommandAllocator> m_copyAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_copyList;
    struct PendingUpload
    {
        ComPtr<ID3D12Resource> staging;
        uint64_t fence = 0;
    };
    std::vector<PendingUpload> m_pendingUploads;
    uint64_t m_pointsVersionSeen = 0;

    std::vector<CellGpu> m_shadow;         // CPU mirror of m_cells
    std::vector<float> m_birthPlan;        // per-cell birth for this frame
    float m_planSuppress = 0.0f;
    bool m_planBulk = false;     // this frame is a bulk repaint (reveal wave)
    uint32_t m_lastPpc = 0;      // density the particle layout was built for
    bool m_resetKeepBright = false;   // reseed at cell brightness (density step)
    bool m_instantNext = false;       // next PlanBirths settles every cell at once
    float m_waveStart = -1e9f;             // last reveal wave (see WaveStart)
    float m_waveRowDelay = 0.0f;
    uint32_t m_cols = 0, m_rows = 0;
    uint32_t m_cellCapacity = 0;
    uint32_t m_particleCount = 0;
    bool m_resetPending = true;
    uint32_t m_dirtyLast = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_lastCbGpu = 0;
    FrameCB m_lastCb{};   // what Simulate last built, for UploadFrameCbOnly
};
