// PNG files of RGBA8 pixels, with no compression library: gfx_replay's frames, the binding's screenshots.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace any360::gfx {

// A PNG of RGBA8 pixels (R in the low byte) in stored (uncompressed) deflate blocks: no compression library needed.
inline bool WritePng(const std::string& path, uint32_t width, uint32_t height, const uint32_t* rgba) {
  static uint32_t crc_table[256];
  if (!crc_table[1]) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
    }
  }
  std::vector<uint8_t> raw;  // per row: filter byte 0, then the pixels
  raw.reserve(size_t(height) * (1 + size_t(width) * 4));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = reinterpret_cast<const uint8_t*>(rgba + size_t(y) * width);
    raw.insert(raw.end(), row, row + size_t(width) * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t at = 0;
  do {
    const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - at));
    const bool last = at + n == raw.size();
    z.insert(z.end(), {uint8_t(last), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(uint16_t(~n) >> 8)});
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
  } while (at < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = b << 16 | a;
  z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto be32 = [&](uint32_t v) {
    const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    std::fwrite(bytes, 1, 4, f);
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(uint32_t(data.size()));
    uint32_t crc = ~0u;
    auto feed = [&](const uint8_t* p, size_t n) {
      for (size_t i = 0; i < n; ++i) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    };
    std::fwrite(type, 1, 4, f);
    feed(reinterpret_cast<const uint8_t*>(type), 4);
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    feed(data.data(), data.size());
    be32(~crc);
  };
  const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(signature, 1, 8, f);
  chunk("IHDR", {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width), uint8_t(height >> 24),
                 uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 6, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  return std::fclose(f) == 0;
}

}  // namespace any360::gfx
