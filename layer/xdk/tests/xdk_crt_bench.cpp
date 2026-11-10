// Benchmark of the C runtime replacements (any360/xdk/crt.h): nanoseconds a call for
//   model    the reference model (crt_reference.h): the function's work one byte at a time, the slowest sane way;
//   first    the replacement as first written (the host's memmove / memset behind the same checks), kept here as
//            the fixed point later versions are measured against;
//   now      the library as it is.
// Size by size first (every call of a case the same size: the branches of a version all predicted), then the
// titles' mixes: for each title measured in game with any360_xdk_bench, calls whose sizes follow, in random order,
// the sizes that title really passes. The functions are every title's, so a change to crt.h is judged on all the
// mixes (kTitles): it is an improvement when `now` gets faster on them, none getting slower, with xdk_crt_tests
// still passing. docs/NATIVE_FUNCTIONS.md is the rule. A title measured adds its mix here.
//
//   xdk_crt_bench [repeats]     (the fastest of `repeats` runs of each case, default 9)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "any360/xdk/crt.h"
#include "any360/xdk/crt_reference.h"

namespace {

using any360::xdk::Memory;
namespace xdk = any360::xdk;

// Each version is measured as a function of its own, as a title's binding makes it (one function a bound address).
#if defined(__clang__) || defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE __declspec(noinline)
#endif

// The replacements as first written (timing only: the register's high half is not saved here).
NOINLINE bool FirstMemcpy(const Memory& m, uint32_t dest, uint32_t source, uint32_t size, uint32_t stack_pointer) {
  if (!xdk::IsPlainForwardCopy(dest, source, size) || !xdk::IsPlainMemory(dest, size) || !xdk::IsPlainMemory(source, size)) {
    return false;
  }
  const uint64_t saved = xdk::detail::ByteSwap64(dest);
  std::memcpy(m.Host(stack_pointer - 8), &saved, sizeof(saved));
  std::memmove(m.Host(dest), m.Host(source), size);
  return true;
}
NOINLINE bool FirstMemset(const Memory& m, uint32_t dest, uint32_t value, uint32_t size) {
  if (!xdk::IsPlainMemory(dest, size)) return false;
  std::memset(m.Host(dest), int(value & 0xFF), size);
  return true;
}

NOINLINE bool NowMemcpy(const Memory& m, uint32_t dest, uint32_t source, uint32_t size, uint32_t stack_pointer) {
  return xdk::CrtMemcpy(m, dest, source, size, stack_pointer);
}
NOINLINE bool NowMemset(const Memory& m, uint32_t dest, uint32_t value, uint32_t size) {
  return xdk::CrtMemset(m, dest, value, size);
}

constexpr uint32_t kArena = 64u << 20, kStack = kArena - 4096;

struct Call {
  uint32_t dest, source, size;
};

struct Random {
  uint32_t state;
  explicit Random(uint32_t seed) : state(seed * 2654435761u + 1) {}
  uint32_t Next() {
    state ^= state << 13, state ^= state >> 17, state ^= state << 5;
    return state;
  }
};

// Calls of one size at varied alignments: small sizes inside a region that stays in the cache (as a game's hot
// copies are), large ones spread over the arena.
std::vector<Call> CallsOfSize(uint32_t size, uint32_t seed) {
  std::vector<Call> calls(1024);
  Random random(seed);
  const uint32_t span = size <= 4096 ? (256u << 10) : kStack - 2 * size - 4096;
  for (Call& c : calls) {
    c.size = size;
    c.dest = 4096 + random.Next() % (span / 2 - size);
    c.source = 4096 + span / 2 + size + random.Next() % (span / 2 - size);
  }
  return calls;
}

// A title's calls by size: sizes `low` to `high` (one size when equal, any of them otherwise) and how many calls
// of each function had them, as any360_xdk_bench logs them: every size with 0.5% of a function's calls or more on
// its own, the others by power of two (sizes above 64 KB left out: under 0.01% of the calls).
struct Share {
  uint32_t low, high;
  uint32_t memcpy_calls, memset_calls;
};

// Test Drive Unlimited over its first 100 seconds (a gamepad connected: boot, title screen and the scenes after
// it). Half of its copies are 16 bytes and a third 64 bytes; two fills in three are 120 bytes.
const Share kTestDriveUnlimited[] = {
    {0, 0, 8642, 4}, {1, 1, 788, 2}, {2, 3, 4658, 6}, {4, 7, 22779, 15}, {8, 8, 184830, 0}, {8, 15, 1110, 339},
    {12, 12, 81485, 0}, {16, 16, 6578412, 24408}, {16, 31, 7153, 259}, {32, 32, 97333, 12582}, {32, 63, 89696, 899},
    {44, 44, 0, 18216}, {48, 48, 1230079, 0}, {60, 60, 157905, 0}, {64, 64, 4270977, 83756}, {64, 127, 42010, 620},
    {88, 88, 0, 5064}, {96, 96, 202800, 0}, {120, 120, 0, 461980}, {128, 128, 0, 37408}, {128, 255, 40659, 3528},
    {192, 192, 0, 6319}, {256, 511, 77139, 15030}, {512, 1023, 33990, 7826}, {1024, 2047, 34713, 644},
    {2048, 4095, 7686, 2059}, {4096, 8191, 24793, 31917}, {8192, 16383, 8207, 9}, {16384, 32767, 138, 9},
    {32768, 65535, 5374, 27},
};

// Crackdown over its first 150 seconds (a gamepad connected: boot, title screen, then play in the city). More
// than a quarter of its copies are 104 bytes, a seventh 12 and a tenth 64; two fills in five are 16 bytes and a
// third 48.
const Share kCrackdown[] = {
    {0, 0, 213965, 55}, {1, 1, 98671, 207}, {2, 3, 50683, 631}, {4, 4, 952766, 256808}, {4, 7, 64993, 1717},
    {8, 8, 228358, 391181}, {8, 15, 153057, 23143}, {12, 12, 4267724, 260740}, {14, 14, 1221733, 0},
    {16, 16, 1536250, 16551984}, {16, 31, 291689, 216078}, {28, 28, 213116, 0}, {30, 30, 277288, 0},
    {32, 32, 158963, 1057132}, {32, 63, 245394, 174225}, {36, 36, 259266, 0}, {40, 40, 2084225, 378175},
    {44, 44, 332412, 358642}, {48, 48, 374771, 13441119}, {56, 56, 183096, 0}, {58, 58, 0, 969879},
    {60, 60, 797279, 0}, {64, 64, 2949918, 0}, {64, 127, 219073, 631369}, {74, 74, 0, 342985}, {80, 80, 0, 229344},
    {96, 96, 1450769, 0}, {104, 104, 8430032, 0}, {108, 108, 242262, 0}, {128, 128, 0, 378265},
    {128, 255, 176346, 253245}, {168, 168, 918299, 0}, {256, 511, 512051, 144567}, {512, 1023, 770788, 88553},
    {1024, 2047, 7748, 1813427}, {2048, 4095, 13796, 212818}, {4096, 8191, 6791, 83540}, {8192, 16383, 4421, 25477},
    {16384, 32767, 802, 701}, {32768, 65535, 1348, 689},
};

// The Simpsons Game over its first 130 seconds (a gamepad connected: boot, menus and a level). Half of its copies
// are 64 bytes and two in five 12 bytes; three fills in five are 64 bytes and one in five between 1 and 2 KB.
const Share kTheSimpsonsGame[] = {
    {0, 0, 9214, 20}, {1, 1, 10331, 104}, {2, 3, 6076, 47}, {4, 7, 141607, 7382}, {8, 15, 110846, 11987},
    {12, 12, 16868618, 0}, {16, 16, 0, 168195}, {16, 31, 175393, 14424}, {24, 24, 0, 665233},
    {32, 63, 484068, 120675}, {48, 48, 0, 703531}, {64, 64, 21506284, 5172732}, {64, 127, 173965, 49013},
    {84, 84, 962440, 0}, {112, 112, 449323, 0}, {128, 255, 83459, 43253}, {192, 192, 1462674, 0},
    {256, 511, 43456, 42900}, {512, 1023, 581187, 79163}, {1024, 2047, 141557, 1591318}, {2048, 4095, 8753, 27797},
    {4096, 8191, 1834, 25482}, {8192, 16383, 2917, 352}, {16384, 32767, 540, 171}, {32768, 65535, 460, 29},
};

// The titles measured so far: a change is judged on all of them.
struct Title {
  const char* name;
  const Share* shares;
  size_t count;
};
const Title kTitles[] = {
    {"Test Drive Unlimited", kTestDriveUnlimited, sizeof(kTestDriveUnlimited) / sizeof(kTestDriveUnlimited[0])},
    {"Crackdown", kCrackdown, sizeof(kCrackdown) / sizeof(kCrackdown[0])},
    {"The Simpsons Game", kTheSimpsonsGame, sizeof(kTheSimpsonsGame) / sizeof(kTheSimpsonsGame[0])},
};

// `count` calls whose sizes follow a title's mix, in random order, inside a region of a few megabytes.
std::vector<Call> CallsOfMix(const Title& title, bool memset, uint32_t count, uint32_t seed) {
  uint64_t total = 0;
  for (size_t i = 0; i < title.count; ++i) total += memset ? title.shares[i].memset_calls : title.shares[i].memcpy_calls;
  std::vector<Call> calls(count);
  Random random(seed);
  constexpr uint32_t kSpan = 4u << 20;
  for (Call& c : calls) {
    uint64_t pick = (uint64_t(random.Next()) << 16 ^ random.Next()) % total;
    const Share* share = title.shares;
    for (;; ++share) {
      const uint64_t n = memset ? share->memset_calls : share->memcpy_calls;
      if (pick < n) break;
      pick -= n;
    }
    c.size = share->low + random.Next() % (share->high - share->low + 1);
    c.dest = 4096 + random.Next() % (kSpan - c.size);
    c.source = 4096 + kSpan + c.size + random.Next() % (kSpan - c.size);
  }
  return calls;
}

volatile uint32_t g_sink = 0;

// One run over the calls, `rounds` times: nanoseconds a call.
template <typename F>
double Run(const std::vector<Call>& calls, uint32_t rounds, F&& f) {
  uint32_t done = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t round = 0; round < rounds; ++round) {
    for (const Call& c : calls) done += f(c);
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  g_sink = g_sink + done;
  return ns / (double(rounds) * double(calls.size()));
}

uint32_t Rounds(const std::vector<Call>& calls) {
  uint64_t bytes = 0;
  for (const Call& c : calls) bytes += c.size + 32;
  return uint32_t(std::clamp<uint64_t>((64ull << 20) / bytes, 1, 4000));
}

template <typename F>
double Fastest(const std::vector<Call>& calls, uint32_t repeats, F&& f) {
  const uint32_t rounds = Rounds(calls);
  double best = 1e30;
  for (uint32_t r = 0; r < repeats; ++r) best = std::min(best, Run(calls, rounds, f));
  return best;
}

// Two versions on the same calls, their runs alternating (so that neither has the machine's better moments).
template <typename F, typename G>
std::pair<double, double> FastestOfBoth(const std::vector<Call>& calls, uint32_t repeats, F&& f, G&& g) {
  const uint32_t rounds = Rounds(calls);
  double best_f = 1e30, best_g = 1e30;
  for (uint32_t r = 0; r < repeats; ++r) {
    best_f = std::min(best_f, Run(calls, rounds, f));
    best_g = std::min(best_g, Run(calls, rounds, g));
  }
  return {best_f, best_g};
}

}  // namespace

