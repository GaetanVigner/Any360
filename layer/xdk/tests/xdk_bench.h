// What the benchmarks of any360::xdk share: calls at varied addresses in a 64 MB arena and the timing of a version
// over them (the fastest of several runs, two versions' runs alternating).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

// Each version is measured as a function of its own, as a title's binding makes it (one function a bound address).
#if defined(__clang__) || defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE __declspec(noinline)
#endif

namespace xdk_bench {

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
inline std::vector<Call> CallsOfSize(uint32_t size, uint32_t seed) {
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

inline volatile uint32_t g_sink = 0;

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

inline uint32_t Rounds(const std::vector<Call>& calls) {
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

}  // namespace xdk_bench
