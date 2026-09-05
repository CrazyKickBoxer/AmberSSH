// desktop.h — the VNC particle desktop: a remote framebuffer rendered as
// one particle per pixel (or two, three, four), each fixed to its source
// pixel and coloured from it every frame.
//
// This is a sibling of ParticleRenderer, not a mode of it. The terminal's
// particles belong to glyph cells and are choreographed by births and
// templates; a desktop particle belongs to a pixel and has a home it never
// leaves for long. They share the device, the per-frame upload ring, the
// scene target, bloom and composite, the motion styles (as force fields, see
// shaders/motion_fields.hlsli) and the appearance controls; they share no
// buffers and no shaders.
//
// Ownership: the render thread owns everything here. The decode worker
// hands it Damage (rectangles plus their pixels, already copied out); this
// class uploads only those rectangles, computes each pixel's colour delta
// against its own CPU shadow of the framebuffer, and injects that delta as
// disturbance energy. Uploads go through the frame's UploadRing when they
// fit, and through a per-frame-slot staging buffer sized for a full
// framebuffer when they do not (a first picture, a resize, a recovery), so
// reuse is protected by the same fence the frame's other uploads rely on.
//
// The budget: particle_count = framebuffer_width x framebuffer_height x
// density, clamped to kMaxParticles and to what the adapter's memory
// allows. When even density 1 exceeds it, the desktop is SAMPLED: every
// stride-th pixel in each axis gets the particle, the effective resolution
// is reported in Layout and on the overlay, and nothing claims one particle
// per pixel that is not.
#pragma once

#include "../common.h"
#include "../dx/device.h"
#include "../dx/shaders.h"
#include "../vnc/RfbDecoders.h"
#include "../vnc/VncSession.h"

// Mirrored in shaders/desktop_common.hlsli; ShaderContractTests compares the
// scalar counts. Tightly packed, 4-byte scalars, 16-byte aligned.
struct DesktopCB
{
    float time = 0, dt = 0, effectSpeed = 1, solidity = 1;
    float springK = 400, damping = 24, curlAmp = 1, curlScale = 0.004f;
    // energyDecay: retention per second — 0.03 means a changed pixel's heat
    // is a third gone in a tenth of a second and all but gone in half; a
    // longer tail reads as ghosting on a desktop
    float particleSize = 1, glowSize = 3, disturbance = 1, energyDecay = 0.03f;
    float dstX = 0, dstY = 0, scale = 1, jitter = 0;
    float screenW = 1, screenH = 1, mouseX = -1e6f, mouseY = -1e6f;
    float mouseRadius = 120, mouseForce = 0, shockX = 0, shockY = 0;
    float shockTime = -1, lightMode = 0, hdrBoost = 0, audioWind = 0;
    uint32_t fbW = 0, fbH = 0, density = 1, stride = 1;
    uint32_t particleCount = 0, animStyle = 0, reducedMotion = 0, faithful = 1;
    uint32_t resetFlag = 0, sampledW = 0, sampledH = 0, cursorCount = 0;
    float brightness = 1, dragAmt = 0.25f, cursorX = -1e6f, cursorY = -1e6f;
    float cursorScale = 0, cursorW = 0, cursorH = 0, motion = 1;
    float fxShock = 0, fxEdge = 0, fxHeat = 0, fxMaterialise = 0;
    float bornTime = -1e6f, shockAmp = 1, edgeGain = 4, vivid = 1;
    float shockStyle = 0, transition = 0, transitionSecs = 0.32f, streak = 0;
    float cursorHotX = 0, cursorHotY = 0, cursorGrid = 16, instanceBase = 0;
    float ignite = 0, trails = 1, prism = 0, tails = 0;
    float irisX = 0, irisY = 0, irisTime = -1e6f, irisSpeed = 600;
};
static_assert(sizeof(DesktopCB) == 72 * 4, "DesktopCB must stay 72 scalars, mirrored in HLSL");
static_assert(sizeof(DesktopCB) % 16 == 0, "constant buffers are 16-byte aligned");

// The largest desktop the particle field takes at full density. 3840x2160
// at density 1 and 1920x1080 at density 4 are both 8 294 400, just inside.
constexpr uint32_t kMaxDesktopParticles = 8u * 1024 * 1024;
// The pointer's cluster.
// The pointer's cluster: a grid the server's cursor shape is laid out on
// (up to 24 x 24 = 576 particles, one per shape pixel), and a ring of 64
// around it that is visible whatever the desktop underneath is doing.
constexpr uint32_t kCursorGrid = 24;
constexpr uint32_t kCursorHalo = 64;
constexpr uint32_t kCursorParticles = kCursorGrid * kCursorGrid + kCursorHalo;

