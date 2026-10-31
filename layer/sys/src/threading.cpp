#include "any360/sys/threading.h"

namespace any360::sys {

namespace {

void InitializeCriticalSection(Guest g, uint32_t cs, uint8_t spin_count_div_256) {
  g.Store8(cs + 0, 1);  // EventSynchronizationObject (auto reset)
  g.Store8(cs + 1, spin_count_div_256);
  g.Store32(cs + 4, 0);           // signal state
  g.Store32(cs + 0x10, 0xFFFFFFFF);  // lock count -1
  g.Store32(cs + 0x14, 0);        // recursion count
  g.Store32(cs + 0x18, 0);        // owning thread
}

}  // namespace

void RtlInitializeCriticalSection(Guest g, uint32_t cs) { InitializeCriticalSection(g, cs, 0); }

uint32_t RtlInitializeCriticalSectionAndSpinCount(Guest g, uint32_t cs, uint32_t spin_count) {
  // Rounded up to 256-spin intervals, at most 255 of them.
  uint32_t div = (spin_count + 255) >> 8;
  if (div > 255) div = 255;
  InitializeCriticalSection(g, cs, uint8_t(div));
  return 0;
}

uint32_t KeGetCurrentProcessType(Guest g, uint32_t pcr) {
  if (g.Load32(pcr + 0x100 + 0x50) != 0) return g.Load8(pcr + 0xC);
  return g.Load8(g.Load32(pcr + 0x100) + 0x73);
}

}  // namespace any360::sys
