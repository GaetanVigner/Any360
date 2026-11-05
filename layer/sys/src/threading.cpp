#include "any360/sys/threading.h"

#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
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
std::condition_variable dpc_ready;
std::vector<uint32_t> dpc_queue;
std::thread dpc_thread;
bool dpc_stop = false;

void RunDpcs(Guest g, GuestContext context) {
  std::unique_lock lock(dpc_mutex);
  for (;;) {
    dpc_ready.wait(lock, [] { return dpc_stop || !dpc_queue.empty(); });
    if (dpc_stop) return;
    // Off the queue before it runs, so the routine may queue it again.
    const uint32_t dpc = dpc_queue.front();
    dpc_queue.erase(dpc_queue.begin());
    const uint32_t routine = g.Load32(dpc + kDpcRoutine), argument = g.Load32(dpc + kDpcContext);
    const uint32_t arg1 = g.Load32(dpc + kDpcArg1), arg2 = g.Load32(dpc + kDpcArg2);
    lock.unlock();
    g.Store8(context.pcr + kPcrIrql, kIrqlDispatch);
    g.Store32(context.pcr + kPrcbDpcActive, 1);
    Threads().Dispatch(routine, context, {dpc, argument, arg1, arg2});
    g.Store32(context.pcr + kPrcbDpcActive, 0);
    g.Store8(context.pcr + kPcrIrql, 0);
    lock.lock();
  }
}
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
  if (!dpc_thread.joinable()) {
    GuestContext context;
    if (Threads().CreateContext(g, 0x4000, context) == 0) {
      dpc_stop = false;
      dpc_thread = std::thread([context] {
        GuestContext c = context;
        Threads().RunAsGuestThread([](void* p) { RunDpcs(Objects().guest(), *static_cast<GuestContext*>(p)); }, &c);
      });
    }
  }
  dpc_ready.notify_one();
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
  {
    std::lock_guard lock(dpc_mutex);
    dpc_stop = true;
    dpc_queue.clear();
  }
  dpc_ready.notify_all();
  if (dpc_thread.joinable()) dpc_thread.join();
}

uint32_t KeQueryPerformanceFrequency() { return 50'000'000; }

// --- read/write locks ---

namespace {

constexpr uint32_t kRwCount = 0, kRwWritersWaiting = 4, kRwReadersWaiting = 8, kRwReadersIn = 0xC, kRwWriterEvent = 0x10,
                   kRwReaderSemaphore = 0x20, kRwSpinLock = 0x34;

int32_t RwCount(Guest g, uint32_t lock) { return int32_t(g.Load32(lock + kRwCount)); }
void Add(Guest g, uint32_t at, int32_t delta) { g.Store32(at, g.Load32(at) + uint32_t(delta)); }

}  // namespace

void ExInitializeReadWriteLock(Guest g, uint32_t lock) {
  g.Store32(lock + kRwCount, 0xFFFFFFFF);
  g.Store32(lock + kRwWritersWaiting, 0);
  g.Store32(lock + kRwReadersWaiting, 0);
  g.Store32(lock + kRwReadersIn, 0);
  Objects().InitializeEvent(g, lock + kRwWriterEvent, kSynchronizationEvent, 0);
  Objects().InitializeSemaphore(g, lock + kRwReaderSemaphore, 0, 0x7FFFFFFF);
  g.Store32(lock + kRwSpinLock, 0);
}

void ExAcquireReadWriteLockExclusive(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t irql = KfAcquireSpinLock(g, pcr, lock + kRwSpinLock);
  Add(g, lock + kRwCount, 1);
  if (RwCount(g, lock) == 0) {
    KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
    return;
  }
  Add(g, lock + kRwWritersWaiting, 1);
  KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
  Objects().WaitForObject(g, lock + kRwWriterEvent, g.Load32(pcr + kPrcbCurrentThread), nullptr);
}

uint32_t ExTryToAcquireReadWriteLockExclusive(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t irql = KfAcquireSpinLock(g, pcr, lock + kRwSpinLock);
  const bool free = RwCount(g, lock) < 0;
  if (free) g.Store32(lock + kRwCount, 0);
  KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
  return free ? 1 : 0;
}

