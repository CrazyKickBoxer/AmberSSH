#include "particles.h"

#include <algorithm>
#include <cmath>

static ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* dev, uint64_t size,
                                           D3D12_RESOURCE_STATES state,
                                           D3D12_RESOURCE_FLAGS flags,
                                           const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> res;
    ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               state, nullptr, IID_PPV_ARGS(&res)),
                  "CreateBuffer");
    res->SetName(name);
    return res;
}

bool ParticleRenderer::Init(Device& dev, ShaderCompiler& sc)
{
    m_dev = &dev;
    ID3D12Device* d = dev.Dev();

    // ---- compute root signature: CBV + UAV(particles) + SRV(cells, points)
    {
        D3D12_ROOT_PARAMETER params[4] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 0, 0 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = { 0, 0 };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor = { 1, 0 };
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 4;
        rs.pParameters = params;

        ComPtr<ID3DBlob> blob, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &blob, &err),
                      "sim RS serialize");
        ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_simRS)),
                      "sim RS");
    }
    {
        ShaderBlob cs = sc.Load(L"particle_sim", L"CSMain", L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_simRS.Get();
        pd.CS = cs.Bytecode();
        ThrowIfFailed(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_simPSO)),
                      "sim PSO");
    }

    // ---- draw root signature: CBV + SRV(particles)
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 0, 0 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;

        ComPtr<ID3DBlob> blob, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &blob, &err),
                      "draw RS serialize");
        ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_drawRS)),
                      "draw RS");
    }
    {
        ShaderBlob vs = sc.Load(L"particle_draw", L"VSMain", L"vs_6_0");
        ShaderBlob ps = sc.Load(L"particle_draw", L"PSMain", L"ps_6_0");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_drawRS.Get();
        pd.VS = vs.Bytecode();
        pd.PS = ps.Bytecode();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kSceneFormat;
        pd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_drawPSO)),
                      "draw PSO");

        // Light-mode variant: premultiplied source-over so dark ink darkens
        // the paper background instead of adding light to it.
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(
            d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_drawPSOLight)),
            "draw PSO (light)");
    }

    // Copy-queue command machinery for glyph point uploads.
    ThrowIfFailed(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                            IID_PPV_ARGS(&m_copyAlloc)),
                  "copy allocator");
    ThrowIfFailed(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                                       m_copyAlloc.Get(), nullptr,
                                       IID_PPV_ARGS(&m_copyList)),
                  "copy list");
    m_copyList->Close();
    return true;
}

void ParticleRenderer::CreateGridBuffers()
{
    ID3D12Device* d = m_dev->Dev();
    // Capacity always covers the maximum density so switching density from the
    // menu never reallocates mid-session.
    uint64_t particleBytes = static_cast<uint64_t>(m_cellCapacity) *
                             kMaxParticlesPerCell * sizeof(float) * 8;   // pos,vel,bright,seed,tint,pad
    // Buffers live in COMMON between command lists (implicit decay) and are
    // implicitly promoted to the state of their first use each frame.
    m_particles = CreateBuffer(d, particleBytes,
                               D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               L"ParticleState");
    m_cells = CreateBuffer(d, static_cast<uint64_t>(m_cellCapacity) * sizeof(CellGpu),
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_NONE, L"CellData");
}

void ParticleRenderer::EnsureGrid(uint32_t cols, uint32_t rows)
{
    uint32_t cellCount = cols * rows;
    uint32_t ppc = std::clamp(tun.particlesPerCell, 8u, kMaxParticlesPerCell);
    const bool sameGrid = cols == m_cols && rows == m_rows &&
                          m_shadow.size() == cellCount;
    if (sameGrid && ppc == m_lastPpc)
        return;                     // nothing changed: never disturb the screen
    m_cols = cols;
    m_rows = rows;
    m_lastPpc = ppc;
    m_particleCount = cellCount * ppc;

    if (cellCount > m_cellCapacity)
    {
        m_dev->WaitIdle();
        m_cellCapacity = cellCount + cellCount / 4;
        CreateGridBuffers();
    }
    // A density-only change (auto density stepping under load) re-seeds the
    // particles but keeps every cell's glyph and birth: the shader's reset
    // path drops each particle at home at its cell's brightness, so the
    // screen never rebirths. Only a real grid change starts from blank.
    if (!sameGrid)
    {
        m_shadow.assign(cellCount, CellGpu{});
        m_birthPlan.clear();
    }
    m_resetPending = true;
    m_resetKeepBright = sameGrid;
}

