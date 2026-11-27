// A bound on the run of a translated or recompiled function on made-up data: the generated headers pass
// ANY360_CPU_STEP() at each function's entry and each label, and ANY360_CPU_ENTER() at each entry; a run that passes
// more steps than its budget, or nests calls deeper than kMaxDepth, stops there. Both versions of a function pass the
// same steps and calls.
#pragma once

#include <cstdint>

namespace any360::cpu::test {

struct OutOfSteps {};
inline uint64_t g_steps_left = 0;
inline int g_depth = 0;
constexpr int kMaxDepth = 256;

struct Depth {
  Depth() {
    if (++g_depth > kMaxDepth) {
      --g_depth;
      throw OutOfSteps{};
    }
  }
  ~Depth() { --g_depth; }
  Depth(const Depth&) = delete;
  Depth& operator=(const Depth&) = delete;
};

template <typename F>
bool RunWithin(uint64_t steps, F&& f) {
  g_steps_left = steps;
  g_depth = 0;
  try {
    f();
    return true;
  } catch (const OutOfSteps&) {
    return false;
  }
}

}  // namespace any360::cpu::test

#define ANY360_CPU_STEP()                                                                          \
  do {                                                                                             \
    if (--any360::cpu::test::g_steps_left == 0) throw any360::cpu::test::OutOfSteps{};             \
  } while (0)

#define ANY360_CPU_ENTER() const ::any360::cpu::test::Depth any360_cpu_depth
