#include "sampler.h"
#include "../term/charwidth.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

GlyphSampler::GlyphSampler() = default;

GlyphSampler::~GlyphSampler()
{
    if (m_colorThread.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(m_colMx);
            m_colStop = true;
        }
        m_reqCv.notify_one();
        m_colorThread.join();
    }
}

static uint32_t Hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// factory is unused today: the face is created from the collection. It stays in
// the signature because custom font-file loading will need it.
static bool CreateFaceFromFamily(IDWriteFactory* /*factory*/,
                                 IDWriteFontCollection* coll,
                                 const wchar_t* family,
                                 IDWriteFontFace** faceOut)
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(coll->FindFamilyName(family, &index, &exists)) || !exists)
        return false;

    ComPtr<IDWriteFontFamily> fam;
    if (FAILED(coll->GetFontFamily(index, &fam)))
        return false;
    ComPtr<IDWriteFont> font;
    if (FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, &font)))
        return false;
    return SUCCEEDED(font->CreateFontFace(faceOut));
}

// Resolve a family in the system collection first, then the private (bundled /
// downloaded) collection.
bool GlyphSampler::FindFace(const std::wstring& family, IDWriteFontFace** out)
{
    ComPtr<IDWriteFontCollection> sys;
    if (SUCCEEDED(m_factory->GetSystemFontCollection(&sys, FALSE)) &&
        CreateFaceFromFamily(m_factory.Get(), sys.Get(), family.c_str(), out))
        return true;
    if (m_appColl &&
        CreateFaceFromFamily(m_factory.Get(), m_appColl.Get(), family.c_str(),
                             out))
        return true;
    return false;
}

bool GlyphSampler::HasFamily(const std::wstring& family)
{
    ComPtr<IDWriteFontFace> f;
    return FindFace(family, &f);
}

void GlyphSampler::LoadAppFonts(const std::vector<std::wstring>& dirs)
{
    ComPtr<IDWriteFactory3> f3;
    if (FAILED(m_factory.As(&f3)))
        return;   // pre-Win10: only system fonts are available
    ComPtr<IDWriteFontSetBuilder> builder;
    if (FAILED(f3->CreateFontSetBuilder(&builder)))
        return;

    bool any = false;
    for (const std::wstring& dir : dirs)
    {
        for (const wchar_t* pat : { L"\\*.ttf", L"\\*.otf" })
        {
            WIN32_FIND_DATAW fd;
            HANDLE hh = FindFirstFileW((dir + pat).c_str(), &fd);
            if (hh == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                std::wstring path = dir + L"\\" + fd.cFileName;
                ComPtr<IDWriteFontFile> file;
                if (FAILED(f3->CreateFontFileReference(path.c_str(), nullptr,
                                                       &file)))
                    continue;
                BOOL supported = FALSE;
                DWRITE_FONT_FILE_TYPE ft;
                DWRITE_FONT_FACE_TYPE fct;
                UINT32 nFaces = 0;
                if (FAILED(file->Analyze(&supported, &ft, &fct, &nFaces)) ||
                    !supported)
                    continue;
                for (UINT32 i = 0; i < nFaces; ++i)
                {
                    ComPtr<IDWriteFontFaceReference> ref;
                    if (SUCCEEDED(f3->CreateFontFaceReference(
                            file.Get(), i, DWRITE_FONT_SIMULATIONS_NONE, &ref)))
                    {
                        builder->AddFontFaceReference(ref.Get());
                        any = true;
                    }
                }
            } while (FindNextFileW(hh, &fd));
            FindClose(hh);
        }
    }
    m_fontDirs = dirs;             // remembered for RefreshAfterFontChange
    m_fallbackFaces.clear();       // rebuilt lazily against the new set
    m_fallbacksBuilt = false;
    if (!any)
        return;
    ComPtr<IDWriteFontSet> set;
    if (FAILED(builder->CreateFontSet(&set)))
        return;
    m_appColl.Reset();
    f3->CreateFontCollectionFromFontSet(set.Get(), &m_appColl);
}

bool GlyphSampler::Init()
{
    ThrowIfFailed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(m_factory.GetAddressOf())),
                  "DWriteCreateFactory");

    ComPtr<IDWriteFontCollection> coll;
    ThrowIfFailed(m_factory->GetSystemFontCollection(&coll, FALSE),
                  "GetSystemFontCollection");

    // System JetBrains Mono if present, else Consolas. If neither is installed
    // the bundled JetBrains Mono is picked up once LoadAppFonts runs and the
    // app applies the saved face.
    if (CreateFaceFromFamily(m_factory.Get(), coll.Get(), L"JetBrains Mono", &m_face))
        m_fontName = L"JetBrains Mono";
    else if (CreateFaceFromFamily(m_factory.Get(), coll.Get(), L"Consolas", &m_face))
        m_fontName = L"Consolas";
    else
        return false;

    CreateFaceFromFamily(m_factory.Get(), coll.Get(),
                         m_fontName == L"Consolas" ? L"Segoe UI Symbol" : L"Consolas",
                         &m_fallback);

    UpdateFaceMetrics();

    InitBlankGlyph();
    ++m_pointsVersion;

    // Pre-rasterize printable ASCII so startup covers the common set.
    for (char32_t c = 33; c < 127; ++c)
        GlyphIdFor(c);
    // Box drawing + block elements (htop, vim UI chrome).
    for (char32_t c = 0x2500; c <= 0x257F; ++c)
        GlyphIdFor(c);
    for (char32_t c = 0x2580; c <= 0x259F; ++c)
        GlyphIdFor(c);
    return true;
}

