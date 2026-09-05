// RfbDecoders.h — the client-side framebuffer and the rectangle decoders:
// Raw, CopyRect, ZRLE, Hextile, and the Cursor pseudo-encoding.
//
// Every decoder writes 0xFFRRGGBB into the framebuffer — BGRA bytes with the
// alpha already opaque — so what leaves this file is what a BGRA texture
// takes, and nothing downstream has to know what the server sent. The pixel
// format this assumes is kRequestedFormat and nothing else: the client sends
// SetPixelFormat before its first FramebufferUpdateRequest, and a server that
// answers in a different format is refused before a rectangle is decoded.
//
// Decoders are fed a Reader over whatever bytes have arrived. They return
// NeedMore, consuming nothing past the last complete unit, when the bytes run
// out — a whole rectangle for Raw, CopyRect, Cursor and ZRLE (all of which
// know their length up front), a single 16x16 tile for Hextile (which does
// not), whose decoder keeps its place between calls. Bounds are checked
// before every write: a rectangle outside the framebuffer, a subrect outside
// its tile, a CopyRect source off the edge, an inflated stream longer than
// its tiles could be, all come back Bad and touch no pixel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RfbProtocol.h"

namespace amber::vnc
{

struct Rect
{
    uint16_t x = 0, y = 0, w = 0, h = 0;
};

// The decoded desktop. Owned by the network/decode worker; the render side
// only ever sees copies of dirty regions taken under a lock.
struct Framebuffer
{
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> px;   // 0xFFRRGGBB, row-major, no padding

    // False, and unchanged, when the size is outside the protocol limits.
    // Contents are black after a resize: the client asks for a full update.
    bool Resize(uint32_t w, uint32_t h);
    uint32_t* Row(uint32_t y) { return px.data() + static_cast<size_t>(y) * width; }
    const uint32_t* Row(uint32_t y) const { return px.data() + static_cast<size_t>(y) * width; }
};

// Rectangles that changed since the render side last took them. Bounded:
// past kMaxRects the list collapses to its bounding box, so a storm of tiny
// updates costs one upload of the union rather than unbounded bookkeeping.
struct DirtyRegion
{
    static constexpr size_t kMaxRects = 64;
    std::vector<Rect> rects;

    void Add(const Rect& r);
    void Clear() { rects.clear(); }
    bool Empty() const { return rects.empty(); }
    Rect Bounds() const;
};

enum class Decode
{
    Ok,
    NeedMore,
    Bad,
};

// §7.7.1: width*height pixels in the negotiated format.
Decode DecodeRaw(Reader& r, Framebuffer& fb, const Rect& rect);

// §7.7.2: a source position; the copy must handle overlap.
Decode DecodeCopyRect(Reader& r, Framebuffer& fb, const Rect& rect);

// §7.7.5. The zlib stream spans the whole connection: one decoder per
// connection, Reset() only when a connection starts, never per rectangle.
class ZrleDecoder
{
public:
    ZrleDecoder();
    ~ZrleDecoder();
    ZrleDecoder(const ZrleDecoder&) = delete;
    ZrleDecoder& operator=(const ZrleDecoder&) = delete;
    void Reset();
    Decode Decode(Reader& r, Framebuffer& fb, const Rect& rect);

private:
    struct Stream;
    Stream* m_z = nullptr;
    std::vector<uint8_t> m_out;   // the inflated tile stream of one rectangle
};

// §7.7.4. Tiles are 16x16 and carry no length, so this decoder is resumable:
// it remembers which tile it reached and the background/foreground colours
// that persist across tiles. BeginRect() before the first call for a
// rectangle; then call Decode again with more bytes after NeedMore.
class HextileDecoder
{
public:
    void BeginRect();
    Decode Decode(Reader& r, Framebuffer& fb, const Rect& rect);
    uint32_t TilesDone() const { return m_tile; }

private:
    uint32_t m_tile = 0;
    uint32_t m_bg = 0xFF000000u, m_fg = 0xFF000000u;
};

// §7.8.1. Pixels in the negotiated format, then a 1-bit-per-pixel mask with
// rows padded to a byte, most significant bit first, set = opaque. Modern
// cursors' soft edges arrive hardened; that is the protocol's limit.
struct CursorShape
{
    uint16_t width = 0, height = 0;   // 0x0 = the pointer is hidden
    uint16_t hotX = 0, hotY = 0;
    std::vector<uint32_t> bgra;       // 0xAARRGGBB with alpha 0 or 255
};
// The rectangle header's x,y are the hotspot, w,h the size.
Decode DecodeCursor(Reader& r, const RectHeader& header, CursorShape& out);

} // namespace amber::vnc
