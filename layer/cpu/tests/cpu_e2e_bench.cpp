// The CPU translator's benchmark: each function of the end-to-end table (tools/cpu_e2e.py), Any360's translation
// against the reference runtime's recompilation, on the same register states and memory. A timed run is kRounds
// passes over the states on one memory, each call seeing what the calls before it stored; every timed run starts from
// the same memory (Space::Refill), its pages already committed by an untimed run. The states kept are those whose
// every call of that sequence ends within the test's step budget (cpu_e2e_table.h); the times are of the versions
// without steps (cpu_e2e_plain_table.h). Prints a row a function (ns a call for both, speed = reference / Any360) and
// the totals, with the host allocations a call.
//
//   cpu_e2e_bench [repeats]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "cpu_e2e_plain_table.h"
#include "cpu_e2e_steps.h"
#include "cpu_e2e_table.h"
#include "cpu_test.h"

using namespace any360::cpu::test;
namespace e2e = any360::cpu::e2e;

namespace {

constexpr uint32_t kStates = 16, kRounds = 64;
constexpr uint64_t kSteps = 1 << 14;

double Time(any360::cpu::e2e_plain::Function f, const std::vector<Ctx>& inputs, const Memory& memory,
            any360::sys::AllocCounts* spent) {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t round = 0; round < kRounds; ++round) {
    for (const Ctx& input : inputs) {
      Ctx ctx = input;
      f(ctx, memory);
    }
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  const any360::sys::AllocCounts used = any360::sys::ThreadAllocations() - before;
  spent->bytes += used.bytes, spent->count += used.count;
  return ns / double(kRounds * inputs.size());
}

[[gnu::noinline]] void Empty(Ctx&, const Memory&) {}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::stoi(argv[1]) : 3;
  Space translated_space(11), reference_space(11);
  const Memory translated_memory{translated_space.base()}, reference_memory{reference_space.base()};
  std::printf("cpu_e2e_bench: %s, %zu functions, %u states x %u rounds, fastest of %d runs\n", e2e::kTitle,
              sizeof e2e::kPairs / sizeof e2e::kPairs[0], kStates, kRounds, repeats);
  {
    any360::sys::AllocCounts unused;
    std::vector<Ctx> inputs;
    for (uint32_t s = 0; s < kStates; ++s) inputs.push_back(RandomState(s));
    double empty = 1e30;
    for (int r = 0; r < repeats; ++r) empty = std::min(empty, Time(&Empty, inputs, translated_memory, &unused));
    std::printf("cpu_e2e_bench: baseline, an empty function with the same context copy and call: %.2f ns\n", empty);
  }
  std::printf("  %-12s %5s | %10s %10s | %7s\n", "function", "insns", "Any360 ns", "ReXGlue ns", "speed");
  double total_translated = 0, total_reference = 0, log_speed = 0;
  any360::sys::AllocCounts allocated;
  uint64_t calls = 0;
  size_t timed = 0;
  for (size_t i = 0; i < sizeof e2e::kPairs / sizeof e2e::kPairs[0]; ++i) {
    const e2e::Pair& pair = e2e::kPairs[i];
    const any360::cpu::e2e_plain::Pair& plain = any360::cpu::e2e_plain::kPairs[i];
    std::vector<Ctx> inputs;
    for (uint32_t s = 0; s < kStates; ++s) inputs.push_back(RandomState(pair.address ^ (s * 0x01000193u)));
    while (!inputs.empty()) {
      size_t stopped = inputs.size();
      for (uint32_t round = 0; round < kRounds && stopped == inputs.size(); ++round) {
        for (size_t k = 0; k < inputs.size(); ++k) {
          Ctx ctx = inputs[k];
          bool ends = false;
          RunFrom([&](Ctx& c) { ends = RunWithin(kSteps, [&] { pair.reference(c, reference_memory); }); }, ctx);
          if (!ends) {
            stopped = k;
            break;
          }
        }
      }
      reference_space.Reset();
      if (stopped == inputs.size()) break;
      inputs.erase(inputs.begin() + std::ptrdiff_t(stopped));
    }
    if (inputs.empty()) {
      std::printf("  sub_%08X %5u | no state ends within %llu steps\n", pair.address, pair.instructions,
                  (unsigned long long)kSteps);
      continue;
    }
    any360::sys::AllocCounts untimed;
    Time(plain.translated, inputs, translated_memory, &untimed);
    Time(plain.reference, inputs, reference_memory, &untimed);
    double translated = 1e30, reference = 1e30;
    for (int r = 0; r < repeats; ++r) {
      translated_space.Refill();
      translated = std::min(translated, Time(plain.translated, inputs, translated_memory, &allocated));
      reference_space.Refill();
      reference = std::min(reference, Time(plain.reference, inputs, reference_memory, &allocated));
      calls += 2ull * kRounds * inputs.size();
    }
    translated_space.Reset();
    reference_space.Reset();
    std::printf("  sub_%08X %5u | %10.2f %10.2f | %6.2fx\n", pair.address, pair.instructions, translated, reference,
                reference / translated);
    total_translated += translated, total_reference += reference;
    log_speed += std::log(reference / translated);
    ++timed;
  }
  const size_t n = timed;
  std::printf("cpu_e2e_bench: %zu functions timed\n", n);
  std::printf("cpu_e2e_bench: total a call of each function: Any360 %.1f ns, ReXGlue %.1f ns, speed %.3fx; "
              "geometric mean of the speeds %.3fx\n",
              total_translated, total_reference, total_reference / total_translated, std::exp(log_speed / double(n)));
  std::printf("cpu_e2e_bench: allocated per call %.2f B in %.4f allocations over %llu timed calls\n",
              calls ? double(allocated.bytes) / double(calls) : 0.0, calls ? double(allocated.count) / double(calls) : 0.0,
              (unsigned long long)calls);
  return 0;
}
