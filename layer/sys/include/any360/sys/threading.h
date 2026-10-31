// xboxkrnl.exe threading exports that only read or initialise guest structures: critical sections, the current
// process type (from the processor control region r13 points at). Scheduling itself comes later.
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// RTL_CRITICAL_SECTION (28 bytes): dispatcher header (type 1, spin count / 256 in the second byte, signal state 0),
// lock count -1, recursion count 0, no owner.
void RtlInitializeCriticalSection(Guest g, uint32_t cs);
uint32_t RtlInitializeCriticalSectionAndSpinCount(Guest g, uint32_t cs, uint32_t spin_count);

// KPCR layout: the PRCB at +0x100 (current thread at +0, dpc_active at +0x50), the process type to report during a
// DPC at +0xC; KTHREAD's process type at +0x73.
uint32_t KeGetCurrentProcessType(Guest g, uint32_t pcr);

}  // namespace any360::sys