void GlyphSampler::UpdateFaceMetrics()
{
    DWRITE_FONT_METRICS fm;
    m_face->GetMetrics(&fm);
    float dpe = static_cast<float>(fm.designUnitsPerEm);

    // Monospace advance from 'M'. For CJK system faces (NSimSun, MS Gothic)
    // this is the half-width Latin advance — the classic terminal look those
    // fonts are known for.
    UINT32 cp = 'M';
    UINT16 idx = 0;
    m_face->GetGlyphIndices(&cp, 1, &idx);
    DWRITE_GLYPH_METRICS gm = {};
    if (idx && SUCCEEDED(m_face->GetDesignGlyphMetrics(&idx, 1, &gm, FALSE)))
        m_advanceEm = gm.advanceWidth / dpe;
    else
        m_advanceEm = 0.6f;

    m_ascentEm = fm.ascent / dpe;
    m_lineEm = (fm.ascent + fm.descent + fm.lineGap) / dpe;
    if (m_lineEm < 1.0f) m_lineEm = 1.2f;

    // Raster em size: the character cell must fit inside the 64x128 canvas.
    m_rasterEm = std::min(62.0f / m_advanceEm, 124.0f / m_lineEm);
}

void GlyphSampler::InvalidateTemplatesAndAtlas()
{
    InitBlankGlyph();
    ++m_pointsVersion;
    // Force the next EnsureAtlas(fontPx) to rebuild regardless of size.
    m_atlasFontPx = -1.0f;
    m_atlasGlyphs.clear();
}

bool GlyphSampler::SetFontFamily(const std::wstring& family)
{
    if (family == m_fontName)
        return true;

    ComPtr<IDWriteFontFace> face;
    if (!FindFace(family, &face))   // system, then bundled / downloaded
        return false;

    m_face = face;
    m_fontName = family;
    // Keep a sensible fallback for glyphs the chosen face lacks (system).
    ComPtr<IDWriteFontCollection> sys;
    if (SUCCEEDED(m_factory->GetSystemFontCollection(&sys, FALSE)))
        CreateFaceFromFamily(
            m_factory.Get(), sys.Get(),
            family == L"Consolas" ? L"Segoe UI Symbol" : L"Consolas",
            &m_fallback);
    UpdateFaceMetrics();
    InvalidateTemplatesAndAtlas();
    return true;
}

void GlyphSampler::BuildFallbackFaces()
{
    m_fallbackFaces.clear();
    m_fallbacksBuilt = true;

    // Every bundled / downloaded face — a symbols/nerd font in fonts\ makes
    // prompt icons work no matter which terminal family is active.
    if (m_appColl)
    {
        UINT32 fams = m_appColl->GetFontFamilyCount();
        for (UINT32 i = 0; i < fams; ++i)
        {
            ComPtr<IDWriteFontFamily> fam;
            ComPtr<IDWriteFont> font;
            ComPtr<IDWriteFontFace> face;
            if (SUCCEEDED(m_appColl->GetFontFamily(i, &fam)) &&
                SUCCEEDED(fam->GetFont(0, &font)) &&
                SUCCEEDED(font->CreateFontFace(&face)))
                m_fallbackFaces.push_back(face);
        }
    }

    // Curated system families: symbols, mono-form emoji, CJK, Windows icons.
    static const wchar_t* kSysFallbacks[] = {
        L"Segoe UI Symbol", L"Segoe UI Emoji", L"Segoe Fluent Icons",
        L"Segoe MDL2 Assets", L"Cambria Math", L"Microsoft YaHei",
        L"Malgun Gothic", L"Yu Gothic", L"MS Gothic", L"Nirmala UI",
        L"Segoe UI Historic",
    };
    ComPtr<IDWriteFontCollection> sys;
    if (SUCCEEDED(m_factory->GetSystemFontCollection(&sys, FALSE)))
    {
        for (const wchar_t* fam : kSysFallbacks)
        {
            ComPtr<IDWriteFontFace> face;
            if (CreateFaceFromFamily(m_factory.Get(), sys.Get(), fam, &face))
                m_fallbackFaces.push_back(face);
        }
    }
}

