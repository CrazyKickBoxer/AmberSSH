#include "prims.h"

#include <algorithm>
#include <cmath>

namespace
{

enum class BlendMode { Additive, PremultOver };

void FillBlend(D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, BlendMode mode)
{
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    if (mode == BlendMode::Additive)
    {
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
    }
    else   // premultiplied source-over
    {
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}

} // namespace

bool PrimRenderer::Init(Device& dev, ShaderCompiler& sc)
{
    m_dev = &dev;
    ID3D12Device* d = dev.Dev();

    m_under.reserve(4096);
    m_over.reserve(2048);
    m_core.reserve(8192);
    m_text.reserve(2048);

    // ---- rects: CBV(b0) + root SRV(t0 instances) ------------------------
    {
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 0, 0 };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;
        ComPtr<ID3DBlob> blob, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &blob, &err),
                      "rect RS serialize");
        ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rectRS)),
                      "rect RS");
    }

    auto makeGfxPso = [&](ID3D12RootSignature* rs, const wchar_t* stem,
                          BlendMode blend, ComPtr<ID3D12PipelineState>& pso)
    {
        ShaderBlob vs = sc.Load(stem, L"VSMain", L"vs_6_0");
        ShaderBlob ps = sc.Load(stem, L"PSMain", L"ps_6_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = vs.Bytecode();
        pd.PS = ps.Bytecode();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        FillBlend(pd, blend);
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kSceneFormat;
        pd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)),
                      "prim PSO");
    };
    makeGfxPso(m_rectRS.Get(), L"rects", BlendMode::PremultOver, m_rectUnderPSO);
    makeGfxPso(m_rectRS.Get(), L"rects", BlendMode::Additive, m_rectOverPSO);
    makeGfxPso(m_rectRS.Get(), L"rects", BlendMode::PremultOver, m_rectOverBlendPSO);

    // ---- text: CBV(b0) + root SRV(t0) + table SRV(t1 atlas) + sampler ---
    {
        D3D12_DESCRIPTOR_RANGE atlasRange = {};
        atlasRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        atlasRange.NumDescriptors = 1;
        atlasRange.BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = { 0, 0 };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor = { 0, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &atlasRange };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 3;
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
        ComPtr<ID3DBlob> blob, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &blob, &err),
                      "text RS serialize");
        ThrowIfFailed(d->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_textRS)),
                      "text RS");
    }
    makeGfxPso(m_textRS.Get(), L"text_draw", BlendMode::Additive, m_textAddPSO);
    makeGfxPso(m_textRS.Get(), L"text_draw", BlendMode::PremultOver, m_textCorePSO);
    makeGfxPso(m_textRS.Get(), L"emoji_draw", BlendMode::PremultOver, m_emojiPSO);

    // ---- atlas texture --------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = GlyphSampler::kAtlasW;
        rd.Height = GlyphSampler::kAtlasH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8_UNORM;
        rd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                          IID_PPV_ARGS(&m_atlasTex)),
                      "atlas texture");
        m_atlasTex->SetName(L"GlyphAtlas");
        m_atlasInCopyDest = true;

        m_atlasSrvSlot = dev.AllocSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(m_atlasTex.Get(), &sv,
                                    dev.SrvCpu(m_atlasSrvSlot));
    }

    // ---- chrome glyph atlas (R8): the skin's own face for the strip ------
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = GlyphSampler::kAtlasW;
        rd.Height = GlyphSampler::kAtlasH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8_UNORM;
        rd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                          IID_PPV_ARGS(&m_chromeAtlasTex)),
                      "chrome atlas texture");
        m_chromeAtlasTex->SetName(L"ChromeGlyphAtlas");
        m_chromeAtlasInCopyDest = true;

        m_chromeAtlasSrvSlot = dev.AllocSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(m_chromeAtlasTex.Get(), &sv,
                                    dev.SrvCpu(m_chromeAtlasSrvSlot));
    }

    // ---- colour emoji atlas (RGBA) --------------------------------------
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = GlyphSampler::kAtlasW;
        rd.Height = GlyphSampler::kAtlasH;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        ThrowIfFailed(d->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                          IID_PPV_ARGS(&m_colorAtlasTex)),
                      "colour atlas texture");
        m_colorAtlasTex->SetName(L"EmojiAtlas");
        m_colorAtlasSrvSlot = dev.AllocSrv();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        d->CreateShaderResourceView(m_colorAtlasTex.Get(), &sv,
                                    dev.SrvCpu(m_colorAtlasSrvSlot));
    }
    return true;
}

