// The two sides of a library function bound to a native one, run by the verify and bench modes
// (--any360_xdk_verify, --any360_xdk_bench): what both sides write, the replacement on a copy of the registers, the
// first register the two sides leave different. Internal to the binding (xdk_natives.cpp); tested by
// host_xdk_sides_tests.
#pragma once

#include "any360/host/xdk_natives.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace any360::host::rexglue::xdk {

struct Sides {
  uint32_t f;
  std::vector<any360::xdk::Range> writes;
  uint32_t work;  // for the timings by size
  std::function<void(PPCContext&)> native;
  std::function<const char*(const PPCContext&, const PPCContext&)> differs;
  std::string call;  // the call's arguments, for a difference logged
};

// Verify: both sides on the call's registers and memory, compared; the function's own result stands. Bench: both
// timed in alternating order, the side that ran last stands; a replacement ten times slower than the function's own
// code runs once more on the call's entry registers, and the standing side's bytes are put back after it.
void RunSides(Mode mode, const Sides& s, PPCContext& ctx, uint8_t* base, GuestFunction original);

}  // namespace any360::host::rexglue::xdk
