// zlib 1.1.4's inflate_fast (any360/xdk/zlib.h) against its model (any360/xdk/zlib_reference.h), byte for byte over
// the whole guest memory, on:
// - deflate streams of random symbols coded with random complete Huffman codes (dynamic blocks: tables with
//   sub-tables) and the fixed codes, from random window states (wrapping copies, the read pointer ahead of the write
//   pointer, bits left in the bit buffer from a part of a byte);
// - the same, handed back at every symbol in turn (the native one stops, the model goes on as the function's own code
//   would from the state left): the result must be the model's alone;
// - tables with corrupted entries and distances past the window (invalid codes, bits that are not there, links past
//   the mask table, copies of 0 to 2 bytes, distances of 0 and past address 0): handed back or declined, never
//   different;
// - the calls it declines, which must leave the memory untouched.
// The tables are built as zlib 1.1.4's huft_build builds them (8-byte entries, the same operation codes).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "any360/xdk/zlib.h"
#include "any360/xdk/zlib_reference.h"
#include "xdk_test.h"
#include "xdk_zlib_streams.h"

namespace {

namespace xdk = any360::xdk;
using namespace xdk_zlib;

const xdk::reference::InflateFastBuild kBuild{kMasks, kMsgLiteral, kMsgDistance, xdk_test::kArena};

uint64_t g_cases = 0, g_handed_back = 0, g_declined = 0, g_runaway = 0, g_wrapped = 0;

// One call both ways; `hand_back_at` forces the native one to stop there. False when the model's own run does not end.
bool RunCase(const Case& c, uint32_t seed, uint32_t hand_back_at = 0xFFFFFFFFu, xdk::InflateOutcome* outcome = nullptr) {
  xdk_test::Pair pair(seed);
  Place(pair.native, c);
  Place(pair.model, c);
  const xdk::InflateFastCall call{c.bl, c.bd, kTables + c.tl * 8, kTables + c.td * 8, kS, kZ};
  const uint32_t expected = xdk::reference::InflateFast(pair.b, call.bl, call.bd, call.tl, call.td, call.s, call.z, kBuild);
  if (expected == xdk::reference::kStopped) {
    ++g_runaway;
    return false;
  }
  uint32_t result = 0xBAD;
  const xdk::InflateOutcome o = xdk::InflateFast(pair.a, call, result, hand_back_at);
  if (outcome) *outcome = o;
  if (o != xdk::InflateOutcome::kDone) {
    // The function's own code goes on (from the state left, or from the start when declined).
    result = xdk::reference::InflateFast(pair.a, call.bl, call.bd, call.tl, call.td, call.s, call.z, kBuild);
    ++(o == xdk::InflateOutcome::kDeclined ? g_declined : g_handed_back);
  }
  // A handed-back run is the function's own only while its bit count does not wrap below 0 (zlib.h): a corrupted
  // entry that drops more bits than there are. The model's run says so in the bit count it leaves.
  const uint32_t left_bits = uint32_t(pair.model[kS + 28]) << 24 | uint32_t(pair.model[kS + 29]) << 16 |
                             uint32_t(pair.model[kS + 30]) << 8 | pair.model[kS + 31];
  if (o == xdk::InflateOutcome::kHandedOff && left_bits >= 0x80000000u) {
    ++g_wrapped;
    return false;
  }
  ++g_cases;
  CHECK(result == expected);
  CHECK(pair.SameEverywhere());
  if (result != expected || !pair.SameEverywhere()) {
    std::printf("  case seed %u, hand back at %u: outcome %u, result %08X expected %08X\n", seed, hand_back_at,
                uint32_t(o), result, expected);
    uint32_t first = 0, count = 0;
    for (uint32_t i = 0; i < xdk_test::kArena; ++i) {
      if (pair.native[i] != pair.model[i]) {
        if (!count++) first = i;
      }
    }
    std::printf("    %u bytes differ from %05X (native %02X model %02X); window %05X-%05X write %05X read %05X\n", count,
                first, pair.native[first], pair.model[first], kWindow, kWindow + c.window_size, kWindow + c.write,
                kWindow + c.read);
    return true;
  }
  return true;
}

void RandomWindow(Case& c, std::mt19937& rng) {
  c.window_size = (rng() % 2) ? 0x8000 : 0x200 << (rng() % 7);
  if (c.window_size < 600) c.window_size = 600;
  const uint32_t mode = rng() % 3;
  if (mode == 0) {  // writing up to the end
    c.write = rng() % (c.window_size - 258);
    c.read = rng() % (c.write + 1);
  } else if (mode == 1) {  // the read pointer ahead
    c.write = rng() % (c.window_size - 300);
    c.read = c.write + 259 + rng() % (c.window_size - c.write - 259);
  } else {
    c.write = c.window_size - 258 - rng() % 64;
    c.read = 0;
  }
}

}  // namespace

