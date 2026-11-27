// The tests' target of `bctrl` (ANY360_CPU_CALL_INDIRECT in the generated headers): a stand-in for the title's
// function table that leaves a mark of each call, the same in both versions of a function: r3 from the target and lr,
// a word stored in the data window, and the number of calls.
#pragma once

#include <cstdint>

#include "any360/xdk/ppc_model.h"
#include "cpu_space.h"

namespace any360::cpu::test {

inline uint64_t g_indirect_calls = 0;

inline void IndirectCall(any360::xdk::ppc_model::Ctx& ctx, const any360::xdk::Memory& base, uint32_t target) {
  ++g_indirect_calls;
  ctx.r3.u64 = DataWord(Hash(target, uint32_t(ctx.lr)));
  any360::xdk::ppc_model::Store32(base, kWindow + 0x40 + (target & 0xFC), uint32_t(ctx.lr));
}

}  // namespace any360::cpu::test

#define ANY360_CPU_CALL_INDIRECT(ctx, base, target) ::any360::cpu::test::IndirectCall(ctx, base, target)
