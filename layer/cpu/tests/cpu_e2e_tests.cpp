// The CPU translator's end-to-end test: a title's functions as Any360 translates them from the XEX's words, against
// the same functions as the reference runtime recompiled them (tools/cpu_e2e.py), each pair run from the same
// register states on two guest spaces that start with the same memory; every register and every byte either version
// touched must match, a run that stops at its bound (steps or call depth) must stop at the same point in both, and
// both must call the runtime's hooks (global lock, time base, traps, imports: cpu_hooks.h) the same way, in order.
//
//   cpu_e2e_tests [states a function] [trace]
#include <cstdint>
#include <cstdio>
#include <string>

#include "cpu_e2e_steps.h"
#include "cpu_e2e_table.h"
#include "cpu_test.h"

using namespace any360::cpu::test;
namespace e2e = any360::cpu::e2e;

constexpr uint64_t kSteps = 1 << 14;

int main(int argc, char** argv) {
  const uint32_t states = argc > 1 ? uint32_t(std::stoul(argv[1])) : 64;
  const bool trace = argc > 2 && std::string(argv[2]) == "trace";
  Space translated_space(11), reference_space(11);
  const Memory translated_memory{translated_space.base()}, reference_memory{reference_space.base()};
  uint64_t instructions = 0, runs = 0, stopped = 0;
  int failed_functions = 0;
  for (const e2e::Pair& pair : e2e::kPairs) {
    int failed = 0;
    for (uint32_t s = 0; s < states; ++s) {
      if (trace) std::fprintf(stderr, "sub_%08X state %u\n", pair.address, s);
      const Ctx input = RandomState(pair.address ^ (s * 0x01000193u));
      Ctx translated = input, reference = input;
      bool translated_ends = false, reference_ends = false;
      const uint32_t seed = pair.address ^ s;
      ResetHooks(seed);
      RunFrom([&](Ctx& c) { translated_ends = RunWithin(kSteps, [&] { pair.translated(c, translated_memory); }); },
              translated);
      const uint64_t translated_hooks = g_hook_trace;
      ResetHooks(seed);
      RunFrom([&](Ctx& c) { reference_ends = RunWithin(kSteps, [&] { pair.reference(c, reference_memory); }); },
              reference);
      const std::string registers = Differences(translated, reference);
      uint32_t where = 0;
      const bool memory = MemoryDiffers(translated_space, reference_space, &where);
      const bool hooks = translated_hooks != g_hook_trace;
      if ((!registers.empty() || memory || hooks || translated_ends != reference_ends) && failed++ < 2) {
        std::printf("FAILED sub_%08X state %u:%s", pair.address, s, registers.c_str());
        if (memory) std::printf(" memory at %08X", where);
        if (hooks) std::printf(" runtime hooks");
        if (translated_ends != reference_ends) std::printf(" one version reached its bound");
        std::printf("\n");
      }
      stopped += !reference_ends;
      translated_space.Reset();
      reference_space.Reset();
      ++runs;
    }
    instructions += pair.instructions;
    if (failed) ++failed_functions;
  }
  const size_t pairs = sizeof e2e::kPairs / sizeof e2e::kPairs[0];
  std::printf("cpu_e2e_tests: %s, %zu functions (%llu instructions) of the %u both versions take, %llu runs "
              "(%llu stopped at %llu steps or %d calls deep), %llu indirect calls, %llu import calls, %llu traps, "
              "%llu global lock checks / enters / leaves %llu / %llu, %llu time base reads: %s\n",
              e2e::kTitle, pairs, (unsigned long long)instructions, e2e::kCandidates, (unsigned long long)runs,
              (unsigned long long)stopped, (unsigned long long)kSteps, kMaxDepth, (unsigned long long)g_indirect_calls,
              (unsigned long long)g_hook_counts.imports, (unsigned long long)g_hook_counts.traps,
              (unsigned long long)g_hook_counts.lock_checks, (unsigned long long)g_hook_counts.lock_enters,
              (unsigned long long)g_hook_counts.lock_leaves, (unsigned long long)g_hook_counts.timebase_reads,
              failed_functions ? (std::to_string(failed_functions) + " functions differ").c_str()
                               : "all match the reference");
  return failed_functions ? 1 : 0;
}
