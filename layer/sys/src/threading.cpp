#include "any360/sys/threading.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace any360::sys {

namespace {

constexpr uint32_t kPcrIrql = 0x18, kPcrDpcProcessType = 0xC, kPrcbCurrentThread = 0x100,
                   kPrcbDpcActive = 0x100 + 0x50;
constexpr uint32_t kThreadTls = 0x68, kThreadProcessType = 0x73, kThreadProcess = 0x84, kThreadApcDisable = 0xB0;
constexpr uint32_t kProcessType = 0x2F, kProcessTlsBitmap = 0x30;
constexpr uint32_t kCsLockCount = 0x10, kCsRecursion = 0x14, kCsOwner = 0x18;
constexpr uint8_t kIrqlDispatch = 2;

std::atomic<uint32_t>& Word(Guest g, uint32_t address) {
  return *reinterpret_cast<std::atomic<uint32_t>*>(g.host(address));
}

// The lock count of a critical section is kept in host order by the reference runtime's atomics.
std::atomic<int32_t>& LockCount(Guest g, uint32_t cs) {
  return *reinterpret_cast<std::atomic<int32_t>*>(g.host(cs + kCsLockCount));
}

uint32_t CurrentThread(Guest g, uint32_t pcr) { return g.Load32(pcr + kPrcbCurrentThread); }

uint32_t BigEndian(uint32_t v) { return std::byteswap(v); }

void InitializeCriticalSection(Guest g, uint32_t cs, uint8_t spin_count_div_256) {
  g.Store8(cs + 0, 1);  // EventSynchronizationObject (auto reset)
  g.Store8(cs + 1, spin_count_div_256);
  g.Store32(cs + 4, 0);  // signal state
  LockCount(g, cs).store(-1);
  g.Store32(cs + kCsRecursion, 0);
  g.Store32(cs + kCsOwner, 0);
}

void Acquire(Guest g, uint32_t pcr, uint32_t lock) {
  std::atomic<uint32_t>& word = Word(g, lock);
  const uint32_t self = BigEndian(pcr);
  for (;;) {
    uint32_t expected = 0;
    if (word.compare_exchange_strong(expected, self)) return;
    std::this_thread::yield();
  }
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

void RtlEnterCriticalSection(Guest g, uint32_t pcr, uint32_t cs) {
  const uint32_t thread = CurrentThread(g, pcr);
  std::atomic<int32_t>& count = LockCount(g, cs);
  if (g.Load32(cs + kCsOwner) == thread) {
    count.fetch_add(1);
    g.Store32(cs + kCsRecursion, g.Load32(cs + kCsRecursion) + 1);
    return;
  }
  for (;;) {
    int32_t expected = -1;
    if (count.compare_exchange_strong(expected, 0)) break;
    std::this_thread::yield();  // the scheduler's wait comes later
  }
  g.Store32(cs + kCsOwner, thread);
  g.Store32(cs + kCsRecursion, 1);
}

uint32_t RtlTryEnterCriticalSection(Guest g, uint32_t pcr, uint32_t cs) {
  const uint32_t thread = CurrentThread(g, pcr);
  std::atomic<int32_t>& count = LockCount(g, cs);
  int32_t expected = -1;
  if (count.compare_exchange_strong(expected, 0)) {
    g.Store32(cs + kCsOwner, thread);
    g.Store32(cs + kCsRecursion, 1);
    return 1;
  }
  if (g.Load32(cs + kCsOwner) == thread) {
    count.fetch_add(1);
    g.Store32(cs + kCsRecursion, g.Load32(cs + kCsRecursion) + 1);
    return 1;
  }
  return 0;
}

void RtlLeaveCriticalSection(Guest g, uint32_t, uint32_t cs) {
  std::atomic<int32_t>& count = LockCount(g, cs);
  const uint32_t recursion = g.Load32(cs + kCsRecursion) - 1;
  g.Store32(cs + kCsRecursion, recursion);
  if (recursion != 0) {
    count.fetch_sub(1);
    return;
  }
  g.Store32(cs + kCsOwner, 0);
  count.fetch_sub(1);  // a waiter, if any, is woken by the scheduler later
}

uint32_t KeGetCurrentProcessType(Guest g, uint32_t pcr) {
  if (g.Load32(pcr + kPrcbDpcActive) != 0) return g.Load8(pcr + kPcrDpcProcessType);
  return g.Load8(CurrentThread(g, pcr) + kThreadProcessType);
}

void KeSetCurrentProcessType(Guest g, uint32_t pcr, uint32_t type) {
  // The process of the current thread takes the type; during a DPC the PCR's own copy does too.
  const uint32_t process = g.Load32(CurrentThread(g, pcr) + kThreadProcess);
  if (process) g.Store8(process + kProcessType, uint8_t(type));
  if (g.Load32(pcr + kPrcbDpcActive) != 0) g.Store8(pcr + kPcrDpcProcessType, uint8_t(type));
}

uint32_t KfRaiseIrql(Guest g, uint32_t pcr, uint32_t new_irql) {
  const uint8_t old = g.Load8(pcr + kPcrIrql);
  g.Store8(pcr + kPcrIrql, uint8_t(new_irql));
  return old;
}

uint32_t KeRaiseIrqlToDpcLevel(Guest g, uint32_t pcr) { return KfRaiseIrql(g, pcr, kIrqlDispatch); }

void KfLowerIrql(Guest g, uint32_t pcr, uint32_t new_irql) { g.Store8(pcr + kPcrIrql, uint8_t(new_irql)); }

uint32_t KfAcquireSpinLock(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t old_irql = KfRaiseIrql(g, pcr, kIrqlDispatch);
  Acquire(g, pcr, lock);
  return old_irql;
}

void KfReleaseSpinLock(Guest g, uint32_t pcr, uint32_t lock, uint32_t old_irql) {
  Word(g, lock).store(0, std::memory_order_release);
  if (old_irql < kIrqlDispatch) KfLowerIrql(g, pcr, old_irql);
}

void KeAcquireSpinLockAtRaisedIrql(Guest g, uint32_t pcr, uint32_t lock) { Acquire(g, pcr, lock); }

uint32_t KeTryToAcquireSpinLockAtRaisedIrql(Guest g, uint32_t pcr, uint32_t lock) {
  uint32_t expected = 0;
  return Word(g, lock).compare_exchange_strong(expected, BigEndian(pcr)) ? 1 : 0;
}

void KeReleaseSpinLockFromRaisedIrql(Guest g, uint32_t lock) { Word(g, lock).store(0, std::memory_order_release); }

void KeEnterCriticalRegion(Guest g, uint32_t pcr) {
  std::atomic<int32_t>& count = *reinterpret_cast<std::atomic<int32_t>*>(g.host(CurrentThread(g, pcr) + kThreadApcDisable));
  count.fetch_sub(1);
}

void KeLeaveCriticalRegion(Guest g, uint32_t pcr) {
  std::atomic<int32_t>& count = *reinterpret_cast<std::atomic<int32_t>*>(g.host(CurrentThread(g, pcr) + kThreadApcDisable));
  count.fetch_add(1);
}

uint32_t KeTlsAlloc(Guest g, uint32_t pcr) {
  const uint32_t process = g.Load32(CurrentThread(g, pcr) + kThreadProcess);
  if (!process) return 0xFFFFFFFF;
  for (uint32_t index = 0; index < 8; ++index) {
    const uint32_t bits = g.Load32(process + kProcessTlsBitmap + 4 * index);
    if (!bits) continue;
    const uint32_t leading = uint32_t(std::countl_zero(bits));
    const uint32_t slot = index * 32 + leading;
    g.Store32(process + kProcessTlsBitmap + 4 * index, bits & ~(1u << (31 - leading)));
    KeTlsSetValue(g, pcr, slot, 0);
    return slot;
  }
  return 0xFFFFFFFF;
}

uint32_t KeTlsFree(Guest g, uint32_t pcr, uint32_t slot) {
  if (slot == 0xFFFFFFFF) return 0;
  if (slot < 256) {
    const uint32_t process = g.Load32(CurrentThread(g, pcr) + kThreadProcess);
    if (process) {
      const uint32_t address = process + kProcessTlsBitmap + 4 * (slot / 32), mask = 1u << (31 - slot % 32);
      const uint32_t bits = g.Load32(address);
      if (!(bits & mask)) g.Store32(address, bits | mask);
    }
  }
  return 1;
}

uint32_t KeTlsGetValue(Guest g, uint32_t pcr, uint32_t slot) {
  if (slot * 4 > kTlsSlots * 4) return 0;
  return g.Load32(g.Load32(CurrentThread(g, pcr) + kThreadTls) + slot * 4);
}

uint32_t KeTlsSetValue(Guest g, uint32_t pcr, uint32_t slot, uint32_t value) {
  if (slot * 4 >= kTlsSlots * 4) return 0;
  g.Store32(g.Load32(CurrentThread(g, pcr) + kThreadTls) + slot * 4, value);
  return 1;
}

namespace {
constexpr uint32_t kDpcListEntry = 4, kDpcRoutine = 0xC, kDpcContext = 0x10, kDpcArg1 = 0x14, kDpcArg2 = 0x18;
std::mutex dpc_mutex;
std::vector<uint32_t> dpc_queue;
}  // namespace

void KeInitializeDpc(Guest g, uint32_t dpc, uint32_t routine, uint32_t context) {
  g.Store16(dpc, 19);
  g.Store8(dpc + 2, 0);
  g.Store8(dpc + 3, 0);
  g.Store32(dpc + kDpcListEntry, 0);
  g.Store32(dpc + kDpcListEntry + 4, 0);
  g.Store32(dpc + kDpcRoutine, routine);
  g.Store32(dpc + kDpcContext, context);
  g.Store32(dpc + kDpcArg1, 0);
  g.Store32(dpc + kDpcArg2, 0);
}

uint32_t KeInsertQueueDpc(Guest g, uint32_t dpc, uint32_t arg1, uint32_t arg2) {
  std::lock_guard lock(dpc_mutex);
  if (std::find(dpc_queue.begin(), dpc_queue.end(), dpc) != dpc_queue.end()) return 0;
  g.Store32(dpc + kDpcArg1, arg1);
  g.Store32(dpc + kDpcArg2, arg2);
  dpc_queue.push_back(dpc);
  return 1;
}

uint32_t KeRemoveQueueDpc(Guest, uint32_t dpc) {
  std::lock_guard lock(dpc_mutex);
  const auto it = std::find(dpc_queue.begin(), dpc_queue.end(), dpc);
  if (it == dpc_queue.end()) return 0;
  dpc_queue.erase(it);
  return 1;
}

void ResetDpcs() {
  std::lock_guard lock(dpc_mutex);
  dpc_queue.clear();
}

uint32_t KeQueryPerformanceFrequency() { return 50'000'000; }

}  // namespace any360::sys
