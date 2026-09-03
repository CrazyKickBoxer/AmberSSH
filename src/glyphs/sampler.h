// sampler.h — DirectWrite glyph rasterization → 32 well-spread particle target
// points per glyph, plus a crisp glyph atlas for UI (connect screen) text.
#pragma once

#include "../common.h"
#include <dwrite_3.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class GlyphSampler
{
public:
    static constexpr uint32_t kRasterW = 64;
    static constexpr uint32_t kRasterH = 128;
    static constexpr uint32_t kAtlasW = 2048;
    static constexpr uint32_t kAtlasH = 1024;

    // Both defined in the .cpp so the D2D/WIC ComPtr members' special-member
    // instantiation happens where those types are complete (not in app.cpp).
    GlyphSampler();
    ~GlyphSampler();
    bool Init();  // JetBrains Mono, falling back to Consolas
    const std::wstring& FontName() const { return m_fontName; }

    // Loads every .ttf/.otf under the given directories into a private
    // DirectWrite collection, so bundled/downloaded fonts resolve by family
    // name without a system install. Safe to call repeatedly (rebuilds).
    void LoadAppFonts(const std::vector<std::wstring>& dirs);
    // True when the family resolves in the system OR the private collection.
    bool HasFamily(const std::wstring& family);

    // Switches the terminal typeface (system font family). Returns false and
    // keeps the current face when the family is not installed. The caller
    // must re-run font metrics (cell size changes) — templates and atlas are
    // invalidated here.
    bool SetFontFamily(const std::wstring& family);

    // Particle template style. Modern samples the antialiased raster on the
    // full 8x16 grid; the dot-matrix styles quantize to classic printer pin
    // grids (8-pin: 6x8 dots, 12-pin: 8x12 dots) with hard on/off dots.
    enum class TemplateStyle { Modern = 0, DotMatrix8 = 1, DotMatrix12 = 2 };
    void SetTemplateStyle(TemplateStyle s);   // rebuilds all glyph templates
    TemplateStyle GetTemplateStyle() const { return m_style; }

    // Cell metric ratios in em units (multiply by font pixel size).
    float AdvanceEm() const { return m_advanceEm; }
    float AscentEm() const { return m_ascentEm; }
    float LineEm() const { return m_lineEm; }

    // Particle target points -------------------------------------------------
    // Glyph 0 is the blank/parked glyph (all points at cell center).
    uint32_t GlyphIdFor(char32_t cp);              // rasterizes lazily
    const std::vector<GlyphPoint>& Points() const { return m_points; }
    uint32_t GlyphCount() const { return m_glyphCount; }
    uint64_t PointsVersion() const { return m_pointsVersion; }

    // UI text atlas ----------------------------------------------------------
    struct AtlasGlyph
    {
        float u0, v0, u1, v1;   // normalized atlas UVs
        float w, h;             // pixels
        float offX, offY;       // offset from character-box top-left
        float advance;          // pixels
    };
    void EnsureAtlas(float fontPx);                 // rebuilds if size changed
    // A glyph's own advance from the design metrics (see sampler.cpp).
    float DesignAdvancePx(char32_t cp, float px, float fallbackPx);
    const AtlasGlyph* AtlasFor(char32_t cp) const;

    // --- full-color emoji ---------------------------------------------------
    // True for pictographic emoji codepoints that should render in colour
    // (not box-drawing / braille / symbols the mono path handles fine).
    static bool IsColorEmoji(char32_t cp);
    // Rasterizes (lazily, via Direct2D/DirectWrite colour glyph rendering) and
    // returns the colour-atlas placement, or null if pending / failed. vs16
    // marks a text-default symbol forced to emoji presentation by U+FE0F —
    // the worker appends the selector so the colour form is drawn.
    const AtlasGlyph* ColorAtlasForLazy(char32_t cp, bool vs16);
    const std::vector<uint8_t>& ColorAtlasBits() const { return m_colorAtlas; }
    uint64_t ColorAtlasVersion() const { return m_colorAtlasVersion; }
    // Sub-rectangle packed on the most recent version bump (one glyph). The
    // GPU copy uploads only this region, never the whole 8 MB atlas.
    void ColorDirtyRect(int& x, int& y, int& w, int& h) const
    { x = m_colorDirtyX; y = m_colorDirtyY; w = m_colorDirtyW; h = m_colorDirtyH; }
    // Like AtlasFor but rasterizes missing glyphs on demand (box drawing,
    // non-ASCII terminal output) — bumps the atlas version so the GPU copy
    // refreshes. Returns null only when the atlas is full or the glyph is
    // unrenderable.
    const AtlasGlyph* AtlasForLazy(char32_t cp);
    const std::vector<uint8_t>& AtlasBits() const { return m_atlas; }
    uint64_t AtlasVersion() const { return m_atlasVersion; }
    float AtlasFontPx() const { return m_atlasFontPx; }

private:
    struct Raster
    {
        std::vector<uint8_t> alpha;   // w*h coverage
        int w = 0, h = 0;             // texture bounds
        float penX = 0, top = 0;      // cell box origin in raster space
        float advPx = 0, linePx = 0;  // cell box size in raster space
        float boundsLeft = 0, boundsTop = 0;
    };

    bool RasterizeGlyph(char32_t cp, float emPx, float padX, float padY, Raster& out);
    void ExtractPoints(const Raster& r, GlyphPoint* out, uint32_t glyphId,
                       char32_t cp);
    UINT16 IndexFor(char32_t cp, IDWriteFontFace** faceOut);

    bool FindFace(const std::wstring& family, IDWriteFontFace** out);
    // Fallback sweep for glyphs the active face lacks: every bundled /
    // downloaded face first (a Nerd Font dropped in fonts\ covers prompt
    // icons), then curated system symbol/CJK families. Built lazily,
    // invalidated by LoadAppFonts.
    void BuildFallbackFaces();

public:
    // True once a Private Use Area codepoint (nerd-font prompt icon) failed
    // every fallback — the app offers/fetches a symbols font in response.
    bool PuaGlyphMissing() const { return m_puaMiss; }
    void ClearPuaMissing() { m_puaMiss = false; }
    // Re-scan font dirs and drop every cached glyph/template so previously
    // substituted '?' glyphs re-rasterize against the new faces.
    void RefreshAfterFontChange();

private:

    ComPtr<IDWriteFactory> m_factory;
    ComPtr<IDWriteFontCollection1> m_appColl;   // bundled / downloaded fonts
    ComPtr<IDWriteFontFace> m_face;
    ComPtr<IDWriteFontFace> m_fallback;
    std::vector<ComPtr<IDWriteFontFace>> m_fallbackFaces;
    bool m_fallbacksBuilt = false;
    bool m_puaMiss = false;
    std::vector<std::wstring> m_fontDirs;       // for RefreshAfterFontChange
    std::wstring m_fontName;

    float m_advanceEm = 0.6f;
    float m_ascentEm = 0.75f;
    float m_lineEm = 1.2f;
    float m_rasterEm = 80.0f;

    std::unordered_map<char32_t, uint32_t> m_glyphIds;
    std::vector<GlyphPoint> m_points;
    uint32_t m_glyphCount = 0;
    uint64_t m_pointsVersion = 0;
    TemplateStyle m_style = TemplateStyle::Modern;

    bool AddAtlasGlyph(char32_t cp);   // rasterize + pack at m_atlasFontPx
    void InitBlankGlyph();             // glyph 0: parked grid, zero weight
    void UpdateFaceMetrics();          // advance/ascent/line from m_face
    void InvalidateTemplatesAndAtlas();

    std::vector<uint8_t> m_atlas;
    std::unordered_map<char32_t, AtlasGlyph> m_atlasGlyphs;
    float m_atlasFontPx = 0;
    uint64_t m_atlasVersion = 0;
    uint32_t m_atlasCurX = 1, m_atlasCurY = 1, m_atlasRowH = 0;

    // Colour-emoji atlas (RGBA, premultiplied) + its own packer cursor.
    // Direct2D/WIC + COM live entirely on a dedicated worker thread so the
    // render thread's COM apartment is never touched (that wedges DXGI
    // flip-model present). RasterizeColorGlyph dispatches to it and blocks
    // briefly the first time each emoji is seen (like lazy glyph raster).
    void EnsureColorInit();
    void ColorThreadMain();
    void PollColorResults();   // pack a finished glyph (render thread)

    bool m_colorInit = false;
    std::thread m_colorThread;
    std::mutex m_colMx;
    std::condition_variable m_reqCv;
    bool m_colStop = false;
    bool m_reqPending = false, m_resReady = false;
    char32_t m_reqCp = 0;
    bool m_reqVs16 = false;
    char32_t m_inFlight = 0;    // cp currently being rasterized (0 = idle)
    int m_reqBox = 0;
    std::vector<uint8_t> m_resRgba;
    int m_resW = 0, m_resH = 0;
    bool m_resOk = false;

    std::vector<uint8_t> m_colorAtlas;   // kAtlasW*kAtlasH*4
    std::unordered_map<char32_t, AtlasGlyph> m_colorGlyphs;
    uint64_t m_colorAtlasVersion = 0;
    uint32_t m_colorCurX = 1, m_colorCurY = 1, m_colorRowH = 0;
    int m_colorDirtyX = 0, m_colorDirtyY = 0, m_colorDirtyW = 0, m_colorDirtyH = 0;
};