class DesktopParticles
{
public:
    struct Layout
    {
        uint32_t fbW = 0, fbH = 0;      // the framebuffer as decoded
        uint32_t density = 1;           // particles per SAMPLED pixel, after clamping
        uint32_t stride = 1;            // 1 = every pixel; s = every s-th pixel per axis
        uint32_t sampledW = 0, sampledH = 0;   // the effective resolution
        uint32_t particles = 0;         // fbW/stride x fbH/stride x density
        uint64_t gpuBytes = 0;          // what this layout allocates
        bool clamped = false;           // the requested density was reduced
        bool sampled = false;           // stride > 1
    };

    struct Params
    {
        float time = 0, dt = 0;
        float solidity = 1;             // 0..1
        float particleSize = 1;         // 1..3 px
        float disturbance = 1;          // 0..2
        uint32_t animStyle = 0;
        float effectSpeed = 1;
        float dragAmt = 0.25f;
        bool reducedMotion = false;
        bool lightMode = false;
        float hdrBoost = 0;
        float audioWind = 0;
        float brightness = 1;
        float mouseX = -1e6f, mouseY = -1e6f, mouseRadius = 120, mouseForce = 0;
        float shockX = 0, shockY = 0, shockTime = -1;
        float shockAmp = 1;             // +1 outward, negative inward
        int shockStyle = 0;             // 0 ring, 1 water drop, 2 splash, 3 vortex
        // How a changed region redraws over transitionSecs: 0 at once, 1
        // burn, 2 dissolve, 3 scan wipe, 4 emboss flash — those four recolour
        // each pixel in place — and 5 light speed, where the particles
        // themselves fly in from far out on curved paths, streaked and blue
        // with their own speed. Anything but 0 is an effect (not pixel-exact).
        int transition = 0;
        float transitionSecs = 0.32f;
        // How motion is drawn. Each is independent of the others and of the
        // transition; all four cost nothing while nothing is moving.
        bool prism = false;      // red and blue separate along the velocity
        bool tails = false;      // the streak's tail bends by the field it flew through
        bool trails = false;     // three sub-positions a frame: real exposure, not a smear
        bool ignite = false;     // energy propagates as a front, conducted by the picture
        // The effects, 0 off .. 1 full (docs/vnc.md, "Effects"). Any of them
        // above 0 takes the desktop out of faithful mode: the picture is
        // exact only once they have settled, and edge glow never settles.
        float fxShock = 0, fxEdge = 0, fxHeat = 0, fxMaterialise = 0;
        // Vividness: saturation and contrast about mid grey, 1 = the decoded
        // colours; ignored (1) in faithful mode. Motion: the tempo of the
        // swarm's drift, the motion style and the effects, 1 = as designed.
        float vivid = 1, motion = 1;
        // where the framebuffer sits on screen: top-left and px scale
        float dstX = 0, dstY = 0, scale = 1;
        // the local pointer, in screen px, and whether to draw its cluster
        float cursorX = -1e6f, cursorY = -1e6f;
        bool showCursor = false;
    };

    bool Init(Device& dev, ShaderCompiler& sc);

    // (Re)creates every resource for a framebuffer size and requested
    // density, within the budget. Returns false only when nothing can be
    // allocated at all. Costs a full re-upload: the caller sends the whole
    // framebuffer as the next Damage.
    bool Configure(uint32_t fbW, uint32_t fbH, uint32_t density);
    const Layout& GetLayout() const { return m_layout; }
    bool Ready() const { return m_layout.particles > 0; }

    // First thing each frame the desktop is drawn: stamps the pass's start
    // and resets the per-frame counters — a frame with no damage never
    // reaches Upload, and the stamp has to exist regardless.
    void Begin(ID3D12GraphicsCommandList* cl);
    // The rectangles that changed, uploaded and their deltas injected as
    // energy. Must precede Simulate in the same command list.
    void Upload(ID3D12GraphicsCommandList* cl, FrameContext& frame, const amber::vnc::Damage& d,
                float disturbance);
    // A new pointer shape (or none: width 0 hides it).
    void SetCursor(ID3D12GraphicsCommandList* cl, FrameContext& frame, const amber::vnc::CursorShape& cs);
    const amber::vnc::CursorShape& Cursor() const { return m_cursorShape; }

    void Simulate(ID3D12GraphicsCommandList* cl, FrameContext& frame, const Params& p,
                  float screenW, float screenH);
    void Draw(ID3D12GraphicsCommandList* cl);

    // For the self-check: the disturbance energy texture, sampledW x
    // sampledH floats, read back synchronously. Never per frame.
    bool ReadbackEnergy(std::vector<float>& out, uint32_t& w, uint32_t& h);

