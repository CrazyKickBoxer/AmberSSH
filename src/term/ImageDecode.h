// ImageDecode.h — inline terminal images: Sixel (DCS q) decoding and the
// PNG/raw payloads of the Kitty graphics protocol (APC G), both to
// straight-alpha RGBA8 for the renderer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amber
{

struct DecodedImage
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // w*h*4, straight alpha
};

// `payload` is the DCS body after the parameters and the 'q' introducer.
// Returns false for empty or malformed data.
bool DecodeSixel(const std::string& payload, DecodedImage& out);

// PNG (via WIC) — used for Kitty f=100.
bool DecodePng(const uint8_t* data, size_t len, DecodedImage& out);

// Kitty raw formats: f=24 (RGB) / f=32 (RGBA) with explicit width/height.
bool DecodeKittyRaw(const uint8_t* data, size_t len, int format, int w, int h,
                    DecodedImage& out);

} // namespace amber