void PrimRenderer::BeginFrame()
{
    m_coreLast = static_cast<uint32_t>(m_core.size());
    m_under.clear();
    m_over.clear();
    m_overBlend.clear();
    m_core.clear();
    m_colorGlyphs.clear();
    m_text.clear();
    m_chromeText.clear();
    m_chromeCore.clear();
}

void PrimRenderer::AddRect(float x, float y, float w, float h, float intensity,
                           float borderPx, PrimLayer layer)
{
    // Compatibility path: amber ramp color. Under-layer fills are opaque
    // enough to block particles behind them; over-layer chrome stays additive.
    float rgb[3];
    AmberRampCpu(intensity, rgb);
    RectInst r = {};
    r.x = x; r.y = y; r.w = w; r.h = h;
    float a = (layer == PrimLayer::Under) ? std::min(1.0f, 0.35f + intensity) : 1.0f;
    for (int i = 0; i < 3; ++i)
        r.c0[i] = r.c1[i] = rgb[i] * intensity * a;
    r.c0[3] = r.c1[3] = (layer == PrimLayer::Under) ? a : 1.0f;
    r.border = borderPx;
    switch (layer)
    {
    case PrimLayer::Under:     m_under.push_back(r);     break;
    case PrimLayer::Over:      m_over.push_back(r);      break;
    case PrimLayer::OverBlend: m_overBlend.push_back(r); break;
    }
}

void PrimRenderer::AddRectRgba(float x, float y, float w, float h,
                               const float rgba[4], float borderPx,
                               PrimLayer layer)
{
    RectInst r = {};
    r.x = x; r.y = y; r.w = w; r.h = h;
    for (int i = 0; i < 3; ++i)
        r.c0[i] = r.c1[i] = rgba[i] * rgba[3];
    r.c0[3] = r.c1[3] = rgba[3];
    r.border = borderPx;
    switch (layer)
    {
    case PrimLayer::Under:     m_under.push_back(r);     break;
    case PrimLayer::Over:      m_over.push_back(r);      break;
    case PrimLayer::OverBlend: m_overBlend.push_back(r); break;
    }
}

void PrimRenderer::AddShapeRgba(float x, float y, float w, float h,
                                const float rgba[4], float borderPx,
                                PrimLayer layer, int shape, float param)
{
    RectInst r = {};
    r.x = x; r.y = y; r.w = w; r.h = h;
    for (int i = 0; i < 3; ++i)
        r.c0[i] = r.c1[i] = rgba[i] * rgba[3];
    r.c0[3] = r.c1[3] = rgba[3];
    r.border = borderPx;
    r.flags = static_cast<float>(shape);
    r.pad0 = param;
    switch (layer)
    {
    case PrimLayer::Under:     m_under.push_back(r);     break;
    case PrimLayer::Over:      m_over.push_back(r);      break;
    case PrimLayer::OverBlend: m_overBlend.push_back(r); break;
    }
}