void ParticleRenderer::SyncGlyphPoints(GlyphSampler& sampler)
{
    if (sampler.PointsVersion() == m_pointsVersionSeen)
        return;
    m_pointsVersionSeen = sampler.PointsVersion();

    const std::vector<GlyphPoint>& pts = sampler.Points();
    uint64_t glyphs = sampler.GlyphCount();
    uint64_t bytes = pts.size() * sizeof(GlyphPoint);
    ID3D12Device* d = m_dev->Dev();

    if (glyphs > m_pointsCapacity)
    {
        m_dev->WaitIdle();
        m_pointsCapacity = glyphs + 256;
        m_points = CreateBuffer(d, m_pointsCapacity * kParticlesPerCell * sizeof(GlyphPoint),
                                D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_NONE, L"GlyphPoints");
    }

    // Staging buffer with the full point set.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    PendingUpload up;
    ThrowIfFailed(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&up.staging)),
                  "points staging");
    void* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    up.staging->Map(0, &noRead, &mapped);
    memcpy(mapped, pts.data(), bytes);
    up.staging->Unmap(0, nullptr);

    // Copy queue: buffers use implicit COMMON-state promotion/decay, so no
    // explicit barriers are needed across queues — only fence ordering.
    ThrowIfFailed(m_copyAlloc->Reset(), "copy alloc reset");
    ThrowIfFailed(m_copyList->Reset(m_copyAlloc.Get(), nullptr), "copy list reset");
    m_copyList->CopyBufferRegion(m_points.Get(), 0, up.staging.Get(), 0, bytes);
    ThrowIfFailed(m_copyList->Close(), "copy list close");
    ID3D12CommandList* lists[] = { m_copyList.Get() };
    m_dev->CopyQueue()->ExecuteCommandLists(1, lists);
    up.fence = m_dev->SignalCopy();
    m_dev->DirectWaitCopy(up.fence);
    m_pendingUploads.push_back(std::move(up));
    if (m_pendingUploads.size() > kFramesInFlight)
        m_pendingUploads.erase(m_pendingUploads.begin());
}

void ParticleRenderer::PlanBirths(const std::vector<CellVisual>& visuals,
                                  float time)
{
    uint32_t cellCount = m_cols * m_rows;
    if (cellCount == 0 || visuals.size() < cellCount)
        return;
    m_birthPlan.resize(cellCount);

    // Instant settle (cube session switch): every cell is already at rest —
    // births in the past, no wave, no scatter — so the arriving face is
    // fully formed on its first frame.
    if (m_instantNext)
    {
        m_instantNext = false;
        std::fill(m_birthPlan.begin(), m_birthPlan.end(), time - 100.0f);
        m_planSuppress = 1.0f;
        m_planBulk = false;
        m_waveRowDelay = 0.0f;
        return;
    }

    // "Bulk" must be judged against INK, not the whole grid — a terminal
    // screen is mostly blank, and a full repaint of all visible text is a
    // minority of total cells.
    uint32_t match = 0, checked = 0, inkTotal = 0, inkChanges = 0;
    // Anchors: ink that has sat unchanged for a while. A full-screen app
    // refreshing in place (htop re-sorting its whole process list) keeps its
    // frame — headers, labels, key bar — while a total refresh (clear + ls,
    // a page change) keeps nothing. Anchors that survive the frame prove
    // the screen is the same screen, however much of it changed.
    constexpr float kAnchorAge = 0.8f;
    uint32_t anchorKept = 0;
    for (uint32_t i = 0; i < cellCount; ++i)
    {
        bool oldInk = m_shadow[i].glyph != 0;
        bool newInk = visuals[i].glyph != 0;
        if (oldInk || newInk)
            ++inkTotal;
        if (visuals[i].glyph == m_shadow[i].glyph)
        {
            if (oldInk && time - m_shadow[i].birth > kAnchorAge)
                ++anchorKept;
            continue;
        }
        if (oldInk || newInk)
            ++inkChanges;
        // Scroll signature: a changed cell's new glyph equals what the cell
        // one row below held last frame.
        if (i + m_cols < cellCount)
        {
            ++checked;
            if (visuals[i].glyph == m_shadow[i + m_cols].glyph)
                ++match;
        }
    }

    bool many = inkChanges > 50 && inkChanges * 3 > inkTotal * 2;
    bool scrollLike = many && checked > 0 && match * 10 > checked * 6;
    // Enough surviving anchors (an absolute floor, and a share of the ink)
    // mean an in-place update: never a bulk repaint, whatever the count.
    // Scrolls are judged first — a static status bar must not stop the
    // scroll suppressor from keeping a scrolling screen calm.
    bool anchored = !scrollLike && anchorKept >= 40 &&
                    anchorKept * 100 >= inkTotal * 8;
    bool bulk = many && !anchored;
    m_planSuppress = (bulk && scrollLike) ? 1.0f : 0.0f;
    // Full-screen apps (cells flagged kVisQuiet): an incremental refresh —
    // htop's numbers, a vim keystroke — is quiet; only a bulk repaint (the
    // app opening, a page change) gets the motion style and its wave.
    m_planBulk = bulk;

    // Non-scroll bulk repaint: sweep the reveal down the screen so the whole
    // frame draws with the active motion style instead of popping at once.
    float rowDelay = 0.0f;
    if (bulk && !scrollLike)
    {
        rowDelay = (0.7f / std::max(tun.effectSpeed, 0.1f)) /
                   static_cast<float>(std::max(m_rows, 1u));
        // Slow choreography (a Mothership descent, an iris, a burn) needs a
        // longer sweep or the whole screen arrives before the entrance reads.
        rowDelay *= MotionStyleAt(tun.animStyle).waveScale;
        // Reduced motion: no wave at all — the screen simply changes.
        if (tun.reducedMotion)
            rowDelay = 0.0f;
        m_waveStart = time;
        m_waveRowDelay = rowDelay;
    }

    for (uint32_t i = 0; i < cellCount; ++i)
    {
        if (visuals[i].glyph != m_shadow[i].glyph)
            m_birthPlan[i] = time + rowDelay * static_cast<float>(i / m_cols);
        else
            m_birthPlan[i] = m_shadow[i].birth;
    }
}

