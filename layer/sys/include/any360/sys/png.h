// PNG images read with no compression library: the glyph atlases of assets/glyphs and any art Any360 ships beside a
// title. Inflate (RFC 1951: stored, fixed and dynamic Huffman blocks) inside a zlib stream (RFC 1950, Adler-32
// checked), then PNG's chunks (CRC checked), the five row filters and the 8-bit colour types: grey, grey + alpha, RGB,
// RGBA, palette (with tRNS). 16-bit samples, lower bit depths and interlaced images are refused, with a message.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace any360::sys {

// Inflates a raw deflate stream (no zlib header) appended to `out`. False with `error` on malformed input.
bool Inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::string* error = nullptr);
// Inflates a zlib stream (2-byte header, deflate data, Adler-32 of the output) appended to `out`.
bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::string* error = nullptr);

// An image of RGBA8 pixels, R in the low byte of each uint32_t, rows from the top, alpha straight.
struct RgbaImage {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> pixels;
  uint32_t at(uint32_t x, uint32_t y) const { return pixels[size_t(y) * width + x]; }
};

// Decodes a PNG file's bytes into `out` (its buffers reused). False with `error` when the file is malformed or of a
// kind not handled (see above).
bool DecodePng(std::span<const uint8_t> file, RgbaImage& out, std::string* error = nullptr);

}  // namespace any360::sys
