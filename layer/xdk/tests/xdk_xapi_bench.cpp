// Benchmark of the XAPI replacements (any360/xdk/xapi.h): nanoseconds a call for
//   model    the reference model (xapi_reference.h): the function's own steps, a byte at a time;
//   first    the replacement as first written (the host's memmove behind the same test), the fixed point later
//            versions are measured against;
//   now      the library as it is.
// Size by size, then the mixes of sizes of the titles measured in game with any360_xdk_bench (kTitles): a change
// to xapi.h is judged on all of them, with xdk_xapi_tests still passing. docs/NATIVE_FUNCTIONS.md is the rule.
//
//   xdk_xapi_bench [repeats]     (the fastest of `repeats` runs of each case, default 9)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "any360/xdk/xapi.h"
#include "any360/xdk/xapi_reference.h"
#include "xdk_bench.h"

namespace {

using namespace xdk_bench;

using any360::xdk::Memory;
namespace xdk = any360::xdk;

NOINLINE bool FirstXMemCpy(const Memory& m, uint32_t dest, uint32_t source, uint32_t size) {
  if (!xdk::XMemCpyAccepts(dest, source, size, true)) return false;
  std::memmove(m.Host(dest), m.Host(source), size);
  return true;
}
NOINLINE bool NowXMemCpy(const Memory& m, uint32_t dest, uint32_t source, uint32_t size) {
  return xdk::XMemCpy(m, dest, source, size, true);
}

// A title's calls by size, as any360_xdk_bench logs them (sizes `low` to `high`, one size when equal).
struct Share {
  uint32_t low, high;
  uint32_t calls;
};
struct Title {
  const char* name;
  const Share* shares;
  size_t count;
};
// The Simpsons Game over its first 130 seconds (a gamepad connected: boot, menus and a level): 22.1 M calls, 97% of
// them between 1 and 2 KB.
const Share kTheSimpsonsGame[] = {
    {0, 0, 318944}, {8, 15, 668}, {32, 63, 1}, {64, 127, 36}, {128, 128, 31773}, {168, 168, 6542}, {172, 172, 13158},
    {128, 255, 23}, {256, 511, 68380}, {512, 1023, 130828}, {1024, 2047, 21552845}, {2048, 4095, 42},
    {4096, 8191, 24097},
};
// Crackdown from its boot to its title screen, 120 seconds: 61 k calls, a third each of 16 bytes, of 64 bytes and
// between 4 and 8 KB. (Test Drive Unlimited made 24 calls in 100 seconds: no mix.)
const Share kCrackdown[] = {
    {16, 16, 20324}, {64, 64, 20324}, {128, 255, 6}, {256, 511, 18}, {2048, 4095, 1}, {4096, 8191, 20324},
    {16384, 32767, 1},
};
const Title kTitles[] = {
    {"The Simpsons Game", kTheSimpsonsGame, sizeof(kTheSimpsonsGame) / sizeof(kTheSimpsonsGame[0])},
    {"Crackdown", kCrackdown, sizeof(kCrackdown) / sizeof(kCrackdown[0])},
};

std::vector<Call> CallsOfMix(const Title& title, uint32_t count, uint32_t seed) {
  uint64_t total = 0;
  for (size_t i = 0; i < title.count; ++i) total += title.shares[i].calls;
  std::vector<Call> calls(count);
  Random random(seed);
  constexpr uint32_t kSpan = 8u << 20;
  for (Call& c : calls) {
    uint64_t pick = (uint64_t(random.Next()) << 16 ^ random.Next()) % total;
    const Share* share = title.shares;
    for (; pick >= share->calls; ++share) pick -= share->calls;
    c.size = share->low + random.Next() % (share->high - share->low + 1);
    c.dest = 4096 + random.Next() % (kSpan - c.size);
    c.source = 4096 + kSpan + c.size + random.Next() % (kSpan - c.size);
  }
  return calls;
}

}  // namespace

int main(int argc, char** argv) {
  const uint32_t repeats = argc > 1 ? uint32_t(std::atoi(argv[1])) : 9;
  std::vector<uint8_t> arena(kArena, 1);
  Memory m;
  m.base = arena.data();
  const auto model = [&](const Call& c) {
    xdk::reference::XMemCpy(m, c.dest, c.source, c.size);
    return 1u;
  };
  const auto first = [&](const Call& c) { return uint32_t(FirstXMemCpy(m, c.dest, c.source, c.size)); };
  const auto now = [&](const Call& c) { return uint32_t(NowXMemCpy(m, c.dest, c.source, c.size)); };

  std::printf("xdk_xapi_bench: XMemCpy, ns a call (fastest of %u runs)\n", repeats);
  std::printf("%9s | %9s %9s %9s %7s\n", "size", "model", "first", "now", "gain");
  const uint32_t sizes[] = {0, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 512, 1024, 4096, 16384, 65536, 1u << 20};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const std::vector<Call> calls = CallsOfSize(sizes[i], uint32_t(i) + 1);
    const double slow = sizes[i] > 65536 ? 0 : Fastest(calls, std::min(repeats, 3u), model);
    const auto [a, b] = FastestOfBoth(calls, repeats, first, now);
    std::printf("%9u | %9.2f %9.2f %9.2f %6.2fx\n", sizes[i], slow, a, b, a / b);
  }
  std::printf("the titles' mixes, sizes in random order (ns a call):\n");
  double all[2] = {};
  for (const Title& title : kTitles) {
    double sum[3] = {};
    for (uint32_t draw = 0; draw < 3; ++draw) {
      const std::vector<Call> calls = CallsOfMix(title, 16384, 300 + draw);
      const double slow = Fastest(calls, 2, model);
      const auto [a, b] = FastestOfBoth(calls, repeats, first, now);
      sum[0] += slow / 3, sum[1] += a / 3, sum[2] += b / 3;
    }
    std::printf("  %-22s model %.2f, first %.2f, now %.2f (%.2fx the first, %.1fx the model)\n", title.name, sum[0], sum[1],
                sum[2], sum[1] / sum[2], sum[0] / sum[2]);
    all[0] += sum[1], all[1] += sum[2];
  }
  std::printf("  all the titles measured: %.2fx the first\n", all[0] / all[1]);
  return int(g_sink & 0);
}