    // Diagnostics for the overlay.
    uint32_t UploadedRectsLastFrame() const { return m_rectsLast; }
    uint64_t UploadedBytesLastFrame() const { return m_bytesLast; }
    bool LastUploadFell() const { return m_fellBack; }   // a rect went to staging, not the ring

private:
    struct Staging
    {
        ComPtr<ID3D12Resource> buffer;
        uint64_t size = 0;
        uint8_t* mapped = nullptr;
    };
    bool UploadRect(ID3D12GraphicsCommandList* cl, FrameContext& frame, ID3D12Resource* target,
                    uint32_t bpp, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    const uint8_t* rows, uint32_t srcPitch);
    Staging& StagingFor(FrameContext& frame, uint64_t bytes);
    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES& cur,
                    D3D12_RESOURCE_STATES to);

    Device* m_dev = nullptr;
    Layout m_layout;

    ComPtr<ID3D12RootSignature> m_energyRS, m_simRS, m_drawRS;
    ComPtr<ID3D12PipelineState> m_energyPSO, m_simPSO, m_drawAdd, m_drawOver;

    ComPtr<ID3D12Resource> m_particles;   // DeskParticle x (particles + cursor)
    ComPtr<ID3D12Resource> m_frame;       // B8G8R8A8_UNORM fbW x fbH
    ComPtr<ID3D12Resource> m_energy;      // R16_FLOAT sampledW x sampledH
    ComPtr<ID3D12Resource> m_inject;      // R8_UNORM sampledW x sampledH
    ComPtr<ID3D12Resource> m_cursor;      // B8G8R8A8_UNORM, up to kMaxCursorDim square
    // For the redraw transitions: what each changed pixel was before its
    // last change (copied out of m_frame before the upload overwrites it),
    // and when it changed (seconds, written by the energy pass)
    ComPtr<ID3D12Resource> m_prev;        // B8G8R8A8_UNORM fbW x fbH
    ComPtr<ID3D12Resource> m_stamp;       // R32_FLOAT sampledW x sampledH
    // Last frame's energy, so the ignition front can read its neighbours
    // while this frame's is being written: copied, not ping-ponged, so the
    // descriptors the sim and draw hold never have to change.
    ComPtr<ID3D12Resource> m_energyPrev;  // R16_FLOAT sampledW x sampledH
    D3D12_RESOURCE_STATES m_energyPrevState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES m_prevState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES m_stampState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_particleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_frameState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES m_injectState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES m_cursorState = D3D12_RESOURCE_STATE_COPY_DEST;
    // energy is a UAV for the energy and sim passes and an SRV for the
    // draw's vertex stage (the heat effect reads it per particle)
    D3D12_RESOURCE_STATES m_energyState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // descriptors, allocated once and rewritten on Configure: the draw's
    // table is five consecutive SRVs (frame, cursor, energy, prev, stamp);
    // the energy pass's three consecutive UAVs (energy, inject, stamp)
    uint32_t m_slotFrameSrv = UINT32_MAX, m_slotCursorSrv = UINT32_MAX, m_slotEnergySrv = UINT32_MAX;
    uint32_t m_slotPrevSrv = UINT32_MAX, m_slotStampSrv = UINT32_MAX;
    uint32_t m_slotEnergyUav = UINT32_MAX, m_slotInjectUav = UINT32_MAX, m_slotStampUav = UINT32_MAX;
    // the energy pass's own reads: last frame's energy and the framebuffer
    // (whose local contrast decides what the front conducts through)
    uint32_t m_slotEnergyPrevSrv = UINT32_MAX, m_slotFrameSrv2 = UINT32_MAX;
    double m_bornTime = -1e6;                     // Params::time at the last reset

    // the CPU shadow of the framebuffer, for colour deltas
    std::vector<uint32_t> m_shadow;
    std::vector<uint8_t> m_injectScratch;
    Staging m_staging[kFramesInFlight];
    // staging is indexed by the frame slot the FrameContext belongs to
    const FrameContext* m_frameSlots[kFramesInFlight] = {};

    amber::vnc::CursorShape m_cursorShape;
    bool m_needReset = true;
    uint32_t m_rectsLast = 0;
    uint64_t m_bytesLast = 0;
    bool m_fellBack = false;
    uint64_t m_stagingHead = 0;                   // bump offset into this frame's staging
    D3D12_GPU_VIRTUAL_ADDRESS m_cbGpu = 0;        // this frame's DesktopCB, shared by the passes
    bool m_lastFaithful = true, m_lastLight = false;
    uint32_t m_lastInstances = 0;                 // particles + cursor cluster, as simulated
    uint32_t m_lastCursorCount = 0;               // ... of which the pointer's
    uint32_t m_lastTrails = 1;                    // exposure copies per particle this frame
    // The last damage's bounding box, in framebuffer pixels: its centre and
    // half-diagonal are what the iris, the sonic boom and the odometer key
    // off, so they act on the region rather than on each pixel alone.
    float m_damageCx = 0, m_damageCy = 0, m_damageReach = 0;
    bool m_damageFresh = false;                   // set by Upload, consumed by Simulate
    double m_damageAt = -1e6;                     // when it arrived (Params::time)
    D3D12_GPU_VIRTUAL_ADDRESS m_cbCursorGpu = 0;  // the same constants with instanceBase set
};