void PrimRenderer::AddRectGradient(float x, float y, float w, float h,
                                   const float rgba0[4], const float rgba1[4],
                                   bool glint)
{
    RectInst r = {};
    r.x = x; r.y = y; r.w = w; r.h = h;
    for (int i = 0; i < 3; ++i)
    {
        r.c0[i] = rgba0[i] * rgba0[3];
        r.c1[i] = rgba1[i] * rgba1[3];
    }
    r.c0[3] = rgba0[3];
    r.c1[3] = rgba1[3];
    r.flags = glint ? 1.0f : 0.0f;
    m_under.push_back(r);
}

void PrimRenderer::AddCoreGlyph(float cellX, float cellY, char32_t cp,
                                const float rgb[3], float alpha, float shearPx,
                                GlyphSampler& sampler, bool chrome)
{
    const GlyphSampler::AtlasGlyph* g = sampler.AtlasForLazy(cp);
    if (!g || g->w <= 0.0f)
        return;
    TextInst t = {};
    // Snap to the pixel grid so stems land consistently between cells.
    t.x = std::floor(cellX + g->offX + 0.5f);
    t.y = std::floor(cellY + g->offY + 0.5f);
    t.w = g->w;
    t.h = g->h;
    t.u0 = g->u0; t.v0 = g->v0;
    t.u1 = g->u1; t.v1 = g->v1;
    t.color[0] = rgb[0];
    t.color[1] = rgb[1];
    t.color[2] = rgb[2];
    t.color[3] = alpha;
    t.shear = shearPx;
    (chrome ? m_chromeCore : m_core).push_back(t);
}

void PrimRenderer::AddColorGlyph(float cellX, float cellY, char32_t cp,
                                 float alpha, float maxWpx, float cellHpx,
                                 bool vs16, GlyphSampler& sampler)
{
    const GlyphSampler::AtlasGlyph* g = sampler.ColorAtlasForLazy(cp, vs16);
    if (!g || g->w <= 0.0f)
        return;
    // The raster box is one line-height square (≈ two cells wide). A narrow
    // cell (VS16 symbol, regional indicator) squeezes it to fit; the glyph
    // stays centred in its cell span either way.
    float w = g->w, h = g->h;
    if (w > maxWpx)
    {
        float s = maxWpx / w;
        w *= s;
        h *= s;
    }
    TextInst t = {};
    t.x = std::floor(cellX + (maxWpx - w) * 0.5f + 0.5f);
    t.y = std::floor(cellY + (cellHpx - h) * 0.5f + 0.5f);
    t.w = w;
    t.h = h;
    t.u0 = g->u0; t.v0 = g->v0;
    t.u1 = g->u1; t.v1 = g->v1;
    t.color[3] = alpha;   // rgb unused — the atlas carries the colour
    m_colorGlyphs.push_back(t);
}

float PrimRenderer::MeasureText(const std::string& utf8, GlyphSampler& sampler)
{
    // The sum of the advances the glyphs would draw with — per glyph, so a
    // proportional chrome face measures the way it lays out. Missing glyphs
    // fall back to the cell, exactly as AddTextRgb advances them.
    const float cell = sampler.AdvanceEm() * sampler.AtlasFontPx();
    float x = 0;
    for (size_t i = 0; i < utf8.size();)
    {
        unsigned char b = utf8[i];
        char32_t cp = U'?';
        if (b < 0x80) { cp = b; i += 1; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < utf8.size())
        { cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F); i += 2; }
        else if ((b & 0xF0) == 0xE0 && i + 2 < utf8.size())
        { cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F); i += 3; }
        else { i += 1; continue; }
        const GlyphSampler::AtlasGlyph* g = sampler.AtlasForLazy(cp);
        x += (g && g->advance > 0) ? g->advance : cell;
    }
    return x;
}

float PrimRenderer::AddText(float x, float y, const std::string& utf8,
                            float intensity, GlyphSampler& sampler)
{
    float rgb[3];
    AmberRampCpu(intensity, rgb);
    for (float& c : rgb)
        c *= intensity;
    return AddTextRgb(x, y, utf8, rgb, sampler);
}