int main(int argc, char** argv) {
  const uint32_t repeats = argc > 1 ? uint32_t(std::atoi(argv[1])) : 9;
  std::vector<uint8_t> arena(kArena, 1);
  Memory m;
  m.base = arena.data();

  const auto copy_model = [&](const Call& c) {
    xdk::reference::Memcpy(m, c.dest, c.source, c.size, kStack);
    return 1u;
  };
  const auto copy_first = [&](const Call& c) { return uint32_t(FirstMemcpy(m, c.dest, c.source, c.size, kStack)); };
  const auto copy_now = [&](const Call& c) { return uint32_t(NowMemcpy(m, c.dest, c.source, c.size, kStack)); };
  const auto set_model = [&](const Call& c) {
    xdk::reference::Memset(m, c.dest, c.size, c.size);
    return 1u;
  };
  const auto set_first = [&](const Call& c) { return uint32_t(FirstMemset(m, c.dest, c.size, c.size)); };
  const auto set_now = [&](const Call& c) { return uint32_t(NowMemset(m, c.dest, c.size, c.size)); };

  std::printf("xdk_crt_bench: ns a call (fastest of %u runs)\n", repeats);
  std::printf("%9s | %9s %9s %9s %7s | %9s %9s %9s %7s\n", "size", "cpy model", "cpy first", "cpy now", "gain", "set model",
              "set first", "set now", "gain");
  const uint32_t sizes[] = {0,  1,  3,   4,   7,   8,    12,   16,   24,    32,    48,     64,
                            96, 128, 192, 256, 512, 1024, 4096, 16384, 65536, 1u << 20};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const uint32_t size = sizes[i];
    const std::vector<Call> calls = CallsOfSize(size, uint32_t(i) + 1);
    const double copy_slow = size > 65536 ? 0 : Fastest(calls, std::min(repeats, 3u), copy_model);
    const double set_slow = size > 65536 ? 0 : Fastest(calls, std::min(repeats, 3u), set_model);
    const auto [copy_a, copy_b] = FastestOfBoth(calls, repeats, copy_first, copy_now);
    const auto [set_a, set_b] = FastestOfBoth(calls, repeats, set_first, set_now);
    std::printf("%9u | %9.2f %9.2f %9.2f %6.2fx | %9.2f %9.2f %9.2f %6.2fx\n", size, copy_slow, copy_a, copy_b,
                copy_a / copy_b, set_slow, set_a, set_b, set_a / set_b);
  }

  // Each title's mix, three different draws of it; then all of them together (what a change is judged on).
  std::printf("the titles' mixes, sizes in random order (ns a call):\n");
  double all[2][3] = {};
  for (const Title& title : kTitles) {
    double sum[2][3] = {};
    for (uint32_t draw = 0; draw < 3; ++draw) {
      const std::vector<Call> copies = CallsOfMix(title, false, 16384, 100 + draw);
      const std::vector<Call> fills = CallsOfMix(title, true, 16384, 200 + draw);
      const double copy_slow = Fastest(copies, 2, copy_model), set_slow = Fastest(fills, 2, set_model);
      const auto [copy_a, copy_b] = FastestOfBoth(copies, repeats, copy_first, copy_now);
      const auto [set_a, set_b] = FastestOfBoth(fills, repeats, set_first, set_now);
      sum[0][0] += copy_slow / 3, sum[0][1] += copy_a / 3, sum[0][2] += copy_b / 3;
      sum[1][0] += set_slow / 3, sum[1][1] += set_a / 3, sum[1][2] += set_b / 3;
    }
    for (int f = 0; f < 2; ++f) {
      std::printf("  %-22s %s: model %.2f, first %.2f, now %.2f (%.2fx the first, %.1fx the model)\n", title.name,
                  f ? "memset" : "memcpy", sum[f][0], sum[f][1], sum[f][2], sum[f][1] / sum[f][2], sum[f][0] / sum[f][2]);
      for (int v = 0; v < 3; ++v) all[f][v] += sum[f][v];
    }
  }
  const size_t titles = sizeof(kTitles) / sizeof(kTitles[0]);
  for (int f = 0; f < 2; ++f) {
    std::printf("  all %zu title%s measured,  %s: first %.2f, now %.2f (%.2fx the first)\n", titles, titles == 1 ? "" : "s",
                f ? "memset" : "memcpy", all[f][1] / double(titles), all[f][2] / double(titles), all[f][1] / all[f][2]);
  }
  return int(g_sink & 0);
}
