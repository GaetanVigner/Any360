#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include "any360/sys/ldi.h"
#include "any360/sys/objects.h"
#include "lzx_cab.h"
using namespace any360::sys;
static std::vector<uint8_t> mem(1u << 24);
template <class F> double T(int n, F f) {
  double best = 1e9;
  for (int r = 0; r < 9; ++r) {
    auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) f();
    auto b = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count() / n);
  }
  return best;
}
int main() {
  Guest g{mem.data()};
  const uint32_t io = 0x1000, scratch = 0x4000, chunk = 0x8000, out = 0x10000;
  g.Store32(io, 0x8000), g.Store32(io + 4, 0x8000);
  Ldi().Create(g, io, io + 4, 0, 0, scratch, io + 8, io + 12, nullptr, nullptr);
  // A verbatim block that reads both trees (from the suite's LZX tests would be better; here an uncompressed one).
  const uint8_t stream[] = {0x00, 0x30, 0x60, 0x00, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 'L', 'Z', 'X', 'o', 'k', '!'};
  std::memcpy(g.host(chunk), stream, sizeof stream);
  for (int w : {15, 21}) {
    LzxDecoder d;
    d.Init(w);
    std::printf("LzxDecoder::Reset (window 2^%d)   %.1f\n", w, T(2000000, [&] { d.Reset(); }));
  }
  std::printf("Ldi().ResetDecompression        %.1f\n", T(2000000, [&] { Ldi().ResetDecompression(g, scratch); }));
  std::printf("Ldi().ResetDecompression (bad)  %.1f\n", T(2000000, [&] { Ldi().ResetDecompression(g, scratch + 16); }));
  // A compressed chunk (verbatim blocks: both trees read), window 2^15.
  std::vector<uint8_t> text;
  for (uint32_t i = 0; text.size() < 0x8000; ++i) { const char* w[] = {"the ", "frame ", "window ", "tree "}; text.insert(text.end(), w[i * 7 % 4], w[i * 7 % 4] + 4); text.push_back(uint8_t('a' + i % 26)); }
  text.resize(0x8000);
  const auto chunks = any360::test::cab::CabChunks(text, 15);
  const uint32_t zchunk = 0x20000;
  std::memcpy(g.host(zchunk), chunks[0].data.data(), chunks[0].data.size());
  bool ok = true;
  std::printf("reset + decompress (verbatim)   %.1f\n", T(20000, [&] {
    Ldi().ResetDecompression(g, scratch);
    g.Store32(io + 16, chunks[0].frame);
    ok &= Ldi().Decompress(g, scratch, zchunk, uint32_t(chunks[0].data.size()), out, io + 16) == 0;
  }));
  if (!ok || std::memcmp(g.host(out), text.data(), chunks[0].frame)) std::printf("  !! verbatim chunk differs\n");
  std::printf("reset + decompress (stored)     %.1f\n", T(200000, [&] {
    Ldi().ResetDecompression(g, scratch);
    g.Store32(io + 16, 6);
    Ldi().Decompress(g, scratch, chunk, sizeof stream, out, io + 16);
  }));
}