float PrimRenderer::AddTextCore(float x, float y, const std::string& utf8,
                                const float rgb[3], float alpha,
                                GlyphSampler& sampler, bool chrome)
{
    const float cell = sampler.AdvanceEm() * sampler.AtlasFontPx();
    float penX = x;
    for (size_t i = 0; i < utf8.size();)
    {
        unsigned char b = utf8[i];
        char32_t cp = U'?';
        if (b < 0x80) { cp = b; i += 1; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < utf8.size())
        { cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F); i += 2; }
        else if ((b & 0xF0) == 0xE0 && i + 2 < utf8.size())
        { cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F); i += 3; }
        else { i += 1; continue; }
        AddCoreGlyph(penX, y, cp, rgb, alpha, 0.0f, sampler, chrome);
        const GlyphSampler::AtlasGlyph* g = sampler.AtlasForLazy(cp);
        penX += (g && g->advance > 0) ? g->advance : cell;
    }
    return penX - x;
}

float PrimRenderer::AddTextRgb(float x, float y, const std::string& utf8,
                               const float rgb[3], GlyphSampler& sampler, bool chrome)
{
    float penX = x;
    for (size_t i = 0; i < utf8.size();)
    {
        // Inline UTF-8 decode.
        unsigned char b = utf8[i];
        char32_t cp = U'?';
        if (b < 0x80) { cp = b; i += 1; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < utf8.size())
        {
            cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
            i += 2;
        }
        else if ((b & 0xF0) == 0xE0 && i + 2 < utf8.size())
        {
            cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) |
                 (utf8[i + 2] & 0x3F);
            i += 3;
        }
        else { i += 1; continue; }

        const GlyphSampler::AtlasGlyph* g = sampler.AtlasForLazy(cp);
        if (!g)
        {
            penX += sampler.AdvanceEm() * sampler.AtlasFontPx();
            continue;
        }
        if (g->w > 0)
        {
            TextInst t = {};
            t.x = penX + g->offX;
            t.y = y + g->offY;
            t.w = g->w;
            t.h = g->h;
            t.u0 = g->u0; t.v0 = g->v0;
            t.u1 = g->u1; t.v1 = g->v1;
            t.color[0] = rgb[0];
            t.color[1] = rgb[1];
            t.color[2] = rgb[2];
            t.color[3] = 1.0f;
            (chrome ? m_chromeText : m_text).push_back(t);
        }
        penX += g->advance > 0 ? g->advance
                               : sampler.AdvanceEm() * sampler.AtlasFontPx();
    }
    return penX - x;
}

void PrimRenderer::SyncMonoAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                                 GlyphSampler& sampler, ID3D12Resource* tex,
                                 uint64_t& versionSeen, bool& inCopyDest)
{

    if (sampler.AtlasVersion() == versionSeen || sampler.AtlasBits().empty())
        return;
    versionSeen = sampler.AtlasVersion();

    PixScope pix(cl, L"AtlasUpload");

    if (!inCopyDest)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        inCopyDest = true;
    }

    // Row-pitch aligned staging copy from the frame's upload ring.
    const uint32_t w = GlyphSampler::kAtlasW;
    const uint32_t h = GlyphSampler::kAtlasH;
    const uint32_t pitch =
        static_cast<uint32_t>(AlignUp(w, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    UploadRing::Alloc a = frame.ring.Allocate(static_cast<uint64_t>(pitch) * h,
                                              D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const auto& bits = sampler.AtlasBits();
    for (uint32_t row = 0; row < h; ++row)
        memcpy(a.cpu + static_cast<size_t>(row) * pitch,
               &bits[static_cast<size_t>(row) * w], w);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = a.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = a.offset;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    inCopyDest = false;
}

void PrimRenderer::SyncAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                             GlyphSampler& sampler)
{
    // The emoji atlas grows independently (lazy per-glyph), so sync it every
    // frame — it self-guards on its own version.
    SyncColorAtlas(cl, frame, sampler);
    SyncMonoAtlas(cl, frame, sampler, m_atlasTex.Get(), m_atlasVersionSeen,
                  m_atlasInCopyDest);
}

void PrimRenderer::SyncChromeAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                                   GlyphSampler& sampler)
{
    SyncMonoAtlas(cl, frame, sampler, m_chromeAtlasTex.Get(), m_chromeAtlasVersionSeen,
                  m_chromeAtlasInCopyDest);
}

