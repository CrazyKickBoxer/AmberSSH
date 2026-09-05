// RfbDecoders.cpp — Raw, CopyRect, ZRLE, Hextile and Cursor. See the header.
#include "RfbDecoders.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace amber::vnc
{
namespace
{

constexpr uint32_t kOpaque = 0xFF000000u;

// A pixel in kRequestedFormat: BGRX bytes on the wire, 0xFFRRGGBB in memory.
inline uint32_t PixelFromWire(const uint8_t* p)
{
    return kOpaque | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

// ZRLE's CPIXEL for that format: depth 24 in 32 bits, little-endian, so the
// three least significant bytes — B, G, R (§7.7.5).
inline uint32_t CPixel(const uint8_t* p)
{
    return kOpaque | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

bool Within(const Rect& rect, const Framebuffer& fb)
{
    RectHeader h;
    h.x = rect.x; h.y = rect.y; h.w = rect.w; h.h = rect.h;
    return RectWithin(h, fb.width, fb.height);
}

void FillRect(Framebuffer& fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
    for (uint32_t row = 0; row < h; ++row)
    {
        uint32_t* dst = fb.Row(y + row) + x;
        std::fill(dst, dst + w, colour);
    }
}

// A run length in ZRLE: one or more bytes, each adding its value, 255
// meaning "another byte follows"; the run is one more than the sum.
bool ReadRun(Reader& t, uint32_t maxRun, uint32_t& run)
{
    run = 1;
    for (;;)
    {
        uint8_t b;
        if (!t.U8(b))
            return false;
        run += b;
        if (run > maxRun)
            return false;
        if (b != 255)
            return true;
    }
}

} // namespace

// ---- framebuffer -------------------------------------------------------------
bool Framebuffer::Resize(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension ||
        static_cast<uint64_t>(w) * h > kMaxPixels)
        return false;
    width = w;
    height = h;
    px.assign(static_cast<size_t>(w) * h, kOpaque);
    return true;
}

// ---- dirty region ------------------------------------------------------------
void DirtyRegion::Add(const Rect& r)
{
    if (r.w == 0 || r.h == 0)
        return;
    if (rects.size() >= kMaxRects)
    {
        Rect b = Bounds();
        const uint32_t x1 = std::min<uint32_t>(b.x, r.x), y1 = std::min<uint32_t>(b.y, r.y);
        const uint32_t x2 = std::max<uint32_t>(b.x + b.w, r.x + r.w);
        const uint32_t y2 = std::max<uint32_t>(b.y + b.h, r.y + r.h);
        rects.clear();
        rects.push_back({ static_cast<uint16_t>(x1), static_cast<uint16_t>(y1),
                          static_cast<uint16_t>(x2 - x1), static_cast<uint16_t>(y2 - y1) });
        return;
    }
    rects.push_back(r);
}

Rect DirtyRegion::Bounds() const
{
    if (rects.empty())
        return {};
    uint32_t x1 = 0xFFFF, y1 = 0xFFFF, x2 = 0, y2 = 0;
    for (const Rect& r : rects)
    {
        x1 = std::min<uint32_t>(x1, r.x);
        y1 = std::min<uint32_t>(y1, r.y);
        x2 = std::max<uint32_t>(x2, r.x + r.w);
        y2 = std::max<uint32_t>(y2, r.y + r.h);
    }
    return { static_cast<uint16_t>(x1), static_cast<uint16_t>(y1),
             static_cast<uint16_t>(x2 - x1), static_cast<uint16_t>(y2 - y1) };
}

// ---- Raw (§7.7.1) -------------------------------------------------------------
Decode DecodeRaw(Reader& r, Framebuffer& fb, const Rect& rect)
{
    if (!Within(rect, fb))
        return Decode::Bad;
    const size_t need = static_cast<size_t>(rect.w) * rect.h * 4;
    const uint8_t* p = r.Peek(need);
    if (!p)
        return Decode::NeedMore;
    for (uint32_t row = 0; row < rect.h; ++row)
    {
        uint32_t* dst = fb.Row(rect.y + row) + rect.x;
        const uint8_t* src = p + static_cast<size_t>(row) * rect.w * 4;
        for (uint32_t c = 0; c < rect.w; ++c)
            dst[c] = PixelFromWire(src + c * 4);
    }
    r.Skip(need);
    return Decode::Ok;
}

// ---- CopyRect (§7.7.2) ---------------------------------------------------------
Decode DecodeCopyRect(Reader& r, Framebuffer& fb, const Rect& rect)
{
    if (!Within(rect, fb))
        return Decode::Bad;
    const size_t start = r.Consumed();
    uint16_t sx, sy;
    if (!r.U16(sx) || !r.U16(sy))
    {
        r.Rewind(start);
        return Decode::NeedMore;
    }
    Rect src{ sx, sy, rect.w, rect.h };
    if (!Within(src, fb))
        return Decode::Bad;
    // Source and destination overlap routinely (a window dragged by a few
    // pixels). Rows are copied in the order that never reads a row already
    // overwritten, and within a row memmove takes care of the rest.
    if (rect.y > sy)
    {
        for (int32_t row = static_cast<int32_t>(rect.h) - 1; row >= 0; --row)
            std::memmove(fb.Row(rect.y + static_cast<uint32_t>(row)) + rect.x,
                         fb.Row(sy + static_cast<uint32_t>(row)) + sx, static_cast<size_t>(rect.w) * 4);
    }
    else
    {
        for (uint32_t row = 0; row < rect.h; ++row)
            std::memmove(fb.Row(rect.y + row) + rect.x, fb.Row(sy + row) + sx,
                         static_cast<size_t>(rect.w) * 4);
    }
    return Decode::Ok;
}

// ---- ZRLE (§7.7.5) --------------------------------------------------------------
struct ZrleDecoder::Stream
{
    z_stream z{};
    bool ok = false;
};

ZrleDecoder::ZrleDecoder()
{
    m_z = new Stream;
    m_z->ok = inflateInit(&m_z->z) == Z_OK;
}

ZrleDecoder::~ZrleDecoder()
{
    if (m_z->ok)
        inflateEnd(&m_z->z);
    delete m_z;
}

void ZrleDecoder::Reset()
{
    if (m_z->ok)
        inflateReset(&m_z->z);
    m_out.clear();
    m_out.shrink_to_fit();
}

Decode ZrleDecoder::Decode(Reader& r, Framebuffer& fb, const Rect& rect)
{
    if (!m_z->ok || !Within(rect, fb))
        return Decode::Bad;
    const size_t start = r.Consumed();
    uint32_t len;
    if (!r.U32(len))
    {
        r.Rewind(start);
        return Decode::NeedMore;
    }
    if (len > kMaxZrleRectBytes)
        return Decode::Bad;
    const uint8_t* body = r.Peek(len);
    if (!body)
    {
        r.Rewind(start);
        return Decode::NeedMore;
    }

    // What the tiles could possibly inflate to: a palette per tile plus, at
    // worst, four bytes per pixel (plain RLE of runs of one). An inflated
    // stream longer than that is not a rectangle, whatever it is.
    const uint32_t tilesX = (rect.w + 63) / 64, tilesY = (rect.h + 63) / 64;
    const size_t maxOut = static_cast<size_t>(tilesX) * tilesY * (1 + 128 * 3) +
                          static_cast<size_t>(rect.w) * rect.h * 4 + 64;

    z_stream& z = m_z->z;
    z.next_in = const_cast<Bytef*>(body);
    z.avail_in = len;
    m_out.clear();
    do
    {
        const size_t have = m_out.size();
        if (have >= maxOut)
            return Decode::Bad;
        m_out.resize(std::min(have + 64 * 1024, maxOut + 1));
        z.next_out = m_out.data() + have;
        z.avail_out = static_cast<uInt>(m_out.size() - have);
        const int rc = inflate(&z, Z_SYNC_FLUSH);
        if (rc == Z_STREAM_END)
            return Decode::Bad;   // the stream spans the connection; an end is corruption
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            return Decode::Bad;
        m_out.resize(m_out.size() - z.avail_out);
        if (rc == Z_BUF_ERROR && z.avail_in == 0)
            break;
    } while (z.avail_in > 0 || z.avail_out == 0);
    r.Skip(len);

    Reader t(m_out.data(), m_out.size());
    for (uint32_t ty = 0; ty < tilesY; ++ty)
    {
        for (uint32_t tx = 0; tx < tilesX; ++tx)
        {
            const uint32_t ox = rect.x + tx * 64, oy = rect.y + ty * 64;
            const uint32_t tw = std::min<uint32_t>(64, rect.w - tx * 64);
            const uint32_t th = std::min<uint32_t>(64, rect.h - ty * 64);
            const uint32_t count = tw * th;
            uint8_t sub;
            if (!t.U8(sub))
                return Decode::Bad;
            const bool rle = (sub & 0x80) != 0;
            const uint32_t pal = sub & 0x7F;
            uint32_t palette[128];

            if (!rle)
            {
                if (pal == 0)
                {
                    const uint8_t* p = t.Peek(static_cast<size_t>(count) * 3);
                    if (!p)
                        return Decode::Bad;
                    for (uint32_t row = 0; row < th; ++row)
                    {
                        uint32_t* dst = fb.Row(oy + row) + ox;
                        for (uint32_t c = 0; c < tw; ++c)
                            dst[c] = CPixel(p + (static_cast<size_t>(row) * tw + c) * 3);
                    }
                    t.Skip(static_cast<size_t>(count) * 3);
                }
                else if (pal == 1)
                {
                    const uint8_t* p = t.Peek(3);
                    if (!p)
                        return Decode::Bad;
                    FillRect(fb, ox, oy, tw, th, CPixel(p));
                    t.Skip(3);
                }
                else if (pal <= 16)
                {
                    const uint8_t* pp = t.Peek(static_cast<size_t>(pal) * 3);
                    if (!pp)
                        return Decode::Bad;
                    for (uint32_t i = 0; i < pal; ++i)
                        palette[i] = CPixel(pp + i * 3);
                    t.Skip(static_cast<size_t>(pal) * 3);
                    const uint32_t bits = pal == 2 ? 1 : pal <= 4 ? 2 : 4;
                    const uint32_t rowBytes = (tw * bits + 7) / 8;
                    const uint8_t* p = t.Peek(static_cast<size_t>(rowBytes) * th);
                    if (!p)
                        return Decode::Bad;
                    const uint32_t mask = (1u << bits) - 1;
                    for (uint32_t row = 0; row < th; ++row)
                    {
                        uint32_t* dst = fb.Row(oy + row) + ox;
                        const uint8_t* rp = p + static_cast<size_t>(row) * rowBytes;
                        for (uint32_t c = 0; c < tw; ++c)
                        {
                            const uint32_t bit = c * bits;
                            const uint32_t idx = (rp[bit / 8] >> (8 - bits - (bit % 8))) & mask;
                            if (idx >= pal)
                                return Decode::Bad;
                            dst[c] = palette[idx];
                        }
                    }
                    t.Skip(static_cast<size_t>(rowBytes) * th);
                }
                else
                    return Decode::Bad;   // 17..127: unused by the specification
            }
            else
            {
                if (pal == 1)
                    return Decode::Bad;   // 129: unused
                if (pal >= 2)
                {
                    const uint8_t* pp = t.Peek(static_cast<size_t>(pal) * 3);
                    if (!pp)
                        return Decode::Bad;
                    for (uint32_t i = 0; i < pal; ++i)
                        palette[i] = CPixel(pp + i * 3);
                    t.Skip(static_cast<size_t>(pal) * 3);
                }
                uint32_t done = 0;
                while (done < count)
                {
                    uint32_t colour, run;
                    if (pal == 0)
                    {
                        const uint8_t* p = t.Peek(3);
                        if (!p)
                            return Decode::Bad;
                        colour = CPixel(p);
                        t.Skip(3);
                        if (!ReadRun(t, count - done, run))
                            return Decode::Bad;
                    }
                    else
                    {
                        uint8_t idx;
                        if (!t.U8(idx))
                            return Decode::Bad;
                        const uint32_t index = idx & 0x7F;
                        if (index >= pal)
                            return Decode::Bad;
                        colour = palette[index];
                        run = 1;
                        if ((idx & 0x80) && !ReadRun(t, count - done, run))
                            return Decode::Bad;
                    }
                    // a run may wrap across tile rows
                    while (run > 0)
                    {
                        const uint32_t row = done / tw, col = done % tw;
                        const uint32_t n = std::min(run, tw - col);
                        uint32_t* dst = fb.Row(oy + row) + ox + col;
                        std::fill(dst, dst + n, colour);
                        done += n;
                        run -= n;
                    }
                }
            }
        }
    }
    return Decode::Ok;
}

// ---- Hextile (§7.7.4) ------------------------------------------------------------
void HextileDecoder::BeginRect()
{
    m_tile = 0;
    m_bg = m_fg = kOpaque;
}

Decode HextileDecoder::Decode(Reader& r, Framebuffer& fb, const Rect& rect)
{
    if (!Within(rect, fb))
        return Decode::Bad;
    const uint32_t tilesX = (rect.w + 15) / 16, tilesY = (rect.h + 15) / 16;
    const uint32_t total = tilesX * tilesY;
    while (m_tile < total)
    {
        const size_t tileStart = r.Consumed();
        const uint32_t tx = m_tile % tilesX, ty = m_tile / tilesX;
        const uint32_t ox = rect.x + tx * 16, oy = rect.y + ty * 16;
        const uint32_t tw = std::min<uint32_t>(16, rect.w - tx * 16);
        const uint32_t th = std::min<uint32_t>(16, rect.h - ty * 16);
        uint8_t sub;
        if (!r.U8(sub))
        {
            r.Rewind(tileStart);
            return Decode::NeedMore;
        }
        if (sub & 1)   // Raw
        {
            const size_t need = static_cast<size_t>(tw) * th * 4;
            const uint8_t* p = r.Peek(need);
            if (!p)
            {
                r.Rewind(tileStart);
                return Decode::NeedMore;
            }
            for (uint32_t row = 0; row < th; ++row)
            {
                uint32_t* dst = fb.Row(oy + row) + ox;
                for (uint32_t c = 0; c < tw; ++c)
                    dst[c] = PixelFromWire(p + (static_cast<size_t>(row) * tw + c) * 4);
            }
            r.Skip(need);
            ++m_tile;
            continue;
        }
        // everything else is known-length once the subrect count is read
        size_t need = 0;
        if (sub & 2) need += 4;   // BackgroundSpecified
        if (sub & 4) need += 4;   // ForegroundSpecified
        uint8_t nsub = 0;
        if (sub & 8)              // AnySubrects
        {
            const uint8_t* hdr = r.Peek(need + 1);
            if (!hdr)
            {
                r.Rewind(tileStart);
                return Decode::NeedMore;
            }
            nsub = hdr[need];
            need += 1 + static_cast<size_t>(nsub) * ((sub & 16) ? 6 : 2);
        }
        const uint8_t* p = r.Peek(need);
        if (!p)
        {
            r.Rewind(tileStart);
            return Decode::NeedMore;
        }
        const uint8_t* q = p;
        if (sub & 2) { m_bg = PixelFromWire(q); q += 4; }
        if (sub & 4) { m_fg = PixelFromWire(q); q += 4; }
        FillRect(fb, ox, oy, tw, th, m_bg);
        if (sub & 8)
        {
            ++q;   // the count, already read
            for (uint32_t i = 0; i < nsub; ++i)
            {
                uint32_t colour = m_fg;
                if (sub & 16)
                {
                    colour = PixelFromWire(q);
                    q += 4;
                }
                const uint32_t sx = q[0] >> 4, sy = q[0] & 15;
                const uint32_t sw = (q[1] >> 4) + 1, sh = (q[1] & 15) + 1;
                q += 2;
                if (sx + sw > tw || sy + sh > th)
                    return Decode::Bad;
                FillRect(fb, ox + sx, oy + sy, sw, sh, colour);
            }
        }
        r.Skip(need);
        ++m_tile;
    }
    return Decode::Ok;
}

// ---- Cursor (§7.8.1) --------------------------------------------------------------
Decode DecodeCursor(Reader& r, const RectHeader& header, CursorShape& out)
{
    if (header.w > kMaxCursorDim || header.h > kMaxCursorDim)
        return Decode::Bad;
    if (header.w == 0 || header.h == 0)
    {
        // a zero-sized shape hides the pointer; there is no body
        out = CursorShape{};
        return Decode::Ok;
    }
    const size_t rowBytes = (static_cast<size_t>(header.w) + 7) / 8;
    const size_t pixels = static_cast<size_t>(header.w) * header.h * 4;
    const size_t need = pixels + rowBytes * header.h;
    const uint8_t* p = r.Peek(need);
    if (!p)
        return Decode::NeedMore;
    const uint8_t* mask = p + pixels;
    out.width = header.w;
    out.height = header.h;
    out.hotX = header.x;
    out.hotY = header.y;
    out.bgra.resize(static_cast<size_t>(header.w) * header.h);
    for (uint32_t row = 0; row < header.h; ++row)
        for (uint32_t c = 0; c < header.w; ++c)
        {
            const uint32_t rgb = PixelFromWire(p + (static_cast<size_t>(row) * header.w + c) * 4) & 0x00FFFFFFu;
            const bool opaque = (mask[row * rowBytes + c / 8] >> (7 - (c % 8))) & 1;
            out.bgra[static_cast<size_t>(row) * header.w + c] = (opaque ? kOpaque : 0u) | rgb;
        }
    r.Skip(need);
    return Decode::Ok;
}

} // namespace amber::vnc