void ParticleRenderer::Simulate(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                                const std::vector<CellVisual>& visuals,
                                uint32_t cursorIndex, float time, float dt,
                                const GridMetrics& gm, float screenW, float screenH,
                                bool hdr)
{
    PixScope pix(cl, L"ParticleSim");
    uint32_t cellCount = m_cols * m_rows;
    if (cellCount == 0 || visuals.size() < cellCount)
        return;

    // ---- constants ------------------------------------------------------
    FrameCB cb = {};
    cb.time = time;
    cb.dt = dt;
    cb.curlAmp = tun.curlAmp;
    cb.springK = tun.springK;
    cb.damping = tun.damping;
    cb.transitionDur = tun.transitionDur;
    cb.cellW = gm.cellW;
    cb.cellH = gm.cellH;
    cb.originX = gm.originX;
    cb.originY = gm.originY;
    cb.screenW = screenW;
    cb.screenH = screenH;
    cb.cols = m_cols;
    cb.rows = m_rows;
    cb.particleCount = m_particleCount;
    cb.cursorIndex = cursorIndex;
    cb.cursorBright = 1.05f;
    cb.cursorPhase = tun.cursorPhase;
    cb.cursorActivity = tun.cursorActivity;
    cb.cursorShape = tun.cursorShape;
    cb.cursorBlink = tun.cursorBlink;
    cb.lightMode = tun.lightMode;
    cb.heatAmp = tun.heatAmp;
    cb.heatTau = tun.heatTau;
    cb.phosphorDecay = tun.phosphorDecay;
    cb.ghostAmp = tun.ghostAmp;
    cb.audioWind = tun.audioWind;
    cb.rainMode = tun.rainMode;
    for (int c = 0; c < 3; ++c)
        cb.inkColor[c] = tun.inkColor[c];
    float* rampDst[5] = { cb.ramp0, cb.ramp1, cb.ramp2, cb.ramp3, cb.ramp4 };
    for (int s = 0; s < 5; ++s)
        for (int c = 0; c < 3; ++c)
            rampDst[s][c] = tun.ramp[s][c];
    // Templates are one point per glyph pixel: below 128 particles grow to
    // fill the gaps; above 128 (slot reuse with jitter) they shrink further
    // for finer grain. The 128-slot full grid is the reference size.
    uint32_t ppc = std::clamp(tun.particlesPerCell, 8u, kMaxParticlesPerCell);
    cb.glowSize = tun.glowSize * sqrtf(static_cast<float>(kParticlesPerCell) /
                                       static_cast<float>(ppc));
    cb.breatheAmp = tun.breatheAmp;
    cb.hdrBoost = hdr ? 1.0f : 0.0f;
    // 1 = fresh start (fade in from dark), 2 = reseed in place at the cell's
    // brightness (density step: invisible).
    cb.resetFlag = m_resetPending ? (m_resetKeepBright ? 2u : 1u) : 0u;
    cb.noiseSpeed = tun.noiseSpeed;
    cb.noiseScale = tun.noiseScale;

    // Particle-model extensions. Densities above the template stride reuse
    // sample points on different layers, which is the documented behaviour for
    // glyphs with fewer coverage samples than the requested particle count.
    cb.particlesPerCell = ppc;
    // Reduced motion is applied here, once, on the way to the GPU: the shader
    // then runs its plain Direct path, so no style — including any added
    // later — can produce depth, flashes or rotation while it is on.
    cb.animStyle = tun.reducedMotion ? 0u : tun.animStyle;
    cb.panelX = tun.panelX;
    cb.panelY = tun.panelY;
    cb.panelW = tun.panelW;
    cb.panelH = tun.panelH;
    cb.panelMargin = tun.panelMargin;
    // Reduced motion silences the departures too: they are motion like any
    // other, and the whole point of the switch is one place that governs all
    // of it.
    cb.departStyle = tun.reducedMotion ? 0.0f
                                       : static_cast<float>(tun.departStyle);
    cb.warmup = tun.warmup;
    cb.tiltX = tun.reducedMotion ? 0.0f : tun.tiltX;
    cb.tiltY = tun.reducedMotion ? 0.0f : tun.tiltY;
    cb.sloshX = tun.reducedMotion ? 0.0f : tun.sloshX;
    cb.sloshY = tun.reducedMotion ? 0.0f : tun.sloshY;
    cb.spreadRadius = tun.spreadRadius;
    cb.shimmerSpeed = tun.shimmerSpeed;
    cb.twinkleAmp = tun.twinkleAmp;
    cb.flickerAmp = tun.flickerAmp;
    cb.mouseX = tun.mouseX;
    cb.mouseY = tun.mouseY;
    cb.mouseRadius = tun.mouseRadius;
    cb.mouseForce = tun.mouseForce;
    cb.shockX = tun.shockX;
    cb.shockY = tun.shockY;
    cb.shockTime = tun.shockTime;
    cb.effectSpeed = tun.effectSpeed;
    cb.dragAmt = tun.dragAmt;
    cb.trailScale = tun.trailScale;

    // Birth planning: normally done by the app before the crisp-core pass so
    // both reveal in the same wave; this is the safety net.
    if (m_birthPlan.size() != cellCount)
        PlanBirths(visuals, time);
    cb.scatterSuppress = m_planSuppress;

    UploadRing::Alloc cbAlloc = frame.ring.Allocate(sizeof(FrameCB), 256);
    memcpy(cbAlloc.cpu, &cb, sizeof(cb));
    m_lastCbGpu = cbAlloc.gpu;
    m_lastCb = cb;

    // ---- diff shadow → dirty ranges → CopyBufferRegion ------------------
    m_dirtyLast = 0;
    bool fullUpload = m_resetPending;
    uint32_t rangeStart = UINT32_MAX;
    uint32_t rangeEnd = 0;

    struct Range { uint32_t first, last; };
    Range ranges[64];
    uint32_t rangeCount = 0;
    auto flushRange = [&]()
    {
        if (rangeStart == UINT32_MAX)
            return;
        if (rangeCount < 64)
            ranges[rangeCount++] = { rangeStart, rangeEnd };
        else
            fullUpload = true;   // pathological churn: one full upload is cheaper
        rangeStart = UINT32_MAX;
    };

    for (uint32_t i = 0; i < cellCount; ++i)
    {
        const CellVisual& v = visuals[i];
        CellGpu& s = m_shadow[i];
        bool changed = false;
        // The quiet bit belongs to a glyph GENERATION: decided when the glyph
        // changes and held until the next change, so the shader's whole
        // transition for this generation runs the same path.
        uint32_t quietBit = s.flags & kCellFlagQuiet;
        if (v.glyph != s.glyph)
        {
            s.prevGlyph = s.glyph;
            s.glyph = v.glyph;
            s.birth = (i < m_birthPlan.size()) ? m_birthPlan[i] : time;
            // Full-screen apps: every change that is not part of a bulk
            // repaint stays inside its cell (Direct morph — no style
            // entrance from lines away). A meter bar or a clock digit
            // changes where it stands; only the app opening or a page
            // change plays the motion style.
            quietBit = ((v.flags & kVisQuiet) && !m_planBulk) ? kCellFlagQuiet : 0u;
            changed = true;
        }
        if (fabsf(v.bright - s.bright) > 0.002f)
        {
            s.bright = v.bright;
            changed = true;
        }
        uint32_t wantFlags =
            ((v.flags & kVisSelected) ? kCellFlagSelected : 0u) |
            ((v.flags & kVisColor) ? kCellFlagColor : 0u) | quietBit;
        if (wantFlags != s.flags || v.rgb != s.fgRgb)
        {
            s.flags = wantFlags;
            s.fgRgb = v.rgb;
            changed = true;
        }
        if (changed)
        {
            ++m_dirtyLast;
            if (rangeStart == UINT32_MAX)
                rangeStart = i;
            else if (i > rangeEnd + 16)   // gap: start a new range
            {
                flushRange();
                rangeStart = i;
            }
            rangeEnd = i;
        }
    }
    flushRange();

    bool copiedCells = fullUpload || rangeCount > 0;
    if (copiedCells)
    {
        // m_cells is in COMMON at frame start (buffer decay) and is implicitly
        // promoted to COPY_DEST by the first CopyBufferRegion.
        if (fullUpload)
        {
            uint64_t bytes = static_cast<uint64_t>(cellCount) * sizeof(CellGpu);
            UploadRing::Alloc a = frame.ring.Allocate(bytes, 4);
            memcpy(a.cpu, m_shadow.data(), bytes);
            cl->CopyBufferRegion(m_cells.Get(), 0, a.resource, a.offset, bytes);
            m_dirtyLast = cellCount;
        }
        else
        {
            for (uint32_t r = 0; r < rangeCount; ++r)
            {
                uint32_t first = ranges[r].first;
                uint32_t count = ranges[r].last - first + 1;
                uint64_t bytes = static_cast<uint64_t>(count) * sizeof(CellGpu);
                UploadRing::Alloc a = frame.ring.Allocate(bytes, 4);
                memcpy(a.cpu, &m_shadow[first], bytes);
                cl->CopyBufferRegion(m_cells.Get(),
                                     static_cast<uint64_t>(first) * sizeof(CellGpu),
                                     a.resource, a.offset, bytes);
            }
        }
    }

    // cells: COPY_DEST → NON_PIXEL_SHADER_RESOURCE for the compute read.
    if (copiedCells)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = m_cells.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    // particles: promoted COMMON → UAV by the dispatch below.
    cl->SetComputeRootSignature(m_simRS.Get());
    cl->SetPipelineState(m_simPSO.Get());
    cl->SetComputeRootConstantBufferView(0, cbAlloc.gpu);
    cl->SetComputeRootUnorderedAccessView(1, m_particles->GetGPUVirtualAddress());
    cl->SetComputeRootShaderResourceView(2, m_cells->GetGPUVirtualAddress());
    cl->SetComputeRootShaderResourceView(3, m_points->GetGPUVirtualAddress());
    cl->Dispatch((m_particleCount + 255) / 256, 1, 1);

    // particles: UAV → SRV for the vertex shader.
    {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[0].UAV.pResource = m_particles.Get();
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = m_particles.Get();
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(2, b);
    }

    m_resetPending = false;
}

