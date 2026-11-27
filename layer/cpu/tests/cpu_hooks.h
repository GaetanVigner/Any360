// The tests' runtime behind the instructions the translator leaves to the including code: the global lock of
// mfmsr / mtmsrd (ANY360_CPU_CHECK_GLOBAL_LOCK, ANY360_CPU_ENTER_GLOBAL_LOCK, ANY360_CPU_LEAVE_GLOBAL_LOCK), the time
// base of mftb (ANY360_CPU_QUERY_TIMEBASE), traps (ANY360_CPU_TRAP) and calls to imports (ANY360_CPU_CALL_IMPORT).
// Stand-ins that keep their state per run (ResetHooks() before each version's run) and fold every call and its
// arguments into g_hook_trace, so two versions of a function compare by the hooks they called, in order. An import
// call leaves a mark like an indirect call's: r3 from the library, the ordinal and lr, a word stored in the data
// window.
#pragma once

#include <cstdint>

#include "any360/xdk/ppc_model.h"
#include "cpu_space.h"

namespace any360::cpu::test {

struct HookCounts {
  uint64_t lock_checks = 0, lock_enters = 0, lock_leaves = 0, timebase_reads = 0, traps = 0, imports = 0;
};

inline uint64_t g_hook_trace = 0;
inline int64_t g_lock_depth = 0;
inline uint64_t g_timebase = 0;
inline HookCounts g_hook_counts;

inline void FoldHook(uint32_t kind, uint64_t a, uint64_t b) {
  g_hook_trace = (g_hook_trace ^ (uint64_t(kind) << 56) ^ a) * 0x100000001B3ull;
  g_hook_trace = (g_hook_trace ^ b) * 0x100000001B3ull + kind;
}

inline void ResetHooks(uint32_t seed) {
  g_hook_trace = 0;
  g_lock_depth = 0;
  g_timebase = (uint64_t(Hash(seed, 0x7B)) << 24) | 0x123456;
}

inline uint64_t CheckGlobalLock() {
  ++g_hook_counts.lock_checks;
  FoldHook(1, uint64_t(g_lock_depth), 0);
  return g_lock_depth ? 0 : 0x8000;
}

inline void EnterGlobalLock() {
  ++g_hook_counts.lock_enters;
  FoldHook(2, uint64_t(++g_lock_depth), 0);
}

inline void LeaveGlobalLock() {
  ++g_hook_counts.lock_leaves;
  FoldHook(3, uint64_t(--g_lock_depth), 0);
}

inline uint64_t QueryTimebase() {
  ++g_hook_counts.timebase_reads;
  g_timebase += 0x9E3779B97Full;
  FoldHook(4, g_timebase, 0);
  return g_timebase;
}

inline void Trap(any360::xdk::ppc_model::Ctx& ctx, const any360::xdk::Memory&, uint32_t type) {
  ++g_hook_counts.traps;
  FoldHook(5, type, ctx.r3.u64);
}

inline uint32_t LibraryId(const char* library) {
  uint32_t h = 2166136261u;
  for (const char* c = library; *c; ++c) h = (h ^ uint8_t(*c)) * 16777619u;
  return h;
}

inline void ImportCall(any360::xdk::ppc_model::Ctx& ctx, const any360::xdk::Memory& base, const char* library,
                       uint32_t ordinal) {
  ++g_hook_counts.imports;
  const uint32_t id = LibraryId(library) ^ (ordinal * 0x9E3779B9u);
  FoldHook(6, id, ctx.lr);
  ctx.r3.u64 = DataWord(Hash(id, uint32_t(ctx.lr)));
  any360::xdk::ppc_model::Store32(base, kWindow + 0x200 + (id & 0xFC), uint32_t(ctx.lr) ^ ordinal);
}

}  // namespace any360::cpu::test

#define ANY360_CPU_CHECK_GLOBAL_LOCK() ::any360::cpu::test::CheckGlobalLock()
#define ANY360_CPU_ENTER_GLOBAL_LOCK() ::any360::cpu::test::EnterGlobalLock()
#define ANY360_CPU_LEAVE_GLOBAL_LOCK() ::any360::cpu::test::LeaveGlobalLock()
#define ANY360_CPU_QUERY_TIMEBASE() ::any360::cpu::test::QueryTimebase()
#define ANY360_CPU_TRAP(ctx, base, type) ::any360::cpu::test::Trap(ctx, base, type)
#define ANY360_CPU_CALL_IMPORT(ctx, base, library, ordinal) \
  ::any360::cpu::test::ImportCall(ctx, base, #library, ordinal)