UINT16 GlyphSampler::IndexFor(char32_t cp, IDWriteFontFace** faceOut)
{
    UINT32 u = static_cast<UINT32>(cp);
    UINT16 idx = 0;
    m_face->GetGlyphIndices(&u, 1, &idx);
    if (idx)
    {
        *faceOut = m_face.Get();
        return idx;
    }
    if (m_fallback)
    {
        m_fallback->GetGlyphIndices(&u, 1, &idx);
        if (idx)
        {
            *faceOut = m_fallback.Get();
            return idx;
        }
    }
    if (!m_fallbacksBuilt)
        BuildFallbackFaces();
    for (auto& face : m_fallbackFaces)
    {
        face->GetGlyphIndices(&u, 1, &idx);
        if (idx)
        {
            *faceOut = face.Get();
            return idx;
        }
    }
    // Nothing renders this codepoint. Nerd-font prompt icons live in the
    // Private Use Area — flag the miss so the app can fetch a symbols font.
    if ((cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0x10FFFD))
        m_puaMiss = true;
    u = '?';
    m_face->GetGlyphIndices(&u, 1, &idx);
    *faceOut = m_face.Get();
    return idx;
}

void GlyphSampler::RefreshAfterFontChange()
{
    LoadAppFonts(m_fontDirs);
    // New faces may cover previously substituted glyphs: throw away every
    // cached template/atlas entry so they re-rasterize.
    InvalidateTemplatesAndAtlas();
    m_puaMiss = false;
}

bool GlyphSampler::RasterizeGlyph(char32_t cp, float emPx, float padX, float padY,
                                  Raster& out)
{
    IDWriteFontFace* face = nullptr;
    UINT16 idx = IndexFor(cp, &face);
    if (!idx || !face)
        return false;

    float advPx = m_advanceEm * emPx;
    float linePx = m_lineEm * emPx;
    float ascPx = m_ascentEm * emPx;

    float penX = padX;
    float top = padY;
    float baseX = penX;
    float baseY = top + ascPx;

    FLOAT advance = advPx;
    DWRITE_GLYPH_OFFSET offset = {};
    DWRITE_GLYPH_RUN run = {};
    run.fontFace = face;
    run.fontEmSize = emPx;
    run.glyphCount = 1;
    run.glyphIndices = &idx;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;

    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    if (FAILED(m_factory->CreateGlyphRunAnalysis(
            &run, 1.0f, nullptr,
            DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            DWRITE_MEASURING_MODE_NATURAL,
            baseX, baseY, &analysis)))
        return false;

    RECT bounds = {};
    if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)))
        return false;

    int w = bounds.right - bounds.left;
    int h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0)
        return false;   // whitespace-like glyph

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds,
                                            rgb.data(), static_cast<UINT32>(rgb.size()))))
        return false;

    out.w = w;
    out.h = h;
    out.alpha.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.alpha.size(); ++i)
    {
        uint32_t a = rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2];
        out.alpha[i] = static_cast<uint8_t>(a / 3);
    }
    out.penX = penX;
    out.top = top;
    out.advPx = advPx;
    out.linePx = linePx;
    out.boundsLeft = static_cast<float>(bounds.left);
    out.boundsTop = static_cast<float>(bounds.top);
    return true;
}

