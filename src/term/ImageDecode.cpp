#include "ImageDecode.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#pragma comment(lib, "windowscodecs.lib")

namespace amber
{

namespace
{
constexpr int kMaxDim = 4096;

struct Rgb { uint8_t r, g, b; };

Rgb HlsToRgb(float h, float l, float s)
{
    // Sixel HLS: hue 0..360 with 0 = blue (VT340 convention), l,s in 0..1.
    h = std::fmod(h + 240.0f, 360.0f) / 60.0f;
    float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; } else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    auto q = [&](float v) { return static_cast<uint8_t>(std::clamp((v + m) * 255.0f, 0.0f, 255.0f)); };
    return { q(r), q(g), q(b) };
}
} // namespace

bool DecodeSixel(const std::string& p, DecodedImage& out)
{
    // VT340 default palette.
    Rgb pal[256];
    static const Rgb kDefault[16] = {
        { 0, 0, 0 }, { 51, 51, 204 }, { 204, 36, 36 }, { 51, 204, 51 },
        { 204, 51, 204 }, { 51, 204, 204 }, { 204, 204, 51 }, { 135, 135, 135 },
        { 68, 68, 68 }, { 84, 84, 153 }, { 153, 68, 68 }, { 84, 153, 84 },
        { 153, 84, 153 }, { 84, 153, 153 }, { 153, 153, 84 }, { 204, 204, 204 },
    };
    for (int i = 0; i < 256; ++i)
        pal[i] = kDefault[i & 15];

    std::vector<uint8_t> buf;   // grows: RGBA rows
    int w = 0, h = 0;           // allocated size
    int x = 0, y = 0, maxX = 0, maxY = 0;
    int color = 0;
    auto ensure = [&](int nx, int ny) {
        if (nx <= w && ny <= h) return true;
        int nw = std::max(w, std::min(kMaxDim, std::max(nx, 64)));
        int nh = std::max(h, std::min(kMaxDim, std::max(ny, 64)));
        if (nw == w && nh == h) return false;
        std::vector<uint8_t> nb(static_cast<size_t>(nw) * nh * 4, 0);
        for (int row = 0; row < h; ++row)
            memcpy(&nb[static_cast<size_t>(row) * nw * 4], &buf[static_cast<size_t>(row) * w * 4],
                   static_cast<size_t>(w) * 4);
        buf.swap(nb);
        w = nw;
        h = nh;
        return true;
    };
    auto plot = [&](int px, int py) {
        if (px >= kMaxDim || py >= kMaxDim) return;
        if (!ensure(px + 1, py + 1)) return;
        uint8_t* d = &buf[(static_cast<size_t>(py) * w + px) * 4];
        d[0] = pal[color].r; d[1] = pal[color].g; d[2] = pal[color].b; d[3] = 255;
        maxX = std::max(maxX, px + 1);
        maxY = std::max(maxY, py + 1);
    };

    size_t i = 0;
    const size_t n = p.size();
    auto readNum = [&](int def) {
        if (i >= n || !isdigit(static_cast<unsigned char>(p[i]))) return def;
        int v = 0;
        while (i < n && isdigit(static_cast<unsigned char>(p[i])))
            v = v * 10 + (p[i++] - '0');
        return v;
    };
    while (i < n)
    {
        char ch = p[i];
        if (ch == '"')
        {
            // Raster attributes: aspect num;den;width;height — size hint only.
            ++i;
            readNum(1); if (i < n && p[i] == ';') ++i;
            readNum(1); if (i < n && p[i] == ';') ++i;
            int pw = readNum(0); if (i < n && p[i] == ';') ++i;
            int ph = readNum(0);
            if (pw > 0 && ph > 0) ensure(std::min(pw, kMaxDim), std::min(ph, kMaxDim));
            continue;
        }
        if (ch == '#')
        {
            ++i;
            int idx = readNum(0) & 255;
            if (i < n && p[i] == ';')
            {
                ++i;
                int sys = readNum(2); if (i < n && p[i] == ';') ++i;
                int a = readNum(0); if (i < n && p[i] == ';') ++i;
                int b = readNum(0); if (i < n && p[i] == ';') ++i;
                int c = readNum(0);
                if (sys == 1)
                    pal[idx] = HlsToRgb(static_cast<float>(a), b / 100.0f, c / 100.0f);
                else
                    pal[idx] = { static_cast<uint8_t>(a * 255 / 100), static_cast<uint8_t>(b * 255 / 100),
                                 static_cast<uint8_t>(c * 255 / 100) };
            }
            else
                color = idx;
            continue;
        }
        if (ch == '!')
        {
            ++i;
            int rep = std::min(readNum(1), kMaxDim);
            if (i < n && p[i] >= '?' && p[i] <= '~')
            {
                int bits = p[i] - '?';
                ++i;
                for (int r = 0; r < rep; ++r, ++x)
                    for (int bit = 0; bit < 6; ++bit)
                        if (bits & (1 << bit)) plot(x, y + bit);
            }
            continue;
        }
        if (ch == '$') { x = 0; ++i; continue; }
        if (ch == '-') { x = 0; y += 6; ++i; continue; }
        if (ch >= '?' && ch <= '~')
        {
            int bits = ch - '?';
            for (int bit = 0; bit < 6; ++bit)
                if (bits & (1 << bit)) plot(x, y + bit);
            ++x;
            ++i;
            continue;
        }
        ++i;   // whitespace / unknown
    }
    if (maxX == 0 || maxY == 0)
        return false;
    out.w = maxX;
    out.h = maxY;
    out.rgba.assign(static_cast<size_t>(maxX) * maxY * 4, 0);
    for (int row = 0; row < maxY; ++row)
        memcpy(&out.rgba[static_cast<size_t>(row) * maxX * 4], &buf[static_cast<size_t>(row) * w * 4],
               static_cast<size_t>(maxX) * 4);
    return true;
}

bool DecodePng(const uint8_t* data, size_t len, DecodedImage& out)
{
    using Microsoft::WRL::ComPtr;
    static ComPtr<IWICImagingFactory> factory;
    if (!factory)
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(len))))
        return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame)))
        return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > kMaxDim || h > kMaxDim)
        return false;
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
}

bool DecodeKittyRaw(const uint8_t* data, size_t len, int format, int w, int h, DecodedImage& out)
{
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim)
        return false;
    size_t bpp = (format == 24) ? 3 : 4;
    if (len < static_cast<size_t>(w) * h * bpp)
        return false;
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
    {
        out.rgba[i * 4 + 0] = data[i * bpp + 0];
        out.rgba[i * 4 + 1] = data[i * bpp + 1];
        out.rgba[i * 4 + 2] = data[i * bpp + 2];
        out.rgba[i * 4 + 3] = (bpp == 4) ? data[i * bpp + 3] : 255;
    }
    return true;
}

} // namespace amber