int main() {
  std::mt19937 rng(0x1140);
  // Valid streams, both kinds of block.
  for (uint32_t i = 0; i < 1500; ++i) {
    Case c = MakeCase(rng, i % 4 == 0, 20 + rng() % 400, 32768);
    RandomWindow(c, rng);
    RunCase(c, rng());
  }
  // zlib's own tables and states: the native one takes every call whole.
  CHECK(g_cases == 1500 && g_handed_back == 0 && g_declined == 0);
  const uint64_t valid = g_cases;
  // Handed back at every symbol of shorter streams.
  for (uint32_t i = 0; i < 40; ++i) {
    Case c = MakeCase(rng, i % 3 == 0, 30 + rng() % 40, 32768);
    RandomWindow(c, rng);
    const uint32_t seed = rng();
    for (uint32_t at = 0; at < 90; ++at) {
      xdk::InflateOutcome o;
      if (!RunCase(c, seed, at, &o)) break;
      if (o == xdk::InflateOutcome::kDone) break;  // it ended before that symbol
    }
  }
  // Corrupted tables and distances past the window.
  uint64_t corrupt = 0;
  for (uint32_t i = 0; i < 4000; ++i) {
    Case c = MakeCase(rng, i % 4 == 0, 20 + rng() % 200, 32768);
    RandomWindow(c, rng);
    const uint32_t changes = 1 + rng() % 4;
    for (uint32_t j = 0; j < changes; ++j) {
      Entry& e = c.tables[rng() % c.tables.size()];
      switch (rng() % 7) {
        case 0: e.exop = 128 + 64; break;                      // an invalid code
        case 1: e.bits = uint8_t(16 + rng() % 240); break;     // more bits than there are: the count wraps
        case 2: e.exop = uint8_t(17 + rng() % 15); break;      // a link past the mask table
        case 3: e.base = rng() % 3; break;                     // a copy of 0 to 2 bytes, a distance of 0 to 2
        case 4: e.base = 30000 + rng() % 400000; break;        // a long copy, a distance past the window
        case 5: e.base = 0xFFFFFFFFu - rng() % 64; break;      // past address 0
        default: e.exop = uint8_t(rng()); break;
      }
    }
    corrupt += RunCase(c, rng());
  }
  // Declined calls leave the memory untouched.
  {
    Case c = MakeCase(rng, false, 100, 32768);
    RandomWindow(c, rng);
    xdk_test::Pair pair(7);
    for (int variant = 0; variant < 6; ++variant) {
      Place(pair.native, c);
      xdk::InflateFastCall call{c.bl, c.bd, kTables + c.tl * 8, kTables + c.td * 8, kS, kZ};
      if (variant == 0) call.bl = 17;
      if (variant == 1) Store32(pair.native, kZ + 4, 9);                                 // under 10 bytes of input
      if (variant == 2) Store32(pair.native, kS + 52, kWindow + c.window_size - 257);   // under 258 bytes of room
      if (variant == 3) Store32(pair.native, kS + 44, kWindow);                         // an empty window
      if (variant == 4) Store32(pair.native, kS + 28, 32);                              // a bit count past the buffer
      if (variant == 5) Store32(pair.native, kZ + 0, 0x7EFFFFF8);                       // input reaching the GPU's registers
      const std::vector<uint8_t> before = pair.native;
      uint32_t result = 0;
      CHECK(xdk::InflateFast(pair.a, call, result) == xdk::InflateOutcome::kDeclined);
      CHECK(pair.native == before);
    }
  }
  std::printf("xdk_zlib_tests: inflate_fast: %llu calls compared (%llu valid streams taken whole; %llu handed back, %llu "
              "declined; %llu with corrupted tables); skipped: %llu model runs that do not end, %llu handed back with "
              "a wrapped bit count\n",
              (unsigned long long)g_cases, (unsigned long long)valid, (unsigned long long)g_handed_back,
              (unsigned long long)g_declined, (unsigned long long)corrupt, (unsigned long long)g_runaway,
              (unsigned long long)g_wrapped);
  return xdk_test::Finish("xdk_zlib_tests");
}
