// prims.h — immediate-mode 2D primitives for the terminal and UI, in four
// batches drawn around the particle pass:
//
//   under  — RGBA rects, premultiplied source-over: ANSI cell backgrounds,
//            the Miami Sunset selection gradient, tab-strip fills.
//   core   — crisp DirectWrite glyph quads, premultiplied source-over: the
//            sharp letterforms that carry legibility (no blur, ANSI colored).
//   over   — additive rects: underlines, carets, borders, UI glow chrome.
//   text   — additive amber atlas text: tabs, status line, debug overlay.
#pragma once

#include "../common.h"
#include "../dx/device.h"
#include "../dx/shaders.h"
#include "../glyphs/sampler.h"

// Under: opaque-ish fills drawn before the particle field (cell backgrounds).
// Over: additive chrome drawn after it (glows, hairlines, UI text).
// OverBlend: premultiplied-over panels drawn after the field — the only layer
// that can DARKEN what is beneath it (palette backdrop, dialogs).
enum class PrimLayer { Under, Over, OverBlend };

class PrimRenderer
{
public:
    bool Init(Device& dev, ShaderCompiler& sc);

    void BeginFrame();

    // Amber-intensity compatibility API (UI chrome).
    void AddRect(float x, float y, float w, float h, float intensity,
                 float borderPx, PrimLayer layer);
    // Straight-alpha RGBA rect in linear color; premultiplied internally.
    void AddRectRgba(float x, float y, float w, float h,
                     const float rgba[4], float borderPx, PrimLayer layer);
    // Skin shapes (rects.hlsl flags): 2 chamfer (param = corner cut px),
    // 3 slant (param = lean px, parallelogram), 4 hexcut (all corners).
    // Border > 0 outlines the shape.
    void AddShapeRgba(float x, float y, float w, float h, const float rgba[4],
                      float borderPx, PrimLayer layer, int shape, float param);
    // Horizontal gradient rect (selection): straight-alpha linear RGBA at the
    // left and right edges; glint adds the cyan top-edge light.
    void AddRectGradient(float x, float y, float w, float h,
                         const float rgba0[4], const float rgba1[4], bool glint);

    // Crisp glyph core at a terminal cell origin, in linear RGB. shearPx
    // skews the glyph top for italics. Uses the lazy atlas.
    void AddCoreGlyph(float cellX, float cellY, char32_t cp,
                      const float rgb[3], float alpha, float shearPx,
                      GlyphSampler& sampler, bool chrome = false);

    // Full-colour emoji glyph at a terminal cell origin (its own colours).
    // maxWpx is the cell budget (1 or 2 cells wide) — oversized glyphs are
    // scaled down and centred; vs16 forces the colour form of a text-default
    // symbol (❤ + U+FE0F).
    void AddColorGlyph(float cellX, float cellY, char32_t cp, float alpha,
                       float maxWpx, float cellHpx, bool vs16,
                       GlyphSampler& sampler);

    // Additive amber UI text; returns the advance width in pixels.
    float AddText(float x, float y, const std::string& utf8, float intensity,
                  GlyphSampler& sampler);
    // Additive UI text in an explicit linear colour (skin accents).
    float AddTextRgb(float x, float y, const std::string& utf8, const float rgb[3],
                     GlyphSampler& sampler, bool chrome = false);
    // Opaque (source-over) UI text through the crisp-core pass: the only way
    // to put DARK text on a coloured bar (LCARS). Returns the advance.
    float AddTextCore(float x, float y, const std::string& utf8, const float rgb[3],
                      float alpha, GlyphSampler& sampler, bool chrome = false);
    float MeasureText(const std::string& utf8, GlyphSampler& sampler);