void PrimRenderer::SyncColorAtlas(ID3D12GraphicsCommandList* cl,
                                  FrameContext& frame, GlyphSampler& sampler)
{
    if (sampler.ColorAtlasVersion() == m_colorAtlasVersionSeen ||
        sampler.ColorAtlasBits().empty())
        return;
    m_colorAtlasVersionSeen = sampler.ColorAtlasVersion();

    // Upload ONLY the glyph packed on this version bump — the full atlas is
    // 8 MB (the whole upload ring), so copying it wholesale exhausts the ring
    // and throws. The dirty sub-rect is a few KB.
    int dx = 0, dy = 0, dw = 0, dh = 0;
    sampler.ColorDirtyRect(dx, dy, dw, dh);
    if (dw <= 0 || dh <= 0)
        return;
    PixScope pix(cl, L"EmojiAtlasUpload");

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = m_colorAtlasTex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    const uint32_t atlasW = GlyphSampler::kAtlasW;
    const uint32_t rowBytes = static_cast<uint32_t>(dw) * 4;
    const uint32_t pitch =
        static_cast<uint32_t>(AlignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    UploadRing::Alloc a = frame.ring.Allocate(
        static_cast<uint64_t>(pitch) * dh, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const auto& bits = sampler.ColorAtlasBits();
    for (int row = 0; row < dh; ++row)
        memcpy(a.cpu + static_cast<size_t>(row) * pitch,
               &bits[((static_cast<size_t>(dy + row) * atlasW) + dx) * 4],
               rowBytes);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_colorAtlasTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = a.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = a.offset;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(dw);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(dh);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    cl->CopyTextureRegion(&dst, static_cast<UINT>(dx), static_cast<UINT>(dy), 0,
                          &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
}

void PrimRenderer::RecordRects(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                               D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                               const std::vector<RectInst>& rects,
                               ID3D12PipelineState* pso)
{
    if (rects.empty())
        return;
    uint64_t bytes = rects.size() * sizeof(RectInst);
    UploadRing::Alloc a = frame.ring.Allocate(bytes, 16);
    memcpy(a.cpu, rects.data(), bytes);

    cl->SetGraphicsRootSignature(m_rectRS.Get());
    cl->SetPipelineState(pso);
    cl->SetGraphicsRootConstantBufferView(0, frameCb);
    cl->SetGraphicsRootShaderResourceView(1, a.gpu);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, static_cast<UINT>(rects.size()), 0, 0);
}

void PrimRenderer::RecordText(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                              D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                              const std::vector<TextInst>& glyphs,
                              ID3D12PipelineState* pso, uint32_t srvSlot,
                              bool inCopyDest)
{
    if (glyphs.empty() || inCopyDest)
        return;
    uint64_t bytes = glyphs.size() * sizeof(TextInst);
    UploadRing::Alloc a = frame.ring.Allocate(bytes, 16);
    memcpy(a.cpu, glyphs.data(), bytes);

    cl->SetGraphicsRootSignature(m_textRS.Get());
    cl->SetPipelineState(pso);
    cl->SetGraphicsRootConstantBufferView(0, frameCb);
    cl->SetGraphicsRootShaderResourceView(1, a.gpu);
    cl->SetGraphicsRootDescriptorTable(2, m_dev->SrvGpu(srvSlot));
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, static_cast<UINT>(glyphs.size()), 0, 0);
}

void PrimRenderer::RecordUnder(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                               D3D12_GPU_VIRTUAL_ADDRESS frameCb)
{
    PixScope pix(cl, L"PrimsUnder");
    RecordRects(cl, frame, frameCb, m_under, m_rectUnderPSO.Get());
}

void PrimRenderer::RecordCore(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                              D3D12_GPU_VIRTUAL_ADDRESS frameCb)
{
    PixScope pix(cl, L"GlyphCore");
    m_coreLast = static_cast<uint32_t>(m_core.size());
    RecordText(cl, frame, frameCb, m_core, m_textCorePSO.Get(), m_atlasSrvSlot, m_atlasInCopyDest);
    RecordText(cl, frame, frameCb, m_chromeCore, m_textCorePSO.Get(), m_chromeAtlasSrvSlot, m_chromeAtlasInCopyDest);
}

void PrimRenderer::RecordColor(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                               D3D12_GPU_VIRTUAL_ADDRESS frameCb)
{
    if (m_colorGlyphs.empty())
        return;
    PixScope pix(cl, L"EmojiGlyphs");
    uint64_t bytes = m_colorGlyphs.size() * sizeof(TextInst);
    UploadRing::Alloc a = frame.ring.Allocate(bytes, 16);
    memcpy(a.cpu, m_colorGlyphs.data(), bytes);

    cl->SetGraphicsRootSignature(m_textRS.Get());
    cl->SetPipelineState(m_emojiPSO.Get());
    cl->SetGraphicsRootConstantBufferView(0, frameCb);
    cl->SetGraphicsRootShaderResourceView(1, a.gpu);
    cl->SetGraphicsRootDescriptorTable(2, m_dev->SrvGpu(m_colorAtlasSrvSlot));
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, static_cast<UINT>(m_colorGlyphs.size()), 0, 0);
}

