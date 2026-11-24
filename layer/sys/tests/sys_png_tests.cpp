// Inflate and the PNG decoder (any360/sys/png.h) against Python zlib's streams and hand-written PNGs
// (tests/png_vectors.h, tools/make_png_vectors.py): every byte of each output; malformed inputs refused; the glyph
// atlases of assets/glyphs decoded. --bench: a 1024x768 atlas decoded (time, host bytes and allocations a call).
//   sys_png_tests [--bench]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/png.h"
#include "png_vectors.h"

using namespace any360::sys;

namespace {

int g_failures = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("sys_png_tests: FAILED line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void InflateTests() {
  for (const png_vectors::Stream& s : png_vectors::kStreams) {
    std::vector<uint8_t> out = {0xAB};  // output is appended: what was there stays
    std::string error;
    const bool ok = ZlibInflate({s.z, s.z_size}, out, &error);
    if (!ok) std::printf("  %s: %s\n", s.name, error.c_str());
    CHECK(ok);
    CHECK(out.size() == s.size + 1 && out[0] == 0xAB);
    CHECK(out.size() == s.size + 1 && std::memcmp(out.data() + 1, s.data, s.size) == 0);
    // The raw deflate inside, alone.
    std::vector<uint8_t> raw;
    CHECK(Inflate({s.z + 2, s.z_size - 6}, raw) && raw.size() == s.size && std::memcmp(raw.data(), s.data, s.size) == 0);
    // Every truncation fails (never reads past the end, never claims success).
    for (uint32_t cut = 0; cut < s.z_size; cut += 1 + s.z_size / 40) {
      std::vector<uint8_t> o;
      CHECK(!ZlibInflate({s.z, cut}, o));
    }
    // A changed Adler-32 is caught.
    std::vector<uint8_t> bad(s.z, s.z + s.z_size), o;
    bad.back() ^= 1;
    CHECK(!ZlibInflate(bad, o, &error) && error.find("Adler") != std::string::npos);
  }
  std::vector<uint8_t> o;
  std::string error;
  const uint8_t not_deflate[] = {0x79, 0x9C, 0, 0, 0, 0};
  CHECK(!ZlibInflate(not_deflate, o, &error));
  const uint8_t bad_check[] = {0x78, 0x9D, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
  CHECK(!ZlibInflate(bad_check, o, &error) && error == "zlib: header check");
  const uint8_t block_type_3[] = {0x07};
  CHECK(!Inflate(block_type_3, o, &error) && error == "deflate: invalid block type");
  const uint8_t stored_bad_len[] = {0x01, 0x05, 0x00, 0x00, 0x00};  // LEN 5, NLEN not its complement
  CHECK(!Inflate(stored_bad_len, o, &error) && error == "deflate: stored block length check");
  // A fixed block whose first symbol is a match: a distance before the start.
  const uint8_t distance_too_far[] = {0x03, 0x02, 0x00};
  o.clear();
  CHECK(!Inflate(distance_too_far, o, &error));
}

void PngTests() {
  for (const png_vectors::Image& img : png_vectors::kImages) {
    RgbaImage out;
    std::string error;
    const bool ok = DecodePng({img.png, img.png_size}, out, &error);
    if (!ok) std::printf("  %s: %s\n", img.name, error.c_str());
    CHECK(ok);
    CHECK(out.width == img.width && out.height == img.height);
    CHECK(out.pixels.size() == size_t(img.width) * img.height &&
          std::memcmp(out.pixels.data(), img.rgba, out.pixels.size() * 4) == 0);
    // A byte changed anywhere after the signature: a CRC (or the data's own check) refuses it.
    for (uint32_t at = 8; at < img.png_size; at += 7) {
      std::vector<uint8_t> bad(img.png, img.png + img.png_size);
      bad[at] ^= 0x10;
      RgbaImage o;
      CHECK(!DecodePng(bad, o));
    }
    // Truncated anywhere.
    for (uint32_t cut = 0; cut < img.png_size; cut += 5) {
      RgbaImage o;
      CHECK(!DecodePng({img.png, cut}, o));
    }
  }
  // What is not handled says so.
  std::vector<uint8_t> f(png_vectors::color6_png, png_vectors::color6_png + sizeof(png_vectors::color6_png));
  auto rewrite_ihdr = [&](size_t offset, uint8_t value) {
    std::vector<uint8_t> g = f;
    g[16 + offset] = value;
    // The IHDR's CRC again, so only the field refuses it.
    uint32_t c = ~0u;
    for (size_t i = 12; i < 29; ++i) {
      c ^= g[i];
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    c = ~c;
    g[29] = uint8_t(c >> 24), g[30] = uint8_t(c >> 16), g[31] = uint8_t(c >> 8), g[32] = uint8_t(c);
    return g;
  };
  RgbaImage o;
  std::string error;
  CHECK(DecodePng(rewrite_ihdr(12, 0), o, &error));  // the same header, CRC recomputed: still fine
  CHECK(!DecodePng(rewrite_ihdr(12, 1), o, &error) && error == "png: interlaced images are not handled");
  CHECK(!DecodePng(rewrite_ihdr(8, 16), o, &error) && error == "png: only 8-bit samples are handled");
  CHECK(!DecodePng(rewrite_ihdr(9, 5), o, &error) && error == "png: bad colour type");
  const uint8_t not_png[] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
  CHECK(!DecodePng(not_png, o, &error) && error == "png: not a PNG file");
}

#ifdef ANY360_GLYPHS_DIR
void AtlasTests() {
  // Every atlas of the glyph sets decodes, at the size its index names (cells of 128 or 64 texels, 8 columns).
  for (const char* name : {"xbox360", "xbox_one", "ps3", "ps4", "ps5", "switch", "steam_controller", "steam_deck"}) {
    for (uint32_t cell : {128u, 64u}) {
      const std::string path = std::string(ANY360_GLYPHS_DIR) + "/" + name + "_" + std::to_string(cell) + ".png";
      const std::vector<uint8_t> file = ReadAll(path);
      RgbaImage img;
      std::string error;
      const bool ok = DecodePng(file, img, &error);
      if (!ok) std::printf("  %s: %s\n", path.c_str(), error.c_str());
      CHECK(ok);
      CHECK(img.width == 8 * cell && img.height == 6 * cell);
      // The first glyph (south) is drawn at its cell's centre, the atlas's corner is clear.
      CHECK(ok && (img.at(cell / 2, cell / 2) >> 24) == 0xFF);
      CHECK(ok && (img.at(0, 0) >> 24) == 0);
    }
  }
}
#endif

void Bench() {
#ifdef ANY360_GLYPHS_DIR
  const std::vector<uint8_t> file = ReadAll(std::string(ANY360_GLYPHS_DIR) + "/xbox_one_128.png");
  RgbaImage img;
  DecodePng(file, img);  // warm: the fixed tables, the image's buffers
  const int runs = 20;
  const AllocCounts before = ThreadAllocations();
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < runs; ++i) DecodePng(file, img);
  const auto t1 = std::chrono::steady_clock::now();
  const AllocCounts spent = ThreadAllocations() - before;
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / runs;
  std::printf("bench DecodePng(xbox_one_128.png, %zu bytes -> %ux%u): %.0f us/call, %.1f MB/s out, %llu bytes and %llu "
              "allocations/call (image buffers reused)\n",
              file.size(), img.width, img.height, us, img.pixels.size() * 4 / us, (unsigned long long)(spent.bytes / runs),
              (unsigned long long)(spent.count / runs));
#endif
}

}  // namespace

int main(int argc, char** argv) {
  InflateTests();
  PngTests();
#ifdef ANY360_GLYPHS_DIR
  AtlasTests();
#endif
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
  std::printf("sys_png_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