    // The chrome atlas: the skin's own face for the title strip, fed by a
    // second sampler and drawn through the same text PSOs with its own SRV.
    // Text added with chrome = true samples this atlas.
    void SyncChromeAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                         GlyphSampler& sampler);
    // Upload the atlas texture(s) if the sampler rebuilt or extended them.
    void SyncAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                   GlyphSampler& sampler);

    void RecordUnder(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                     D3D12_GPU_VIRTUAL_ADDRESS frameCb);
    void RecordCore(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                    D3D12_GPU_VIRTUAL_ADDRESS frameCb);
    // Colour emoji, drawn premultiplied source-over (after the core).
    void RecordColor(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                     D3D12_GPU_VIRTUAL_ADDRESS frameCb);
    void RecordOver(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                    D3D12_GPU_VIRTUAL_ADDRESS frameCb);
    // OverBlend panels: after the particle field, before additive chrome.
    void RecordOverBlend(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                         D3D12_GPU_VIRTUAL_ADDRESS frameCb);
    // Immediate quad from an arbitrary RGBA SRV (inline terminal images),
    // premultiplied-over. Call during scene recording, after RecordColor.
    void DrawImage(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                   D3D12_GPU_VIRTUAL_ADDRESS frameCb, uint32_t srvSlot,
                   float x, float y, float w, float h, float alpha);

    uint32_t CoreGlyphsLastFrame() const { return m_coreLast; }

private:
    struct RectInst
    {
        float x, y, w, h;
        float c0[4];       // premultiplied linear RGBA, left edge
        float c1[4];       // right edge
        float border, flags, pad0, pad1;
    };
    struct TextInst
    {
        float x, y, w, h;
        float u0, v0, u1, v1;
        float color[4];    // straight linear RGBA
        float shear, pad0, pad1, pad2;
    };
    static_assert(sizeof(RectInst) == 64, "matches rects.hlsl");
    static_assert(sizeof(TextInst) == 64, "matches text_draw.hlsl");

    void RecordRects(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                     D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                     const std::vector<RectInst>& rects, ID3D12PipelineState* pso);
    void RecordText(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                    D3D12_GPU_VIRTUAL_ADDRESS frameCb,
                    const std::vector<TextInst>& glyphs, ID3D12PipelineState* pso,
                    uint32_t srvSlot, bool inCopyDest);
    // One R8 atlas upload, shared by the terminal and chrome atlases.
    void SyncMonoAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                       GlyphSampler& sampler, ID3D12Resource* tex,
                       uint64_t& versionSeen, bool& inCopyDest);
    void SyncColorAtlas(ID3D12GraphicsCommandList* cl, FrameContext& frame,
                        GlyphSampler& sampler);

    Device* m_dev = nullptr;
    ComPtr<ID3D12RootSignature> m_rectRS;
    ComPtr<ID3D12PipelineState> m_rectOverPSO;    // additive
    ComPtr<ID3D12PipelineState> m_rectOverBlendPSO;   // premultiplied over
    ComPtr<ID3D12PipelineState> m_rectUnderPSO;   // premultiplied source-over
    ComPtr<ID3D12RootSignature> m_textRS;
    ComPtr<ID3D12PipelineState> m_textAddPSO;     // additive (UI amber)
    ComPtr<ID3D12PipelineState> m_textCorePSO;    // premultiplied source-over
    ComPtr<ID3D12PipelineState> m_emojiPSO;       // colour atlas, premult over

    ComPtr<ID3D12Resource> m_atlasTex;
    uint32_t m_atlasSrvSlot = UINT32_MAX;
    uint64_t m_atlasVersionSeen = 0;
    bool m_atlasInCopyDest = false;

    ComPtr<ID3D12Resource> m_chromeAtlasTex;      // the strip's own face
    uint32_t m_chromeAtlasSrvSlot = UINT32_MAX;
    uint64_t m_chromeAtlasVersionSeen = 0;
    bool m_chromeAtlasInCopyDest = false;

    ComPtr<ID3D12Resource> m_colorAtlasTex;       // RGBA colour emoji atlas
    uint32_t m_colorAtlasSrvSlot = UINT32_MAX;
    uint64_t m_colorAtlasVersionSeen = 0;

    std::vector<RectInst> m_under;
    std::vector<RectInst> m_over;
    std::vector<RectInst> m_overBlend;
    std::vector<TextInst> m_core;
    std::vector<TextInst> m_colorGlyphs;
    std::vector<TextInst> m_text;
    std::vector<TextInst> m_chromeText;   // additive, chrome atlas
    std::vector<TextInst> m_chromeCore;   // source-over, chrome atlas
    uint32_t m_coreLast = 0;
};
