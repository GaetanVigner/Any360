// xboxkrnl.exe threading exports that work on the guest kernel structures the running thread owns: the processor
// control region r13 points at (KPCR: IRQL at +0x18, PRCB at +0x100 with the current KTHREAD at +0, DPC flag at +0x50,
// the process type to report during a DPC at +0xC), the KTHREAD (TLS block at +0x68, process type at +0x73, process at
// +0x84, APC disable count at +0xB0, host-order) and the KPROCESS (TLS slot bitmap at +0x30). Critical sections,
// spinlocks and IRQL follow the kernel's own in-memory protocol, so threads of the layer and of a static recompilation
// interoperate. Waiting on a contended object belongs to the scheduler, which comes later: the contended paths spin.
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

constexpr uint32_t kTlsSlots = 1024;  // the slots a thread gets when the title's XEX asks for none

// RTL_CRITICAL_SECTION (28 bytes): dispatcher header (type 1, spin count / 256 in the second byte, signal state 0),
// lock count -1 (host order), recursion count 0, no owner.
void RtlInitializeCriticalSection(Guest g, uint32_t cs);
uint32_t RtlInitializeCriticalSectionAndSpinCount(Guest g, uint32_t cs, uint32_t spin_count);
void RtlEnterCriticalSection(Guest g, uint32_t pcr, uint32_t cs);
uint32_t RtlTryEnterCriticalSection(Guest g, uint32_t pcr, uint32_t cs);
void RtlLeaveCriticalSection(Guest g, uint32_t pcr, uint32_t cs);

uint32_t KeGetCurrentProcessType(Guest g, uint32_t pcr);
void KeSetCurrentProcessType(Guest g, uint32_t pcr, uint32_t type);

uint32_t KfRaiseIrql(Guest g, uint32_t pcr, uint32_t new_irql);
uint32_t KeRaiseIrqlToDpcLevel(Guest g, uint32_t pcr);
void KfLowerIrql(Guest g, uint32_t pcr, uint32_t new_irql);
// A spinlock is one word holding the owner's PCR address (0 when free).
uint32_t KfAcquireSpinLock(Guest g, uint32_t pcr, uint32_t lock);
void KfReleaseSpinLock(Guest g, uint32_t pcr, uint32_t lock, uint32_t old_irql);
void KeAcquireSpinLockAtRaisedIrql(Guest g, uint32_t pcr, uint32_t lock);
uint32_t KeTryToAcquireSpinLockAtRaisedIrql(Guest g, uint32_t pcr, uint32_t lock);
void KeReleaseSpinLockFromRaisedIrql(Guest g, uint32_t lock);

void KeEnterCriticalRegion(Guest g, uint32_t pcr);
void KeLeaveCriticalRegion(Guest g, uint32_t pcr);

uint32_t KeTlsAlloc(Guest g, uint32_t pcr);
uint32_t KeTlsFree(Guest g, uint32_t pcr, uint32_t slot);
uint32_t KeTlsGetValue(Guest g, uint32_t pcr, uint32_t slot);
uint32_t KeTlsSetValue(Guest g, uint32_t pcr, uint32_t slot, uint32_t value);

// KDPC (0x1C bytes): type 19, selected and desired processor, a list entry, routine, context and two system arguments.
// Insert records the arguments and queues the DPC once (0 when already queued); Remove takes it off the queue. Queued
// DPCs run in order on a guest context of their own (a host thread started with the first one, at dispatch level, the
// PRCB's DPC-active flag set) as routine(dpc, context, arg1, arg2); the reference never runs them. Unlike the console
// they run beside the thread that queued them, not in its place.
void KeInitializeDpc(Guest g, uint32_t dpc, uint32_t routine, uint32_t context);
uint32_t KeInsertQueueDpc(Guest g, uint32_t dpc, uint32_t arg1, uint32_t arg2);
uint32_t KeRemoveQueueDpc(Guest g, uint32_t dpc);
// Stops the DPC thread (after the routine it is running) and empties the queue; the scheduler's Shutdown calls it.
void ResetDpcs();

uint32_t KeQueryPerformanceFrequency();  // 50 MHz, the Xenon time base

}  // namespace any360::sys
