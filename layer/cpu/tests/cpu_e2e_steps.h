// A bound on the run of a translated or recompiled function on made-up data: the generated headers pass
// ANY360_CPU_STEP() at each function's entry and each label, and a run that passes more than its budget stops there.
// Both versions of a function pass the same steps.
#pragma once

#include <cstdint>

namespace any360::cpu::test {

struct OutOfSteps {};
inline uint64_t g_steps_left = 0;

template <typename F>
bool RunWithin(uint64_t steps, F&& f) {
  g_steps_left = steps;
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