// Classic 5x7 dot-matrix font, ASCII 32..126 — the letterforms real dot
// printers and character LCDs used. Dot styles render ASCII from this table
// instead of quantizing the outline font: quantizing narrow faces (NSimSun,
// MS Gothic Latin) onto a 6x8 pin grid destroys the letterforms. Rows top to
// bottom; bit 4 is the leftmost of 5 columns.
static const uint8_t kClassic5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // !
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, // "
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, // #
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // $
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // %
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // &
    {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, // '
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // (
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // )
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // *
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, // ,
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, // /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, // ;
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // <
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // =
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // >
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // ?
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, // @
    {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // [
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, // backslash
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ]
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // _
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // a
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, // b
    {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, // c
    {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, // d
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // e
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, // f
    {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, // g
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, // h
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // i
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, // j
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // k
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, // l
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, // m
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, // n
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // o
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, // p
    {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, // q
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, // r
    {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, // s
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, // t
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, // v
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, // x
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, // y
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, // z
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, // {
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, // |
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, // }
    {0x00,0x08,0x15,0x02,0x00,0x00,0x00}, // ~
};

// Samples the classic font on a DW x DH pin grid. Returns false when cp is
// outside the table (box drawing etc.), letting the caller fall back to
// quantizing the outline raster.
static bool ClassicDotOn(char32_t cp, int gx, int gy, int DW, int DH, bool& on)
{
    if (cp < 32 || cp > 126)
        return false;
    int tx = (DW == 6) ? gx : (gx * 6) / DW;
    int ty = (DH == 8) ? gy : (gy * 8) / DH;
    on = tx < 5 && ty < 7 &&
         ((kClassic5x7[cp - 32][ty] >> (4 - tx)) & 1) != 0;
    return true;
}

// Reverse the low 7 bits of v — a deterministic shuffle of the 128 grid slots
// so any density prefix (16, 32, 48...) still samples the whole glyph evenly.
static uint32_t BitRev7(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 7; ++i)
        r |= ((v >> i) & 1u) << (6 - i);
    return r;
}

void GlyphSampler::InitBlankGlyph()
{
    // Glyph 0: blank — every slot parked at its own grid-pixel center with
    // zero weight, so typing morphs each particle only a pixel or two. Slot
    // order must match ExtractPoints' bit-reversed shuffle.
    m_points.assign(kParticlesPerCell, GlyphPoint{});
    for (uint32_t i = 0; i < kParticlesPerCell; ++i)
    {
        uint32_t rev = 0;
        for (int b = 0; b < 7; ++b)
            rev |= ((i >> b) & 1u) << (6 - b);
        uint32_t gx = rev % 8, gy = rev / 8;
        m_points[i] = { (gx + 0.5f) / 8.0f, (gy + 0.5f) / 16.0f, 0.0f, 0.0f };
    }
    m_glyphCount = 1;
    m_glyphIds.clear();
    m_glyphIds[U' '] = 0;
    m_glyphIds[0] = 0;
}

void GlyphSampler::SetTemplateStyle(TemplateStyle s)
{
    if (s == m_style)
        return;
    m_style = s;
    // Throw away every template AND the atlas: the crisp-core pass renders
    // the same style (DirectWrite letterforms for Modern, hard round dots for
    // the dot-matrix styles), so both must regenerate together. Glyphs
    // re-rasterize lazily, and the id churn triggers the fly-in morph.
    InvalidateTemplatesAndAtlas();
}

void GlyphSampler::ExtractPoints(const Raster& r, GlyphPoint* out,
                                 uint32_t glyphId, char32_t cp)
{
    // Dot-matrix styles: quantize the glyph onto a classic printer pin grid
    // with hard on/off dots at exact dot centers — 8-pin (6x8) and 12-pin
    // (8x12). Remaining template slots repeat lit dots, so full density makes
    // each dot a solid bright bead.
    if (m_style != TemplateStyle::Modern)
    {
        const int DW = (m_style == TemplateStyle::DotMatrix8) ? 6 : 8;
        const int DH = (m_style == TemplateStyle::DotMatrix8) ? 8 : 12;
        const int dots = DW * DH;

        float sum[12 * 8] = {};
        int cnt[12 * 8] = {};
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
            {
                float nx = (r.boundsLeft + x + 0.5f - r.penX) / r.advPx;
                float ny = (r.boundsTop + y + 0.5f - r.top) / r.linePx;
                int gx = static_cast<int>(nx * DW);
                int gy = static_cast<int>(ny * DH);
                if (nx < 0.0f || gx < 0 || gx >= DW || ny < 0.0f || gy < 0 || gy >= DH)
                    continue;
                sum[gy * DW + gx] += r.alpha[static_cast<size_t>(y) * r.w + x] / 255.0f;
                ++cnt[gy * DW + gx];
            }

        // Deterministic shuffle of the dot order so density prefixes cover
        // the whole glyph.
        int order[12 * 8];
        for (int i = 0; i < dots; ++i)
            order[i] = i;
        for (int i = dots - 1; i > 0; --i)
        {
            int j = static_cast<int>(Hash32(glyphId * 977u + i * 31u) % (i + 1));
            std::swap(order[i], order[j]);
        }

        // Several template slots stack on each dot (128 / dots), so scale the
        // per-particle weight to keep total dot energy near a single full
        // particle — otherwise every glyph blows out to white.
        float dotWeight = 1.15f * static_cast<float>(dots) /
                          static_cast<float>(kParticlesPerCell);
        for (uint32_t k = 0; k < kParticlesPerCell; ++k)
        {
            int d = order[k % dots];
            int gx = d % DW, gy = d / DW;
            // ASCII uses the embedded classic dot font (legible with every
            // face); other glyphs quantize the outline raster.
            bool on;
            if (!ClassicDotOn(cp, gx, gy, DW, DH, on))
                on = cnt[d] > 0 && (sum[d] / cnt[d]) > 0.35f;
            out[k].x = (gx + 0.5f) / DW;
            out[k].y = (gy + 0.5f) / DH;
            out[k].w = on ? dotWeight : 0.0f;
            out[k].pad = 0.0f;
        }
        return;
    }

    // Bitmap-font style templates (after amber-particle-ssh): the glyph's
    // antialiased raster is box-filtered onto an 8x16 coverage grid — one
    // template slot per grid pixel, weight = coverage. Strokes come out
    // continuous like a rendered font instead of a scattered point cloud.
    constexpr int GW = 8, GH = 16;
    static_assert(GW * GH == static_cast<int>(kParticlesPerCell),
                  "grid must match the template stride");

    float sum[GW * GH] = {};
    int cnt[GW * GH] = {};
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
        {
            float nx = (r.boundsLeft + x + 0.5f - r.penX) / r.advPx;
            float ny = (r.boundsTop + y + 0.5f - r.top) / r.linePx;
            int gx = static_cast<int>(nx * GW);
            int gy = static_cast<int>(ny * GH);
            if (nx < 0.0f || gx < 0 || gx >= GW || ny < 0.0f || gy < 0 || gy >= GH)
                continue;
            sum[gy * GW + gx] += r.alpha[static_cast<size_t>(y) * r.w + x] / 255.0f;
            ++cnt[gy * GW + gx];
        }

    float weight[GW * GH];
    float maxW = 0.0f;
    for (int i = 0; i < GW * GH; ++i)
    {
        weight[i] = cnt[i] ? sum[i] / static_cast<float>(cnt[i]) : 0.0f;
        maxW = std::max(maxW, weight[i]);
    }
    // Per-glyph normalization keeps thin strokes as bright as heavy ones; a
    // mild gamma lifts the antialiased edges without fattening them.
    if (maxW > 0.05f)
        for (float& w : weight)
        {
            w = std::pow(std::clamp(w / maxW, 0.0f, 1.0f), 0.8f);
            if (w < 0.06f)
                w = 0.0f;
        }

    for (uint32_t k = 0; k < kParticlesPerCell; ++k)
    {
        uint32_t idx = BitRev7(k);
        uint32_t gx = idx % GW, gy = idx / GW;
        // Tiny static jitter (~±10% of a grid pixel) breaks up the mechanical
        // grid without smearing strokes — the reference uses ±5%.
        float jx = ((Hash32(glyphId * 977u + k * 7u + 1) & 1023) / 1023.0f - 0.5f) * 0.20f;
        float jy = ((Hash32(glyphId * 331u + k * 13u + 5) & 1023) / 1023.0f - 0.5f) * 0.20f;
        out[k].x = (gx + 0.5f + jx) / GW;
        out[k].y = (gy + 0.5f + jy) / GH;
        out[k].w = weight[gy * GW + gx];
        out[k].pad = 0.0f;
    }
}

uint32_t GlyphSampler::GlyphIdFor(char32_t cp)
{
    auto it = m_glyphIds.find(cp);
    if (it != m_glyphIds.end())
        return it->second;

    Raster r;
    float padX = (kRasterW - m_advanceEm * m_rasterEm) * 0.5f;
    float padY = (kRasterH - m_lineEm * m_rasterEm) * 0.5f;
    if (!RasterizeGlyph(cp, m_rasterEm, padX, padY, r))
    {
        // Whitespace or unrenderable: alias to blank.
        m_glyphIds[cp] = 0;
        return 0;
    }

    uint32_t id = m_glyphCount++;
    m_points.resize(static_cast<size_t>(m_glyphCount) * kParticlesPerCell);
    ExtractPoints(r, &m_points[static_cast<size_t>(id) * kParticlesPerCell], id,
                  cp);
    m_glyphIds[cp] = id;
    ++m_pointsVersion;
    return id;
}

bool GlyphSampler::AddAtlasGlyph(char32_t cp)
{
    Raster r;
    // Generous padding so bounds never clip.
    if (!RasterizeGlyph(cp, m_atlasFontPx, m_atlasFontPx, m_atlasFontPx, r))
    {
        AtlasGlyph g = {};
        g.advance = DesignAdvancePx(cp, m_atlasFontPx, m_advanceEm * m_atlasFontPx);
        m_atlasGlyphs[cp] = g;
        return true;   // whitespace-like: zero-size entry with an advance
    }

    // Dot-matrix styles: replace the DirectWrite raster with the glyph's own
    // dot pattern — the exact same pin-grid quantization the particle
    // templates use — rendered as crisp round dots. The core pass then draws
    // sharp dots and the particles glow on top of them.
    std::vector<uint8_t> dotBits;
    if (m_style != TemplateStyle::Modern)
    {
        const int DW = (m_style == TemplateStyle::DotMatrix8) ? 6 : 8;
        const int DH = (m_style == TemplateStyle::DotMatrix8) ? 8 : 12;

        float sum[12 * 8] = {};
        int cnt[12 * 8] = {};
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
            {
                float nx = (r.boundsLeft + x + 0.5f - r.penX) / r.advPx;
                float ny = (r.boundsTop + y + 0.5f - r.top) / r.linePx;
                int gx = static_cast<int>(nx * DW);
                int gy = static_cast<int>(ny * DH);
                if (nx < 0.0f || gx < 0 || gx >= DW || ny < 0.0f || gy < 0 || gy >= DH)
                    continue;
                sum[gy * DW + gx] += r.alpha[static_cast<size_t>(y) * r.w + x] / 255.0f;
                ++cnt[gy * DW + gx];
            }

        int cw = std::max(2, static_cast<int>(r.advPx + 0.5f));
        int ch = std::max(2, static_cast<int>(r.linePx + 0.5f));
        float pitchX = r.advPx / DW;
        float pitchY = r.linePx / DH;
        float radius = std::min(pitchX, pitchY) * 0.38f;

        dotBits.assign(static_cast<size_t>(cw) * ch, 0);
        for (int d = 0; d < DW * DH; ++d)
        {
            bool on;
            if (!ClassicDotOn(cp, d % DW, d / DW, DW, DH, on))
                on = cnt[d] > 0 && (sum[d] / cnt[d]) > 0.35f;
            if (!on)
                continue;
            float cx = (d % DW + 0.5f) * pitchX;
            float cy = (d / DW + 0.5f) * pitchY;
            int x0 = std::max(0, static_cast<int>(cx - radius - 1.0f));
            int x1 = std::min(cw - 1, static_cast<int>(cx + radius + 1.0f));
            int y0 = std::max(0, static_cast<int>(cy - radius - 1.0f));
            int y1 = std::min(ch - 1, static_cast<int>(cy + radius + 1.0f));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    float a = std::clamp((radius - dist + 0.5f), 0.0f, 1.0f);
                    uint8_t v = static_cast<uint8_t>(a * 255.0f + 0.5f);
                    uint8_t& px = dotBits[static_cast<size_t>(y) * cw + x];
                    px = std::max(px, v);
                }
        }

        // Repoint the raster at the dot bitmap: the pack path below is shared.
        r.alpha.swap(dotBits);
        r.w = cw;
        r.h = ch;
        r.boundsLeft = r.penX;   // dot bitmap covers the whole cell box
        r.boundsTop = r.top;
    }

    uint32_t w = static_cast<uint32_t>(r.w);
    uint32_t h = static_cast<uint32_t>(r.h);
    if (m_atlasCurX + w + 1 >= kAtlasW)
    {
        m_atlasCurX = 1;
        m_atlasCurY += m_atlasRowH + 2;
        m_atlasRowH = 0;
    }
    if (m_atlasCurY + h + 1 >= kAtlasH)
        return false;   // atlas full

    for (uint32_t y = 0; y < h; ++y)
        memcpy(&m_atlas[static_cast<size_t>(m_atlasCurY + y) * kAtlasW + m_atlasCurX],
               &r.alpha[static_cast<size_t>(y) * w], w);

    AtlasGlyph g;
    g.u0 = static_cast<float>(m_atlasCurX) / kAtlasW;
    g.v0 = static_cast<float>(m_atlasCurY) / kAtlasH;
    g.u1 = static_cast<float>(m_atlasCurX + w) / kAtlasW;
    g.v1 = static_cast<float>(m_atlasCurY + h) / kAtlasH;
    g.w = static_cast<float>(w);
    g.h = static_cast<float>(h);
    g.offX = r.boundsLeft - r.penX;
    g.offY = r.boundsTop - r.top;
    // The real advance, not the cell: the chrome atlas letters in proportional
    // faces, and a monospace face returns its cell here anyway.
    g.advance = DesignAdvancePx(cp, m_atlasFontPx, r.advPx);
    m_atlasGlyphs[cp] = g;

    m_atlasCurX += w + 2;
    m_atlasRowH = std::max(m_atlasRowH, h);
    return true;
}

void GlyphSampler::EnsureAtlas(float fontPx)
{
    if (m_atlasFontPx == fontPx && !m_atlas.empty())
        return;

    m_atlasFontPx = fontPx;
    m_atlas.assign(static_cast<size_t>(kAtlasW) * kAtlasH, 0);
    m_atlasGlyphs.clear();
    m_atlasCurX = 1;
    m_atlasCurY = 1;
    m_atlasRowH = 0;

    for (char32_t c = 33; c < 127; ++c)
        AddAtlasGlyph(c);
    AddAtlasGlyph(U'•');   // • password mask
    AddAtlasGlyph(U'█');   // █

    ++m_atlasVersion;

    // Colour emoji re-rasterize at the new cell size on next demand.
    m_colorGlyphs.clear();
    m_colorCurX = m_colorCurY = 1;
    m_colorRowH = 0;
    if (!m_colorAtlas.empty())
    {
        std::fill(m_colorAtlas.begin(), m_colorAtlas.end(), uint8_t(0));
        ++m_colorAtlasVersion;
    }
}

const GlyphSampler::AtlasGlyph* GlyphSampler::AtlasFor(char32_t cp) const
{
    auto it = m_atlasGlyphs.find(cp);
    return it == m_atlasGlyphs.end() ? nullptr : &it->second;
}

const GlyphSampler::AtlasGlyph* GlyphSampler::AtlasForLazy(char32_t cp)
{
    auto it = m_atlasGlyphs.find(cp);
    if (it != m_atlasGlyphs.end())
        return &it->second;
    if (m_atlas.empty())
        return nullptr;
    if (!AddAtlasGlyph(cp))
        return nullptr;
    ++m_atlasVersion;
    it = m_atlasGlyphs.find(cp);
    return it == m_atlasGlyphs.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------- colour emoji
bool GlyphSampler::IsColorEmoji(char32_t cp)
{
    // Unicode Emoji_Presentation table — text-default symbols (✔ ❤ ➜, box
    // drawing, braille, PUA icons) stay mono unless VS16 forces them.
    return IsEmojiPresentation(cp);
}

void GlyphSampler::EnsureColorInit()
{
    if (m_colorInit)
        return;
    m_colorInit = true;
    m_colorAtlas.assign(static_cast<size_t>(kAtlasW) * kAtlasH * 4, 0);
    m_colorThread = std::thread(&GlyphSampler::ColorThreadMain, this);
}

// Runs on a dedicated thread with its own COM apartment. Owns the Direct2D +
// WIC objects and does all colour-glyph drawing here, so the render thread's
// apartment (and DXGI present) is never disturbed.
void GlyphSampler::ColorThreadMain()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wicFactory;
    // The worker owns its OWN DirectWrite factory — sharing the render
    // thread's m_factory across threads corrupts glyph resolution there and
    // blanks all terminal text + particles. An ISOLATED factory never touches
    // the shared one's internal state.
    ComPtr<IDWriteFactory> dw;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    D2D1_FACTORY_OPTIONS opt = {};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                      &opt, reinterpret_cast<void**>(d2d.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&wicFactory));

    for (;;)
    {
        char32_t cp;
        int box;
        bool vs16;
        {
            std::unique_lock<std::mutex> lk(m_colMx);
            m_reqCv.wait(lk, [&] { return m_reqPending || m_colStop; });
            if (m_colStop)
                break;
            cp = m_reqCp;
            box = m_reqBox;
            vs16 = m_reqVs16;
            m_reqPending = false;
        }

        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool ok = false;
        if (d2d && wicFactory && dw)
        {
            ComPtr<IWICBitmap> wic;
            ComPtr<ID2D1RenderTarget> rt;
            ComPtr<IDWriteTextFormat> fmt;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(wicFactory->CreateBitmap(
                    box, box, GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapCacheOnDemand, &wic)) &&
                SUCCEEDED(d2d->CreateWicBitmapRenderTarget(
                    wic.Get(),
                    D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_DEFAULT,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    &rt)) &&
                SUCCEEDED(dw->CreateTextFormat(
                    L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    static_cast<float>(box) * 0.86f, L"en-us", &fmt)))
            {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);

                wchar_t text[4];
                int n = 0;
                if (cp > 0xFFFF)
                {
                    char32_t v = cp - 0x10000;
                    text[n++] = static_cast<wchar_t>(0xD800 + (v >> 10));
                    text[n++] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
                }
                else
                    text[n++] = static_cast<wchar_t>(cp);
                if (vs16)
                    text[n++] = 0xFE0F;   // force the colour presentation form
                text[n] = 0;

                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 0));
                rt->DrawText(text, static_cast<UINT32>(n), fmt.Get(),
                             D2D1::RectF(0, 0, static_cast<float>(box),
                                         static_cast<float>(box)),
                             brush.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                             DWRITE_MEASURING_MODE_NATURAL);
                if (SUCCEEDED(rt->EndDraw()))
                {
                    ComPtr<IWICBitmapLock> lock;
                    WICRect wr = { 0, 0, box, box };
                    if (SUCCEEDED(wic->Lock(&wr, WICBitmapLockRead, &lock)))
                    {
                        UINT stride = 0, size = 0;
                        BYTE* data = nullptr;
                        lock->GetStride(&stride);
                        lock->GetDataPointer(&size, &data);
                        if (data)
                        {
                            w = box;
                            h = box;
                            rgba.assign(static_cast<size_t>(box) * box * 4, 0);
                            for (int y = 0; y < box; ++y)
                            {
                                const BYTE* r0 = data +
                                                 static_cast<size_t>(y) * stride;
                                for (int x = 0; x < box; ++x)
                                {
                                    BYTE b = r0[x * 4 + 0], g = r0[x * 4 + 1];
                                    BYTE r = r0[x * 4 + 2], a = r0[x * 4 + 3];
                                    size_t o = (static_cast<size_t>(y) * box + x) * 4;
                                    rgba[o] = r; rgba[o + 1] = g;
                                    rgba[o + 2] = b; rgba[o + 3] = a;
                                    ok = ok || a != 0;
                                }
                            }
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_colMx);
            m_resRgba = std::move(rgba);
            m_resW = w;
            m_resH = h;
            m_resOk = ok;
            m_reqPending = false;
            m_resReady = true;   // render thread polls and packs
        }
    }
    CoUninitialize();
}

// Render thread: pack a finished emoji into the colour atlas (never blocks).
void GlyphSampler::PollColorResults()
{
    char32_t cp = 0;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(m_colMx);
        if (!m_resReady)
            return;
        cp = m_inFlight;
        rgba = std::move(m_resRgba);
        w = m_resW;
        h = m_resH;
        ok = m_resOk;
        m_resReady = false;
        m_inFlight = 0;          // worker is idle again
    }
    if (cp == 0)
        return;

    AtlasGlyph g = {};
    if (!ok || w <= 0 || h <= 0)
    {
        m_colorGlyphs[cp] = g;   // negative-cache
        return;
    }
    if (m_colorCurX + static_cast<uint32_t>(w) + 1 >= kAtlasW)
    {
        m_colorCurX = 1;
        m_colorCurY += m_colorRowH + 2;
        m_colorRowH = 0;
    }
    if (m_colorCurY + static_cast<uint32_t>(h) + 1 >= kAtlasH)
    {
        m_colorGlyphs[cp] = g;   // atlas full
        return;
    }
    for (int y = 0; y < h; ++y)
        memcpy(&m_colorAtlas[((static_cast<size_t>(m_colorCurY + y)) * kAtlasW +
                              m_colorCurX) * 4],
               &rgba[static_cast<size_t>(y) * w * 4],
               static_cast<size_t>(w) * 4);
    g.u0 = static_cast<float>(m_colorCurX) / kAtlasW;
    g.v0 = static_cast<float>(m_colorCurY) / kAtlasH;
    g.u1 = static_cast<float>(m_colorCurX + w) / kAtlasW;
    g.v1 = static_cast<float>(m_colorCurY + h) / kAtlasH;
    g.w = static_cast<float>(w);
    g.h = static_cast<float>(h);
    g.advance = m_advanceEm * m_atlasFontPx;
    m_colorGlyphs[cp] = g;
    m_colorDirtyX = static_cast<int>(m_colorCurX);
    m_colorDirtyY = static_cast<int>(m_colorCurY);
    m_colorDirtyW = w;
    m_colorDirtyH = h;
    m_colorCurX += static_cast<uint32_t>(w) + 2;
    m_colorRowH = std::max(m_colorRowH, static_cast<uint32_t>(h));
    ++m_colorAtlasVersion;
}

