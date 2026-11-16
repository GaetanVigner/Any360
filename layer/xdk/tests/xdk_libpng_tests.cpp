// libpng 1.0.5's png_read_filter_row (any360/xdk/libpng.h) against its model (any360/xdk/libpng_reference.h), byte
// for byte over the whole guest memory: every filter, the pixel depths PNG has (1 to 64 bits) and some it has not,
// rows of 0 to 5,000 bytes, rows that are smooth (what images give, the Paeth predictor's three ways all taken) and
// random; and the calls it declines, which must leave the memory untouched.
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "any360/xdk/libpng.h"
#include "any360/xdk/libpng_reference.h"
#include "xdk_test.h"

namespace {

namespace xdk = any360::xdk;

constexpr uint32_t kInfo = 0x1000, kRow = 0x10000, kPrev = 0x40000;

void Store32(std::vector<uint8_t>& m, uint32_t at, uint32_t v) {
  m[at] = uint8_t(v >> 24), m[at + 1] = uint8_t(v >> 16), m[at + 2] = uint8_t(v >> 8), m[at + 3] = uint8_t(v);
}

uint64_t g_compared = 0, g_declined = 0;

void Fill(std::vector<uint8_t>& m, uint32_t at, uint32_t size, bool smooth, std::mt19937& rng) {
  uint32_t v = rng() & 0xFF;
  for (uint32_t i = 0; i < size; ++i) {
    v = smooth ? (v + rng() % 7 - 3) & 0xFF : rng() & 0xFF;
    m[at + i] = uint8_t(v);
  }
}

void RunCase(uint32_t filter, uint32_t depth, uint32_t rowbytes, uint32_t row, uint32_t prev, bool smooth, uint32_t seed) {
  xdk_test::Pair pair(seed);
  std::mt19937 rng(seed);
  for (std::vector<uint8_t>* m : {&pair.native, &pair.model}) {
    Store32(*m, kInfo + 4, rowbytes);
    (*m)[kInfo + 11] = uint8_t(depth);
  }
  Fill(pair.native, row, rowbytes, smooth, rng);
  Fill(pair.native, prev, rowbytes, smooth, rng);
  pair.model = pair.native;
  const bool modelled = xdk::reference::PngReadFilterRow(pair.b, kInfo, row, prev, filter);
  const std::vector<uint8_t> before = pair.native;
  const bool taken = xdk::PngReadFilterRow(pair.a, kInfo, row, prev, filter);
  if (!taken) {
    ++g_declined;
    CHECK(pair.native == before);  // declined: untouched
    return;
  }
  ++g_compared;
  CHECK(modelled);  // it takes only what the function does without warning
  CHECK(pair.SameEverywhere());
}

}  // namespace

int main() {
  std::mt19937 rng(0x0105);
  const uint32_t depths[] = {1, 2, 4, 8, 16, 24, 32, 48, 64, 3, 7, 12, 255};
  for (uint32_t filter = 0; filter <= 4; ++filter) {
    for (uint32_t depth : depths) {
      for (int i = 0; i < 60; ++i) {
        const uint32_t rowbytes = i < 10 ? uint32_t(i) : rng() % (i < 40 ? 64 : 5000);
        const uint64_t before = g_declined;
        RunCase(filter, depth, rowbytes, kRow, kPrev, i % 2 == 0, rng());
        // Taken: every filter of 0 to 4, the rows apart, but Average and Paeth with bpp above rowbytes.
        CHECK((g_declined != before) == (filter >= 3 && (depth + 7) / 8 > rowbytes));
      }
    }
  }
  const uint64_t compared = g_compared, declined = g_declined;
  // Declined: filters above 4, a depth of 0, rows that overlap or reach the GPU's registers, bpp above rowbytes.
  const auto declines = [&](uint32_t filter, uint32_t depth, uint32_t rowbytes, uint32_t row, uint32_t prev) {
    const uint64_t before = g_declined;
    RunCase(filter, depth, rowbytes, row, prev, false, rng());
    return g_declined == before + 1;
  };
  CHECK(declines(5, 8, 100, kRow, kPrev));
  CHECK(declines(255, 8, 100, kRow, kPrev));
  CHECK(declines(1, 0, 100, kRow, kPrev));
  CHECK(declines(2, 8, 100, kRow, kRow + 50));
  CHECK(declines(4, 8, 100, kRow + 50, kRow));
  CHECK(declines(3, 32, 3, kRow, kPrev));
  CHECK(declines(4, 64, 7, kRow, kPrev));
  {
    xdk_test::Pair pair(3);
    Store32(pair.native, kInfo + 4, 64);
    pair.native[kInfo + 11] = 8;
    const std::vector<uint8_t> before = pair.native;
    CHECK(!xdk::PngReadFilterRow(pair.a, kInfo, 0x7EFFFFF0, kPrev, 2));  // a row reaching the GPU's registers
    CHECK(!xdk::PngReadFilterRow(pair.a, kInfo, kRow, 0x7EFFFFF0, 3));   // a previous row there
    CHECK(pair.native == before);
  }
  // Sub reads no previous row: a previous row that overlaps is not looked at.
  {
    const uint64_t before = g_compared;
    RunCase(1, 24, 300, kRow, kRow, true, rng());
    CHECK(g_compared == before + 1);
  }
  std::printf("xdk_libpng_tests: png_read_filter_row: %llu calls compared, %llu declined (Average and Paeth with bpp above "
              "rowbytes), then the declines asked for\n",
              (unsigned long long)compared, (unsigned long long)declined);
  return xdk_test::Finish("xdk_libpng_tests");
}