void ExAcquireReadWriteLockShared(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t irql = KfAcquireSpinLock(g, pcr, lock + kRwSpinLock);
  Add(g, lock + kRwCount, 1);
  // Free, or held by readers with no writer waiting: in at once.
  if (RwCount(g, lock) == 0 || (g.Load32(lock + kRwReadersIn) && !g.Load32(lock + kRwWritersWaiting))) {
    Add(g, lock + kRwReadersIn, 1);
    KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
    return;
  }
  Add(g, lock + kRwReadersWaiting, 1);
  KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
  Objects().WaitForObject(g, lock + kRwReaderSemaphore, g.Load32(pcr + kPrcbCurrentThread), nullptr);
}

uint32_t ExTryToAcquireReadWriteLockShared(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t irql = KfAcquireSpinLock(g, pcr, lock + kRwSpinLock);
  const bool in = RwCount(g, lock) < 0 || (g.Load32(lock + kRwReadersIn) && !g.Load32(lock + kRwWritersWaiting));
  if (in) {
    Add(g, lock + kRwCount, 1);
    Add(g, lock + kRwReadersIn, 1);
  }
  KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
  return in ? 1 : 0;
}

void ExReleaseReadWriteLock(Guest g, uint32_t pcr, uint32_t lock) {
  const uint32_t irql = KfAcquireSpinLock(g, pcr, lock + kRwSpinLock);
  Add(g, lock + kRwCount, -1);
  if (RwCount(g, lock) < 0) {
    g.Store32(lock + kRwReadersIn, 0);
    KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
    return;
  }
  // A writer leaving with readers waiting lets them all in.
  if (!g.Load32(lock + kRwReadersIn)) {
    if (const uint32_t waiting = g.Load32(lock + kRwReadersWaiting)) {
      g.Store32(lock + kRwReadersWaiting, 0);
      g.Store32(lock + kRwReadersIn, waiting);
      KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
      uint32_t status = 0;
      Objects().ReleaseSemaphore(g, lock + kRwReaderSemaphore, waiting, status);
      return;
    }
  }
  Add(g, lock + kRwReadersIn, -1);
  if (g.Load32(lock + kRwReadersIn)) {
    KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
    return;
  }
  // The last reader (or a writer with no reader waiting) hands the lock to one writer.
  Add(g, lock + kRwWritersWaiting, -1);
  KfReleaseSpinLock(g, pcr, lock + kRwSpinLock, irql);
  Objects().SetEvent(g, lock + kRwWriterEvent);
}

// --- interlocked singly linked lists ---

namespace {

struct SListHeader {
  uint32_t next;
  uint16_t depth, sequence;
};

SListHeader Decode(uint64_t raw) {
  uint8_t b[8];
  std::memcpy(b, &raw, 8);
  return {uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3], uint16_t(b[4] << 8 | b[5]),
          uint16_t(b[6] << 8 | b[7])};
}

uint64_t Encode(SListHeader h) {
  const uint8_t b[8] = {uint8_t(h.next >> 24), uint8_t(h.next >> 16), uint8_t(h.next >> 8), uint8_t(h.next),
                        uint8_t(h.depth >> 8),  uint8_t(h.depth),       uint8_t(h.sequence >> 8), uint8_t(h.sequence)};
  uint64_t raw;
  std::memcpy(&raw, b, 8);
  return raw;
}

std::atomic_ref<uint64_t> HeaderWord(Guest g, uint32_t header) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(g.host(header)));
}

}  // namespace

uint32_t InterlockedPushEntrySList(Guest g, uint32_t header, uint32_t entry) {
  auto word = HeaderWord(g, header);
  uint64_t old = word.load();
  SListHeader h;
  do {
    h = Decode(old);
    g.Store32(entry, h.next);
  } while (!word.compare_exchange_weak(old, Encode({entry, uint16_t(h.depth + 1), uint16_t(h.sequence + 1)})));
  return h.next;
}

uint32_t InterlockedPopEntrySList(Guest g, uint32_t header) {
  auto word = HeaderWord(g, header);
  uint64_t old = word.load();
  SListHeader h;
  do {
    h = Decode(old);
    if (!h.next) return 0;
  } while (!word.compare_exchange_weak(old, Encode({g.Load32(h.next), uint16_t(h.depth - 1), h.sequence})));
  return h.next;
}

uint32_t InterlockedFlushSList(Guest g, uint32_t header) {
  return Decode(HeaderWord(g, header).exchange(0)).next;
}

}  // namespace any360::sys