const GlyphSampler::AtlasGlyph* GlyphSampler::ColorAtlasForLazy(char32_t cp,
                                                                bool vs16)
{
    if (!IsColorEmoji(cp) && !vs16)
        return nullptr;
    auto it = m_colorGlyphs.find(cp);
    if (it != m_colorGlyphs.end())
        return it->second.w > 0.0f ? &it->second : nullptr;

    EnsureColorInit();
    PollColorResults();   // pack whatever the worker finished last frame

    it = m_colorGlyphs.find(cp);
    if (it != m_colorGlyphs.end())
        return it->second.w > 0.0f ? &it->second : nullptr;

    // Not cached yet: kick off one rasterization if the worker is idle. The
    // render thread never waits — the glyph appears a frame or two later.
    int box = std::max(8, static_cast<int>(std::lround(m_lineEm * m_atlasFontPx)));
    box = std::min(box, static_cast<int>(kAtlasH) - 4);
    std::unique_lock<std::mutex> lk(m_colMx);
    if (!m_reqPending && !m_resReady && m_inFlight == 0)
    {
        m_inFlight = cp;
        m_reqCp = cp;
        m_reqBox = box;
        m_reqVs16 = vs16;
        m_reqPending = true;
        lk.unlock();
        m_reqCv.notify_one();
    }
    return nullptr;
}

// The glyph's own advance in pixels from the face's design metrics, or
// `fallbackPx` when the codepoint is not in the primary face (a fallback
// face rasterised it, and the cell is the honest width for those).
float GlyphSampler::DesignAdvancePx(char32_t cp, float px, float fallbackPx)
{
    if (!m_face)
        return fallbackPx;
    UINT32 c = static_cast<UINT32>(cp);
    UINT16 idx = 0;
    if (FAILED(m_face->GetGlyphIndices(&c, 1, &idx)) || idx == 0)
        return fallbackPx;
    DWRITE_FONT_METRICS fm = {};
    m_face->GetMetrics(&fm);
    DWRITE_GLYPH_METRICS gm = {};
    if (fm.designUnitsPerEm == 0 ||
        FAILED(m_face->GetDesignGlyphMetrics(&idx, 1, &gm, FALSE)))
        return fallbackPx;
    return static_cast<float>(gm.advanceWidth) / fm.designUnitsPerEm * px;
}