void PrimRenderer::DrawImage(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                             D3D12_GPU_VIRTUAL_ADDRESS frameCb, uint32_t srvSlot,
                             float x, float y, float w, float h, float alpha)
{
    TextInst t = {};
    t.x = x; t.y = y; t.w = w; t.h = h;
    t.u0 = 0.0f; t.v0 = 0.0f; t.u1 = 1.0f; t.v1 = 1.0f;
    t.color[3] = alpha;
    UploadRing::Alloc a = frame.ring.Allocate(sizeof(TextInst), 16);
    memcpy(a.cpu, &t, sizeof(TextInst));
    cl->SetGraphicsRootSignature(m_textRS.Get());
    cl->SetPipelineState(m_emojiPSO.Get());
    cl->SetGraphicsRootConstantBufferView(0, frameCb);
    cl->SetGraphicsRootShaderResourceView(1, a.gpu);
    cl->SetGraphicsRootDescriptorTable(2, m_dev->SrvGpu(srvSlot));
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cl->DrawInstanced(4, 1, 0, 0);
}

void PrimRenderer::RecordOverBlend(ID3D12GraphicsCommandList* cl,
                                   FrameContext& frame,
                                   D3D12_GPU_VIRTUAL_ADDRESS frameCb)
{
    if (m_overBlend.empty())
        return;
    PixScope pix(cl, L"PrimsOverBlend");
    RecordRects(cl, frame, frameCb, m_overBlend, m_rectOverBlendPSO.Get());
}

void PrimRenderer::RecordOver(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                              D3D12_GPU_VIRTUAL_ADDRESS frameCb)
{
    PixScope pix(cl, L"PrimsOver");
    RecordRects(cl, frame, frameCb, m_over, m_rectOverPSO.Get());
    RecordText(cl, frame, frameCb, m_text, m_textAddPSO.Get(), m_atlasSrvSlot, m_atlasInCopyDest);
    RecordText(cl, frame, frameCb, m_chromeText, m_textAddPSO.Get(), m_chromeAtlasSrvSlot, m_chromeAtlasInCopyDest);
}