void ParticleRenderer::Draw(ID3D12GraphicsCommandList* cl)
{
    if (m_particleCount == 0 || m_lastCbGpu == 0)
        return;
    PixScope pix(cl, L"ParticleDraw");
    cl->SetGraphicsRootSignature(m_drawRS.Get());
    cl->SetPipelineState((tun.lightMode > 0.5f && m_drawPSOLight)
                             ? m_drawPSOLight.Get()
                             : m_drawPSO.Get());
    cl->SetGraphicsRootConstantBufferView(0, m_lastCbGpu);
    cl->SetGraphicsRootShaderResourceView(1, m_particles->GetGPUVirtualAddress());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, m_particleCount, 0, 0);
}

D3D12_GPU_VIRTUAL_ADDRESS ParticleRenderer::UploadFrameCbOnly(FrameContext& frame, const GridMetrics& gm,
                                                              float screenW, float screenH, float time,
                                                              float dt, bool hdr)
{
    FrameCB cb = m_lastCb;
    cb.time = time;
    cb.dt = dt;
    cb.cellW = gm.cellW;
    cb.cellH = gm.cellH;
    cb.originX = gm.originX;
    cb.originY = gm.originY;
    cb.screenW = screenW;
    cb.screenH = screenH;
    cb.cols = gm.cols;
    cb.rows = gm.rows;
    cb.hdrBoost = hdr ? 1.0f : 0.0f;
    cb.resetFlag = 0;
    UploadRing::Alloc cbAlloc = frame.ring.Allocate(sizeof(FrameCB), 256);
    if (!cbAlloc.cpu)
        return m_lastCbGpu;
    memcpy(cbAlloc.cpu, &cb, sizeof(cb));
    m_lastCbGpu = cbAlloc.gpu;
    m_lastCb = cb;
    return m_lastCbGpu;
}
