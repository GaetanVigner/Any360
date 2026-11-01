// Scenario tests of the object manager against the NT kernel's documented semantics (the reference runtime keeps
// this state in host objects and is not an oracle for it, see docs/SYS.md): sequences of calls checked on statuses,
// returned states and the guest fields. Handles are abstracted: a test keeps what it was given.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/files.h"
#include "any360/sys/memory.h"
#include <filesystem>
#include <fstream>
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/crypt.h"
#include "any360/sys/rtl.h"
#include "any360/sys/xam.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <thread>

namespace {

using namespace any360::sys;

std::vector<uint8_t> g_memory(0x80000);  // sized once: `g` points into it
const Guest g{g_memory.data()};
constexpr uint32_t kHeap = 0x10000, kScratch = 0x1000, kMainThread = 0x8000, kOtherThread = 0x8100;
int g_failures = 0;

#define CHECK(condition)                                                                   \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
      ++g_failures;                                                                        \
    }                                                                                      \
  } while (0)

void Fresh() {
  Threads().Shutdown();
  std::memset(g_memory.data(), 0, g_memory.size());
  Objects().SetKernelHeap(kHeap, 0x8000);
  Objects().SetGuest(g);
  Objects().Reset();
}

// An OBJECT_ATTRIBUTES at kScratch + 0x100 naming `name` (ANSI_STRING at +0x120, text at +0x140).
uint32_t Named(const char* name) {
  const uint32_t attributes = kScratch + 0x100, string = kScratch + 0x120, text = kScratch + 0x140;
  g.Store32(attributes + 4, string);
  g.Store16(string, uint16_t(std::strlen(name)));
  g.Store16(string + 2, uint16_t(std::strlen(name) + 1));
  g.Store32(string + 4, text);
  std::memcpy(g.host(text), name, std::strlen(name) + 1);
  return attributes;
}

void Events() {
  Fresh();
  // Ke* on a guest object: the state is in the header, the previous state comes back.
  const uint32_t ev = kScratch;
  Objects().InitializeEvent(g, ev, kSynchronizationEvent, 0);
  CHECK(g.Load8(ev) == kSynchronizationEvent && g.Load32(ev + 4) == 0);
  CHECK(g.Load32(ev + 8) == ev + 8 && g.Load32(ev + 12) == ev + 8);
  CHECK(Objects().SetEvent(g, ev) == 0 && g.Load32(ev + 4) == 1);
  CHECK(Objects().SetEvent(g, ev) == 1);
  CHECK(Objects().ResetEvent(g, ev) == 1 && g.Load32(ev + 4) == 0);
  CHECK(Objects().ResetEvent(g, ev) == 0);
  CHECK(Objects().SetEvent(g, ev) == 0 && Objects().PulseEvent(g, ev) == 1 && g.Load32(ev + 4) == 0);
  // A synchronization event is consumed by a wait; a notification event is not.
  Objects().SetEvent(g, ev);
  const int64_t zero = 0;
  CHECK(Objects().WaitForObject(g, ev, kMainThread, &zero) == 0 && g.Load32(ev + 4) == 0);
  CHECK(Objects().WaitForObject(g, ev, kMainThread, &zero) == kStatusTimeout);
  Objects().InitializeEvent(g, ev, kNotificationEvent, 1);
  CHECK(Objects().WaitForObject(g, ev, kMainThread, &zero) == 0 && g.Load32(ev + 4) == 1);
  CHECK(Objects().WaitForObject(g, ev, kMainThread, &zero) == 0);
  // A short relative timeout on an unsignalled event times out.
  Objects().ResetEvent(g, ev);
  const int64_t ten_ms = -100000;
  CHECK(Objects().WaitForObject(g, ev, kMainThread, &ten_ms) == kStatusTimeout);

  // Nt* through handles.
  const uint32_t out = kScratch + 0x200;
  CHECK(Objects().CreateEvent(g, out, 0, 1, 0) == 0);
  const uint32_t h = g.Load32(out);
  CHECK(h >= kHandleBase && (h & 3) == 0);
  const uint32_t object = Objects().Lookup(h);
  CHECK(object >= kHeap && g.Load8(object) == kSynchronizationEvent);
  CHECK(Objects().NtSetEvent(g, h, out + 4) == 0 && g.Load32(out + 4) == 0 && g.Load32(object + 4) == 1);
  CHECK(Objects().NtSetEvent(g, h, out + 4) == 0 && g.Load32(out + 4) == 1);
  CHECK(Objects().NtQueryEvent(g, h, out + 8) == 0 && g.Load32(out + 8) == 1 && g.Load32(out + 12) == 1);
  CHECK(Objects().NtClearEvent(g, h) == 0 && g.Load32(object + 4) == 0);
  CHECK(Objects().NtPulseEvent(g, h, out + 4) == 0 && g.Load32(out + 4) == 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == kStatusTimeout);
  Objects().NtSetEvent(g, h, 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == 0);
  // Duplicate, close: the object lives while a handle does.
  CHECK(Objects().Duplicate(g, h, out + 16, 0) == 0);
  const uint32_t h2 = g.Load32(out + 16);
  CHECK(h2 != h && Objects().Lookup(h2) == object);
  CHECK(Objects().Close(h) == 0 && Objects().Lookup(h) == 0 && Objects().Lookup(h2) == object);
  CHECK(Objects().Close(h) == kStatusInvalidHandle);
  CHECK(Objects().NtSetEvent(g, h, 0) == kStatusInvalidHandle);
  CHECK(Objects().Close(h2) == 0 && Objects().Lookup(h2) == 0);
  CHECK(Objects().WaitForHandle(g, h2, kMainThread, &zero) == kStatusInvalidHandle);
  // A freed slot is reused; handles stay four apart.
  CHECK(Objects().CreateEvent(g, out, 0, 0, 1) == 0 && g.Load32(out) == h);
  // Named events: a second creation opens the existing one.
  CHECK(Objects().CreateEvent(g, out, Named("\\Shared"), 0, 0) == 0);
  const uint32_t named = g.Load32(out);
  CHECK(Objects().CreateEvent(g, out + 4, Named("\\Shared"), 1, 1) == kStatusObjectNameExists);
  CHECK(Objects().Lookup(g.Load32(out + 4)) == Objects().Lookup(named));
  CHECK(Objects().CreateSemaphore(g, out + 8, Named("\\Shared"), 0, 1) == kStatusInvalidHandle);
}

void Semaphores() {
  Fresh();
  const uint32_t sem = kScratch;
  Objects().InitializeSemaphore(g, sem, 2, 5);
  CHECK(g.Load8(sem) == kSemaphore && g.Load32(sem + 4) == 2 && g.Load32(sem + 16) == 5);
  uint32_t status = 1;
  CHECK(Objects().ReleaseSemaphore(g, sem, 2, status) == 2 && status == 0 && g.Load32(sem + 4) == 4);
  CHECK(Objects().ReleaseSemaphore(g, sem, 2, status) == 4 && status == kStatusSemaphoreLimitExceeded && g.Load32(sem + 4) == 4);
  CHECK(Objects().ReleaseSemaphore(g, sem, 1, status) == 4 && status == 0 && g.Load32(sem + 4) == 5);
  const int64_t zero = 0;
  for (int i = 0; i < 5; ++i) CHECK(Objects().WaitForObject(g, sem, kMainThread, &zero) == 0);
  CHECK(g.Load32(sem + 4) == 0 && Objects().WaitForObject(g, sem, kMainThread, &zero) == kStatusTimeout);

  const uint32_t out = kScratch + 0x200;
  CHECK(Objects().CreateSemaphore(g, out, 0, 3, 2) == kStatusInvalidParameter);
  CHECK(Objects().CreateSemaphore(g, out, 0, 1, 3) == 0);
  const uint32_t h = g.Load32(out);
  CHECK(Objects().NtReleaseSemaphore(g, h, 1, out + 4) == 0 && g.Load32(out + 4) == 1);
  CHECK(Objects().NtReleaseSemaphore(g, h, 5, out + 4) == kStatusSemaphoreLimitExceeded && g.Load32(out + 4) == 2);
  CHECK(Objects().NtReleaseSemaphore(g, kHandleBase + 0x400, 1, 0) == kStatusInvalidHandle);
  CHECK(Objects().Close(h) == 0);
}

void Mutants() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  // Created owned: the creating thread holds it, another thread cannot release or take it.
  CHECK(Objects().CreateMutant(g, out, 0, 1, kMainThread) == 0);
  const uint32_t h = g.Load32(out), m = Objects().Lookup(h);
  CHECK(g.Load8(m) == kMutant && g.Load32(m + 4) == 0 && g.Load32(m + kMutantOwner) == kMainThread);
  CHECK(Objects().NtReleaseMutant(g, h, kOtherThread) == kStatusMutantNotOwned);
  const int64_t zero = 0;
  CHECK(Objects().WaitForHandle(g, h, kOtherThread, &zero) == kStatusTimeout);
  // The owner may acquire again (recursion) and must release as many times.
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == 0 && int32_t(g.Load32(m + 4)) == -1);
  CHECK(Objects().NtReleaseMutant(g, h, kMainThread) == 0 && g.Load32(m + 4) == 0 && g.Load32(m + kMutantOwner) == kMainThread);
  CHECK(Objects().NtReleaseMutant(g, h, kMainThread) == 0 && g.Load32(m + 4) == 1 && g.Load32(m + kMutantOwner) == 0);
  CHECK(Objects().NtReleaseMutant(g, h, kMainThread) == kStatusMutantNotOwned);
  // Free: the other thread takes it.
  CHECK(Objects().WaitForHandle(g, h, kOtherThread, &zero) == 0 && g.Load32(m + kMutantOwner) == kOtherThread);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == kStatusTimeout);
  // Created free.
  CHECK(Objects().CreateMutant(g, out + 4, 0, 0, kMainThread) == 0);
  const uint32_t m2 = Objects().Lookup(g.Load32(out + 4));
  CHECK(g.Load32(m2 + 4) == 1 && g.Load32(m2 + kMutantOwner) == 0);
}

void References() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
  const uint32_t h = g.Load32(out), object = Objects().Lookup(h);
  // A reference keeps the object alive after its handle closes.
  CHECK(Objects().ReferenceObjectByHandle(g, h, kMainThread, out + 4) == 0 && g.Load32(out + 4) == object);
  CHECK(Objects().Close(h) == 0 && Objects().Lookup(h) == 0);
  CHECK(Objects().SetEvent(g, object) == 0 && g.Load32(object + 4) == 1);
  CHECK(Objects().DereferenceObject(g, object) == 0);
  // The current-thread pseudo handle.
  CHECK(Objects().ReferenceObjectByHandle(g, kCurrentThreadHandle, kMainThread, out + 8) == 0 && g.Load32(out + 8) == kMainThread);
  CHECK(Objects().ReferenceObjectByHandle(g, kHandleBase + 0x1000, kMainThread, out + 8) == kStatusInvalidHandle);
  // DUPLICATE_CLOSE_SOURCE.
  CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
  const uint32_t h3 = g.Load32(out);
  CHECK(Objects().Duplicate(g, h3, out + 12, 1) == 0 && Objects().Lookup(h3) == 0 && Objects().Lookup(g.Load32(out + 12)) != 0);
}

void Timers() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  const int64_t zero = 0, ten_ms = -100000, half_second = -5000000;
  // A relative one-shot notification timer: not signalled before its due time, signalled after, and it stays so.
  CHECK(Objects().CreateTimer(g, out, 0, 0) == 0);
  const uint32_t h = g.Load32(out), timer = Objects().Lookup(h);
  CHECK(g.Load8(timer) == kNotificationTimer && g.Load32(timer + 4) == 0);
  g.Store64(out + 8, uint64_t(ten_ms));
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, out + 16) == 0 && g.Load32(out + 16) == 0 && g.Load8(timer + 3) == 1);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == kStatusTimeout);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &half_second) == 0);
  CHECK(g.Load32(timer + 4) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == 0);
  // Re-arming reports the timer was not armed any more; cancelling an armed one reports it was.
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, out + 16) == 0 && g.Load32(out + 16) == 0 && g.Load32(timer + 4) == 0);
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, out + 16) == 0 && g.Load32(out + 16) == 1);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &ten_ms) == kStatusTimeout);
  // A synchronization timer is consumed by the wait; a periodic one re-arms.
  CHECK(Objects().CreateTimer(g, out + 24, 0, 1) == 0);
  const uint32_t hs = g.Load32(out + 24), sync = Objects().Lookup(hs);
  CHECK(g.Load8(sync) == kSynchronizationTimer);
  CHECK(Objects().NtSetTimerEx(g, hs, out + 8, 5, 0) == 0);
  CHECK(Objects().WaitForHandle(g, hs, kMainThread, &half_second) == 0 && g.Load32(sync + 4) == 0 && g.Load8(sync + 3) == 1);
  CHECK(Objects().WaitForHandle(g, hs, kMainThread, &half_second) == 0);
  CHECK(Objects().NtCancelTimer(g, hs, 0) == 0);
  // An absolute due time in the past is due at once; KeInitializeTimerEx on a guest structure.
  const uint32_t kt = kScratch;
  Objects().InitializeTimer(g, kt, 0, 2);
  CHECK(g.Load8(kt) == kNotificationTimer && g.Load8(kt + 2) == 2 && g.Load64(kt + 16) == 0);
  CHECK(Objects().SetTimer(g, kt, int64_t(SystemTime() - 10), 0) == 0);
  CHECK(Objects().WaitForObject(g, kt, kMainThread, &zero) == 0 && g.Load32(kt + 4) == 1);
  CHECK(Objects().NtSetTimerEx(g, h, 0, 0, 0) == kStatusInvalidParameter);
  CHECK(Objects().NtSetTimerEx(g, kHandleBase + 0x800, out + 8, 0, 0) == kStatusInvalidHandle);
  // The clock advances and sits after 2020.
  const uint64_t t0 = SystemTime();
  CHECK(t0 > 132223104000000000ull && SystemTime() >= t0);
}

void MultipleWaits() {
  Fresh();
  const uint32_t out = kScratch + 0x200, handles = kScratch + 0x300;
  const int64_t zero = 0, ten_ms = -100000;
  CHECK(Objects().CreateEvent(g, handles, 0, 1, 0) == 0);      // sync event, unsignalled
  CHECK(Objects().CreateEvent(g, handles + 4, 0, 0, 1) == 0);  // notification event, signalled
  CHECK(Objects().CreateSemaphore(g, handles + 8, 0, 1, 2) == 0);
  const uint32_t e0 = Objects().Lookup(g.Load32(handles)), e1 = Objects().Lookup(g.Load32(handles + 4)),
                 s2 = Objects().Lookup(g.Load32(handles + 8));
  // Wait-any takes the first satisfiable object, in order, and only that one.
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAny, kMainThread, &zero) == 1);
  CHECK(g.Load32(s2 + 4) == 1);
  Objects().SetEvent(g, e0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAny, kMainThread, &zero) == 0 && g.Load32(e0 + 4) == 0);
  // Wait-all needs every object and consumes them together; nothing is taken when one is missing.
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kMainThread, &zero) == kStatusTimeout);
  CHECK(g.Load32(s2 + 4) == 1 && g.Load32(e1 + 4) == 1);
  Objects().SetEvent(g, e0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kMainThread, &zero) == 0);
  CHECK(g.Load32(e0 + 4) == 0 && g.Load32(e1 + 4) == 1 && g.Load32(s2 + 4) == 0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kMainThread, &ten_ms) == kStatusTimeout);
  // An unknown handle in the list is a parameter error; Ke* on guest objects goes through the same path.
  g.Store32(handles + 12, kHandleBase + 0x900);
  CHECK(Objects().WaitForHandles(g, 4, handles, kWaitAny, kMainThread, &zero) == kStatusInvalidParameter);
  const uint32_t objects[] = {e0, e1};
  CHECK(Objects().WaitForObjects(g, 2, objects, kWaitAny, kMainThread, &zero) == 1);
  (void)out;
}

// The test's "guest code": host functions by start address, run on the scheduler's threads.
std::map<uint32_t, std::function<uint64_t(uint32_t pcr, uint64_t, uint64_t)>> g_code;
uint64_t Dispatch(uint32_t address, uint32_t, uint32_t pcr, uint64_t a0, uint64_t a1, void*) {
  auto it = g_code.find(address);
  return it == g_code.end() ? 0xDEAD : it->second(pcr, a0, a1);
}

void SchedulerTests() {
  Fresh();
  Objects().SetKernelHeap(0x10000, 0x70000);
  Objects().SetGuest(Guest{g_memory.data()});
  Objects().Reset();
  const Guest gm{g_memory.data()};
  Threads().SetDispatcher(&Dispatch, nullptr);
  const uint32_t out = kScratch + 0x200, ev = kScratch;
  const int64_t zero = 0, second = -10000000;
  Objects().InitializeEvent(gm, ev, kNotificationEvent, 0);
  // A thread that sets the event and returns 7; its handle is waitable and its exit status recorded.
  g_code[0x1000] = [&](uint32_t pcr, uint64_t context, uint64_t) {
    CHECK(gm.Load32(pcr + 0x100) != 0 && gm.Load32(gm.Load32(pcr + 0x100) + 0x68) != 0);
    CHECK(context == 0x1234);
    Objects().SetEvent(gm, ev);
    return 7;
  };
  CHECK(Threads().CreateThread(gm, out, 0, out + 4, 0, 0x1000, 0x1234, 0) == 0);
  const uint32_t h = gm.Load32(out), id = gm.Load32(out + 4), kthread = Objects().Lookup(h);
  CHECK(id == 1 && kthread != 0 && gm.Load8(kthread) == 6);
  CHECK(Objects().WaitForHandle(gm, h, kMainThread, &second) == 0);
  CHECK(Objects().WaitForObject(gm, ev, kMainThread, &zero) == 0);
  CHECK(gm.Load32(kthread + 0x140) == 7 && gm.Load8(kthread + 0xBE) == 1 && gm.Load32(kthread + 0x14C) == 1);
  CHECK(gm.Load32(kthread + 0x84) == Threads().process() && gm.Load8(kthread + 0x73) == 2);
  CHECK(Threads().NtSuspendThread(gm, h, out + 8) == kStatusThreadIsTerminating);
  // Created suspended: nothing runs until resumed; the previous count comes back.
  Objects().ResetEvent(gm, ev);
  CHECK(Threads().CreateThread(gm, out, 0x8000, out + 4, 0, 0x1000, 0x1234, kCreateSuspended) == 0);
  const uint32_t hs = gm.Load32(out), ks = Objects().Lookup(hs);
  CHECK(gm.Load32(out + 4) == 2 && gm.Load8(ks + 0xBC) == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(gm.Load32(ev + 4) == 0 && gm.Load32(ks + 4) == 0);
  CHECK(Threads().NtSuspendThread(gm, hs, out + 8) == 0 && gm.Load32(out + 8) == 1);
  CHECK(Threads().NtResumeThread(gm, hs, out + 8) == 0 && gm.Load32(out + 8) == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(gm.Load32(ev + 4) == 0);
  CHECK(Threads().NtResumeThread(gm, hs, out + 8) == 0 && gm.Load32(out + 8) == 1);
  CHECK(Objects().WaitForHandle(gm, hs, kMainThread, &second) == 0 && gm.Load32(ev + 4) == 1);
  CHECK(Threads().NtResumeThread(gm, kHandleBase + 0x700, out + 8) == kStatusInvalidHandle);
  // The XAPI start-up path receives (start, context); ExTerminateThread inside the thread records its code; the
  // handle-is-object flag hands the KTHREAD back; a delay sleeps for its interval.
  g_code[0x2000] = [&](uint32_t pcr, uint64_t start, uint64_t context) {
    CHECK(start == 0x3000 && context == 0x42);
    const auto t0 = std::chrono::steady_clock::now();
    Threads().Delay(gm, pcr, -200000);  // 20 ms
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(19));
    Threads().TerminateThread(gm, pcr, 99);
    CHECK(false);  // not reached
    return 0;
  };
  CHECK(Threads().CreateThread(gm, out, 0, 0, 0x2000, 0x3000, 0x42, kCreateHandleIsObject) == 0);
  const uint32_t kx = gm.Load32(out);
  CHECK(gm.Load8(kx) == 6 && Objects().Lookup(kx) == 0);
  CHECK(Objects().WaitForObject(gm, kx, kMainThread, &second) == 0 && gm.Load32(kx + 0x140) == 99);
  // Affinity, priority, boost on a KTHREAD.
  CHECK(Threads().SetAffinity(gm, kx, 0, out + 12) == kStatusInvalidParameter);
  CHECK(Threads().SetAffinity(gm, kx, 0x4, out + 12) == 0 && gm.Load32(out + 12) == 1 && gm.Load8(kx + 0xBF) == 2);
  CHECK(Threads().SetAffinity(gm, kx, 0x2, out + 12) == 0 && gm.Load32(out + 12) == 4);
  CHECK(Threads().QueryBasePriority(gm, kx) == 8);
  CHECK(Threads().SetBasePriority(gm, kx, 12) == 8 && Threads().QueryBasePriority(gm, kx) == 12);
  CHECK(Threads().SetBasePriority(gm, kx, uint32_t(-5)) == 12 && Threads().QueryBasePriority(gm, kx) == 0);
  CHECK(Threads().SetDisableBoost(gm, kx, 1) == 0 && Threads().SetDisableBoost(gm, kx, 0) == 1);
  // Two threads share the process TLS bitmap and have their own TLS blocks.
  std::atomic<int> slots{0};
  g_code[0x4000] = [&](uint32_t pcr, uint64_t, uint64_t) {
    const uint32_t kt = gm.Load32(pcr + 0x100);
    gm.Store32(gm.Load32(kt + 0x68), gm.Load32(kt + 0x14C));
    ++slots;
    return 0;
  };
  CHECK(Threads().CreateThread(gm, out, 0, 0, 0, 0x4000, 0, 0) == 0);
  CHECK(Threads().CreateThread(gm, out + 4, 0, 0, 0, 0x4000, 0, 0) == 0);
  CHECK(Objects().WaitForHandle(gm, gm.Load32(out), kMainThread, &second) == 0);
  CHECK(Objects().WaitForHandle(gm, gm.Load32(out + 4), kMainThread, &second) == 0);
  const uint32_t ka = Objects().Lookup(gm.Load32(out)), kb = Objects().Lookup(gm.Load32(out + 4));
  CHECK(slots == 2 && gm.Load32(gm.Load32(ka + 0x68)) == gm.Load32(ka + 0x14C) && gm.Load32(gm.Load32(kb + 0x68)) == gm.Load32(kb + 0x14C));
  CHECK(gm.Load32(ka + 0x68) != gm.Load32(kb + 0x68));
  Threads().Shutdown();
}

// The memory manager on a small layout the test's buffer can hold: a 4 KB virtual heap, a 64 KB one, a 4 KB physical
// view and a 64 KB physical view sharing one physical space.
void MemoryTests() {
  Fresh();
  std::vector<uint8_t> memory(0x200000);
  const Guest gm{memory.data()};
  Memory().Reset({{0x00000000, 0x40000, 0x1000, false}, {0x40000, 0x80000, 0x10000, false},
                  {0x100000, 0x40000, 0x1000, true}, {0x140000, 0x80000, 0x10000, true}});
  const uint32_t io = 0x3F000;  // the in/out words live in the small heap's last page, never allocated by the tests
  gm.Store32(io, 0);
  gm.Store32(io + 4, 0x3000);
  // Reserve and commit: the size rounds up to 64 KB for a search, pages are zeroed, the region queries back.
  std::memset(memory.data() + 0x1000, 0xAA, 0x20000);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemReserve | kMemCommit, kPageReadWrite, 0) == 0);
  const uint32_t a = gm.Load32(io);
  CHECK(a != 0 && a % 0x1000 == 0 && a < 0x40000 && gm.Load32(io + 4) == 0x10000);
  CHECK(memory[a] == 0 && memory[a + 0xFFFF] == 0);
  RegionInfo info;
  CHECK(Memory().QueryRegion(a + 0x2000, info) && info.allocation_base == a && info.region_size == 0xE000 &&
        info.state == kMemCommit && info.protect == kPageReadWrite && info.allocation_size == 0x10000);
  CHECK(Memory().QueryVirtual(gm, a, io + 0x20, 0) == 0 && gm.Load32(io + 0x24) == a && gm.Load32(io + 0x2C) == 0x10000 &&
        gm.Load32(io + 0x30) == kMemCommit && gm.Load32(io + 0x38) == kMemPrivate);
  CHECK(Memory().QueryVirtual(gm, a, io + 0x20, 3) == kStatusInvalidParameter);
  CHECK(Memory().QueryAllocationSize(a + 0x100) == 0x10000 && Memory().QueryProtect(a) == kPageReadWrite);
  // Protection changes, queried back; execute bits normalise to read / read-write.
  Memory().SetProtect(a, 0x2000, kPageReadOnly | kPageNoCache);
  CHECK(Memory().QueryProtect(a + 0x1000) == (kPageReadOnly | kPageNoCache) && Memory().QueryProtect(a + 0x2000) == kPageReadWrite);
  Memory().SetProtect(a, 0x1000, kPageReadOnly | kPageReadWrite);  // two bits: ignored
  CHECK(Memory().QueryProtect(a) == (kPageReadOnly | kPageNoCache));
  Memory().SetProtect(a, 0x1000, kPageExecuteReadWrite);
  CHECK(Memory().QueryProtect(a) == kPageReadWrite);
  // Reserve first, commit a part at a fixed address with the heap's page size, decommit, release the whole region.
  gm.Store32(io, 0);
  gm.Store32(io + 4, 0x12000);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemReserve | kMemLargePages, kPageReadWrite, 0) == 0);
  const uint32_t r = gm.Load32(io);
  CHECK(r >= 0x40000 && r % 0x10000 == 0 && gm.Load32(io + 4) == 0x20000);
  CHECK(Memory().QueryRegion(r, info) && info.state == kMemReserve);
  gm.Store32(io, r + 0x10000);
  gm.Store32(io + 4, 0x100);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit, kPageReadOnly, 0) == 0 && gm.Load32(io) == r + 0x10000 && gm.Load32(io + 4) == 0x10000);
  CHECK(Memory().QueryRegion(r, info) && info.state == kMemReserve && info.region_size == 0x10000);
  CHECK(Memory().QueryRegion(r + 0x10000, info) && info.state == kMemCommit && info.protect == kPageReadOnly && info.allocation_base == r);
  gm.Store32(io, r + 0x10000);
  gm.Store32(io + 4, 0x10000);
  CHECK(Memory().FreeVirtual(gm, io, io + 4, kMemDecommit, 0) == 0);
  CHECK(Memory().QueryRegion(r + 0x10000, info) && info.state == kMemReserve);
  gm.Store32(io, r);
  gm.Store32(io + 4, 0);
  CHECK(Memory().FreeVirtual(gm, io, io + 4, kMemRelease, 0) == 0 && gm.Load32(io + 4) == 0x20000);
  CHECK(Memory().QueryRegion(r, info) && info.state == kMemFree);
  gm.Store32(io, r + 0x10000);
  CHECK(Memory().FreeVirtual(gm, io, io + 4, kMemRelease, 0) == kStatusUnsuccessful);  // not a region start
  gm.Store32(io, 0);
  CHECK(Memory().FreeVirtual(gm, io, io + 4, kMemRelease, 0) == kStatusMemoryNotAllocated);
  // Parameter checks, exhaustion, top-down placement.
  gm.Store32(io, 0);
  gm.Store32(io + 4, 0);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit, kPageReadWrite, 0) == kStatusInvalidParameter);
  gm.Store32(io + 4, 0x1000);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, 0, kPageReadWrite, 0) == kStatusInvalidParameter);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemReset | kMemCommit, kPageReadWrite, 0) == kStatusInvalidParameter);
  gm.Store32(io + 4, 0x100000);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit, kPageReadWrite, 0) == kStatusNoMemory);
  gm.Store32(io, 0);
  gm.Store32(io + 4, 0x1000);
  CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit | kMemTopDown, kPageReadWrite, 0) == 0);
  CHECK(gm.Load32(io) == 0x40000 - 0x10000);
  // Physical allocations: page size from the protection flags, physical addresses, the views share one space.
  const uint32_t p = Memory().AllocatePhysical(gm, 0, 0x3000, kPageReadWrite, 0, 0xFFFFFFFF, 0);
  CHECK(p >= 0x100000 && p < 0x140000 && Memory().PhysicalAddress(p) == p - 0x100000);
  CHECK(Memory().QueryAllocationSize(p) == 0x3000 && Memory().QueryProtect(p + 0x2000) == kPageReadWrite);
  const uint32_t q = Memory().AllocatePhysical(gm, 0, 0x20000, kPageReadWrite | kMemLargePages, 0, 0xFFFFFFFF, 0x20000);
  CHECK(q >= 0x140000 && q % 0x20000 == 0);
  CHECK(Memory().PhysicalAddress(q) != Memory().PhysicalAddress(p));
  const uint32_t pp = Memory().PhysicalAddress(q);
  bool overlap = pp < Memory().PhysicalAddress(p) + 0x3000 && Memory().PhysicalAddress(p) < pp + 0x20000;
  CHECK(!overlap);
  CHECK(Memory().AllocatePhysical(gm, 0, 0x1000, kPageNoAccess, 0, 0xFFFFFFFF, 0) == 0);
  CHECK(Memory().AllocatePhysical(gm, 0, 0x50000, kPageReadWrite, 0, 0xFFFFFFFF, 0) == 0);  // more than the 4 KB view holds
  Memory().FreePhysical(0, p);
  CHECK(Memory().QueryProtect(p) == 0 && Memory().PhysicalAddress(0x3F000) == 0xFFFFFFFF);
  // Statistics and XamAlloc.
  gm.Store32(io + 0x40, 104);
  CHECK(Memory().QueryStatistics(gm, io + 0x40) == 0 && gm.Load32(io + 0x44) == 0x20000 && gm.Load32(io + 0x4C) < 0x20000);
  gm.Store32(io + 0x40, 100);
  CHECK(Memory().QueryStatistics(gm, io + 0x40) == 0xC0000023u);
  CHECK(Memory().XamAlloc(gm, 0, 100, io + 8) == 0);
  const uint32_t x = gm.Load32(io + 8);
  CHECK(x != 0 && x % 0x1000 == 0 && Memory().QueryAllocationSize(x) == 0x1000 && memory[x] == 0);
  CHECK(Memory().XamFree(gm, x) == 0 && Memory().QueryAllocationSize(x) == 0);
  Memory().Reset();
}

// The file system on a temporary directory: a read-only "game" device and a writable "hdd" one.
void FileTests() {
  Fresh();
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "any360-sys-files";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "game" / "Media" / "Levels");
  fs::create_directories(root / "hdd");
  { std::ofstream(root / "game" / "Media" / "Data.BIN", std::ios::binary) << "0123456789ABCDEF"; }
  { std::ofstream(root / "game" / "Media" / "Levels" / "one.lvl", std::ios::binary) << "1"; }
  { std::ofstream(root / "game" / "Media" / "Levels" / "two.lvl", std::ios::binary) << "22"; }
  { std::ofstream(root / "game" / "Media" / "Levels" / "readme.txt", std::ios::binary) << "x"; }
  Files().Reset();
  Files().Mount("\\Device\\Cdrom0", root / "game", true);
  Files().Mount("\\Device\\Harddisk0\\Partition1", root / "hdd", false);
  Files().Link("game:", "\\Device\\Cdrom0");
  const uint32_t attrs = kScratch + 0x100, name = kScratch + 0x120, text = kScratch + 0x140, iosb = kScratch + 0x300,
                 out = kScratch + 0x320, buf = kScratch + 0x400, info = kScratch + 0x600, pat = kScratch + 0x700;
  auto path = [&](const char* p, uint32_t root_handle = 0) {
    g.Store32(attrs, root_handle);
    g.Store32(attrs + 4, name);
    g.Store16(name, uint16_t(std::strlen(p)));
    g.Store32(name + 4, text);
    std::memcpy(g.host(text), p, std::strlen(p) + 1);
    return attrs;
  };
  const uint32_t kRead = 0x80000000, kWrite = 0x40000000;
  // Open through a link, case-insensitively; read sequentially, then at an offset, then past the end.
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen, kFileNonDirectoryFile) == 0);
  CHECK(g.Load32(iosb) == 0 && g.Load32(iosb + 4) == kOpened);
  const uint32_t f = g.Load32(out);
  CHECK(f >= kHandleBase);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == 0 && g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "0123", 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == 0 && std::memcmp(g.host(buf), "4567", 4) == 0);
  g.Store64(out + 8, 12);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 100, out + 8) == 0 && g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "CDEF", 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == kStatusEndOfFile && g.Load32(iosb) == kStatusEndOfFile && g.Load32(iosb + 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 0, 0) == 0);  // zero length at the end succeeds
  // The read signals an event; an unknown event handle is refused.
  const uint32_t ev = out + 16;
  CHECK(Objects().CreateEvent(g, ev, 0, 0, 0) == 0);
  g.Store64(out + 8, 0);
  CHECK(Files().ReadFile(g, f, g.Load32(ev), iosb, buf, 2, out + 8) == 0 && Objects().WaitForHandle(g, g.Load32(ev), kMainThread, nullptr) == 0);
  CHECK(Files().ReadFile(g, f, kHandleBase + 0xF00, iosb, buf, 2, 0) == 0xC0000008u);
  // Information classes.
  CHECK(Files().QueryInformation(g, f, iosb, info, 56, kFileNetworkOpenInformation) == 0 && g.Load64(info + 40) == 16 &&
        g.Load32(info + 48) == kAttributeNormal && g.Load64(info + 24) > 0);
  CHECK(Files().QueryInformation(g, f, iosb, info, 8, kFilePositionInformation) == 0 && g.Load64(info) == 2);
  CHECK(Files().QueryInformation(g, f, iosb, info, 24, kFileStandardInformation) == 0 && g.Load64(info + 8) == 16 && g.Load8(info + 21) == 0);
  CHECK(Files().QueryInformation(g, f, iosb, info, 4, kFileNetworkOpenInformation) == kStatusInfoLengthMismatch);
  CHECK(Files().QueryInformation(g, f, iosb, info, 64, 99) == kStatusInvalidInfoClass);
  g.Store64(info, 10);
  CHECK(Files().SetInformation(g, f, iosb, info, 8, kFilePositionInformation) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 1, 0) == 0 && g.Load8(buf) == 'A');
  // Read-only device: writes are refused at open and at write.
  CHECK(Files().WriteFile(g, f, 0, iosb, buf, 1, 0) == 0xC0000022u);
  CHECK(Files().CreateFile(g, out, kWrite, path("game:\\media\\data.bin"), iosb, 0, kOpen, 0) == 0xC0000022u);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\new.bin"), iosb, 0, kCreate, 0) == 0xC0000022u);
  // Missing names and paths, directory / file mismatches, collisions.
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\nope.bin"), iosb, 0, kOpen, 0) == kStatusObjectNameNotFound &&
        g.Load32(iosb + 4) == kDoesNotExist && g.Load32(out) == 0xFFFFFFFF);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\nope\\data.bin"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("nodevice:\\data.bin"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media"), iosb, 0, kOpen, kFileNonDirectoryFile) == kStatusFileIsADirectory);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen, kFileDirectoryFile) == kStatusNotADirectory);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\..\\x"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  // Full attributes by path; a path relative to an open directory handle.
  CHECK(Files().QueryFullAttributes(g, path("\\Device\\Cdrom0\\MEDIA\\Levels\\two.lvl"), info) == 0 && g.Load64(info + 40) == 2);
  CHECK(Files().QueryFullAttributes(g, path("game:\\media\\Levels"), info) == 0 && g.Load32(info + 48) == kAttributeDirectory);
  CHECK(Files().QueryFullAttributes(g, path("game:\\media\\x.y"), info) == kStatusObjectNameNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\levels"), iosb, 0, kOpen, kFileDirectoryFile) == 0);
  const uint32_t d = g.Load32(out);
  CHECK(Files().CreateFile(g, out, kRead, path("one.lvl", d), iosb, 0, kOpen, 0) == 0);
  CHECK(Files().ReadFile(g, g.Load32(out), 0, iosb, buf, 8, 0) == 0 && g.Load32(iosb + 4) == 1 && g.Load8(buf) == '1');
  CHECK(Objects().Close(g.Load32(out)) == 0);
  // Directory enumeration: one entry per call, sorted, with a pattern; restart; the end.
  g.Store16(pat, 5);
  g.Store32(pat + 4, pat + 8);
  std::memcpy(g.host(pat + 8), "*.lvl", 6);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 0x100, pat, 0) == 0 && g.Load32(info + 0x3C) == 7 &&
        std::memcmp(g.host(info + 0x40), "one.lvl", 7) == 0 && g.Load64(info + 0x28) == 1);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 0x100, 0, 0) == 0 && std::memcmp(g.host(info + 0x40), "two.lvl", 7) == 0);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 0x100, 0, 0) == kStatusNoMoreFiles);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 0x100, 0, 1) == 0 && std::memcmp(g.host(info + 0x40), "one.lvl", 7) == 0);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 64, 0, 0) == kStatusInfoLengthMismatch);
  std::memcpy(g.host(pat + 8), "*.zzz", 6);
  CHECK(Files().QueryDirectory(g, d, iosb, info, 0x100, pat, 0) == kStatusNoSuchFile);
  CHECK(Files().QueryVolume(g, d, iosb, info, 16, kFsAttributeInformation) == 0 && std::memcmp(g.host(info + 12), "FATX", 4) == 0);
  CHECK(Files().QueryVolume(g, d, iosb, info, 24, kFsSizeInformation) == 0 && g.Load32(info + 20) == 0x200 && g.Load64(info + 8) == 0);
  CHECK(Files().QueryVolume(g, d, iosb, info, 8, kFsDeviceInformation) == 0 && g.Load32(info) == 7);
  CHECK(Files().QueryVolume(g, d, iosb, info, 8, 2) == kStatusInvalidInfoClass);
  CHECK(Objects().Close(d) == 0 && Objects().Close(f) == 0);
  // The writable device: create, write, overwrite, grow, rename, delete on close, directories; symbolic links made
  // and removed by the title.
  g.Store16(pat, 4);
  g.Store32(pat + 4, pat + 8);
  std::memcpy(g.host(pat + 8), "hdd:", 4);
  g.Store16(pat + 0x20, 28);
  g.Store32(pat + 0x24, pat + 0x28);
  std::memcpy(g.host(pat + 0x28), "\\Device\\Harddisk0\\Partition1", 29);
  CHECK(Files().CreateSymbolicLink(g, pat, pat + 0x20) == 0);
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\save.dat"), iosb, 0, kOpenIf, 0) == 0 && g.Load32(iosb + 4) == kCreated);
  const uint32_t w = g.Load32(out);
  std::memcpy(g.host(buf), "hello world", 11);
  CHECK(Files().WriteFile(g, w, 0, iosb, buf, 11, 0) == 0 && g.Load32(iosb + 4) == 11);
  g.Store64(out + 8, 6);
  std::memcpy(g.host(buf), "WORLD", 5);
  CHECK(Files().WriteFile(g, w, 0, iosb, buf, 5, out + 8) == 0);
  g.Store64(out + 8, 0);
  CHECK(Files().ReadFile(g, w, 0, iosb, buf, 32, out + 8) == 0 && g.Load32(iosb + 4) == 11 && std::memcmp(g.host(buf), "hello WORLD", 11) == 0);
  g.Store64(info, 4);
  CHECK(Files().SetInformation(g, w, iosb, info, 8, kFileEndOfFileInformation) == 0 && fs::file_size(root / "hdd" / "save.dat") == 4);
  CHECK(Files().Flush(g, w, iosb) == 0);
  // Rename on the same device.
  g.Store32(info, 0);
  g.Store32(info + 4, 0);
  g.Store16(info + 8, 13);
  g.Store32(info + 12, info + 16);
  std::memcpy(g.host(info + 16), "hdd:\\save.bak", 13);
  CHECK(Files().SetInformation(g, w, iosb, info, 16, kFileRenameInformation) == 0 && fs::exists(root / "hdd" / "save.bak") &&
        !fs::exists(root / "hdd" / "save.dat"));
  CHECK(Objects().Close(w) == 0);
  CHECK(Files().CreateFile(g, out, kRead, path("hdd:\\SAVE.BAK"), iosb, 0, kCreate, 0) == kStatusObjectNameCollision);
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\save.bak"), iosb, 0, kOverwriteIf, 0) == 0 &&
        g.Load32(iosb + 4) == kOverwritten && fs::file_size(root / "hdd" / "save.bak") == 0);
  CHECK(Objects().Close(g.Load32(out)) == 0);
  // Delete on close, through the disposition class; the file goes when its last handle does.
  CHECK(Files().CreateFile(g, out, kRead | kWrite | 0x10000, path("hdd:\\save.bak"), iosb, 0, kOpen, 0) == 0);
  const uint32_t del = g.Load32(out);
  CHECK(Objects().Duplicate(g, del, out + 4, 0) == 0);
  g.Store8(info, 1);
  CHECK(Files().SetInformation(g, del, iosb, info, 1, kFileDispositionInformation) == 0);
  CHECK(Objects().Close(del) == 0 && fs::exists(root / "hdd" / "save.bak"));
  CHECK(Objects().Close(g.Load32(out + 4)) == 0 && !fs::exists(root / "hdd" / "save.bak"));
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\cache"), iosb, 0, kCreate, kFileDirectoryFile) == 0 &&
        fs::is_directory(root / "hdd" / "cache"));
  CHECK(Objects().Close(g.Load32(out)) == 0);
  CHECK(Files().DeleteSymbolicLink(g, pat) == 0 && Files().DeleteSymbolicLink(g, pat) == kStatusObjectNameNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("hdd:\\cache"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  CHECK(Files().ReadFile(g, kHandleBase + 0xF00, 0, iosb, buf, 1, 0) == 0xC0000008u);
  Files().Reset();
  fs::remove_all(root, ec);
}

// XAM as an offline console with one local profile, and the title module from a synthetic XEX header.
const char* g_stop_reason = nullptr;
uint32_t g_stop_code = 0;
void RecordStop(const char* reason, uint32_t code) {
  g_stop_reason = reason;
  g_stop_code = code;
}

void XamTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  Files().Reset();
  XamState().host().stop = &RecordStop;
  const fs::path content = fs::temp_directory_path() / "any360-sys-content";
  std::error_code ec;
  fs::remove_all(content, ec);
  XamState().title().content_root = content;
  const uint32_t out = kScratch + 0x200, ov = kScratch + 0x300, buf = kScratch + 0x400, str = kScratch + 0x800;
  // Messages complete at once; through an overlapped structure, its event is set and the call is pending.
  CHECK(XamState().MsgStartIORequest(g, 0xFB, 0x58003, 0, buf, 16) == 0);
  CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
  std::memset(g.host(ov), 0xEE, 0x1C);
  g.Store32(ov + 0x0C, g.Load32(out));
  CHECK(XamState().MsgStartIORequest(g, 0xFB, 0x58003, ov, buf, 16) == kErrorIoPending && g.Load32(ov) == 0 && g.Load32(ov + 4) == 0);
  const int64_t zero = 0;
  CHECK(Objects().WaitForHandle(g, g.Load32(out), kMainThread, &zero) == 0);
  // Notifications: a new system listener hears the sign-in first; filters; the guide when the sign-in UI is shown.
  const uint32_t l = XamState().NotifyCreateListener(g, 1, 0);
  CHECK(l >= kHandleBase);
  CHECK(XamState().NotifyGetNext(g, l, 0, out + 4, out + 8) == 1 && g.Load32(out + 4) == kNotifySigninChanged && g.Load32(out + 8) == 1);
  CHECK(XamState().NotifyGetNext(g, l, 0, out + 4, out + 8) == 0 && g.Load32(out + 4) == 0);
  CHECK(XamState().ShowSigninUI(1, 0) == 0);
  CHECK(XamState().NotifyGetNext(g, l, kNotifySystemUi, out + 4, out + 8) == 1 && g.Load32(out + 8) == 1);
  CHECK(XamState().NotifyGetNext(g, l, kNotifySystemUi, out + 4, out + 8) == 1 && g.Load32(out + 8) == 0);
  CHECK(XamState().NotifyGetNext(g, kHandleBase + 0xF00, 0, out + 4, 0) == 0);
  const uint32_t other = XamState().NotifyCreateListener(g, 1ull << 1, 0);  // a listener of another area hears nothing
  CHECK(XamState().NotifyGetNext(g, other, 0, out + 4, 0) == 0);
  // The user.
  CHECK(XamState().UserGetSigninState(0) == 1 && XamState().UserGetSigninState(1) == 0);
  CHECK(XamState().UserGetXUID(g, 0, 1, out) == 0 && g.Load64(out) == XamState().title().xuid);
  CHECK(XamState().UserGetXUID(g, 2, 1, out) == kErrorNoSuchUser);
  CHECK(XamState().UserGetName(g, 0, str, 16) == 0 && std::strcmp(reinterpret_cast<const char*>(g.host(str)), "Player") == 0);
  CHECK(XamState().UserGetName(g, 0, str, 3) == 0 && std::strcmp(reinterpret_cast<const char*>(g.host(str)), "Pl") == 0);
  CHECK(XamState().UserGetSigninInfo(g, 0, 0, buf) == 0 && g.Load64(buf) == XamState().title().xuid && g.Load32(buf + 12) == 1);
  CHECK(XamState().UserCheckPrivilege(g, 0, 0xFC, out) == 0 && g.Load32(out) == 1);
  // Profile settings: the size protocol, defaults, then what the title wrote.
  const uint32_t ids = kScratch + 0x180, size = kScratch + 0x1F0;
  g.Store32(ids, 0x10040002);      // an int32 setting
  g.Store32(ids + 4, 0x63E83FFF);  // a binary setting of 0x3E8 bytes
  g.Store32(size, 0);
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 2, ids, size, 0, 0) == kErrorInsufficientBuffer);
  const uint32_t needed = g.Load32(size);
  CHECK(needed == 8 + 2 * 40 + 0x3E8);
  const uint32_t big = 0x10000;
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 2, ids, size, big, 0) == 0);
  CHECK(g.Load32(big) == 2 && g.Load32(big + 4) == big + 8 && g.Load32(big + 8) == 1 && g.Load32(big + 8 + 16) == 0x10040002);
  CHECK(g.Load32(big + 48 + 16) == 0x63E83FFF && g.Load32(big + 48 + 32) == 0x3E8 && g.Load32(big + 48 + 36) == big + 88);
  CHECK(XamState().UserReadProfileSettings(g, 0, 1, 0, 0, 2, ids, size, big, 0) == kErrorNoSuchUser);
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 0, ids, size, big, 0) == kErrorInvalidParameter);
  std::memset(g.host(buf), 0, 40);
  g.Store32(buf + 16, 0x10040002);
  g.Store8(buf + 24, 1);
  g.Store32(buf + 32, 77);
  CHECK(XamState().UserWriteProfileSettings(g, 0, 0, 1, buf, 0) == 0);
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 2, ids, size, big, 0) == 0 && g.Load32(big + 8) == 2 && g.Load32(big + 8 + 32) == 77);
  // Sessions, message boxes, device selection.
  CHECK(XamState().SessionCreateHandle(g, out) == 0 && XamState().SessionRefObjByHandle(g, g.Load32(out), out + 4) == 0 && g.Load32(out + 4) != 0);
  CHECK(XamState().ShowMessageBoxUI(g, out + 8, ov) == kErrorIoPending && g.Load32(out + 8) == 0);
  CHECK(XamState().ShowDeviceSelectorUI(g, out + 8, 0) == 0 && g.Load32(out + 8) == 1);
  // Content: create a save, write through its root, close, enumerate, delete.
  const uint32_t data = kScratch + 0xA00;
  std::memset(g.host(data), 0, 0x134);
  g.Store32(data, 1);
  g.Store32(data + 4, 1);  // saved game
  std::memcpy(g.host(data + 0x108), "slot0", 5);
  std::memcpy(g.host(str), "save", 5);
  CHECK(XamState().ContentCreate(g, 0, str, data, 3, out, out + 4, 0) == kErrorFileNotFound);  // open existing: none yet
  CHECK(XamState().ContentCreate(g, 0, str, data, 4, out, out + 4, 0) == 0 && g.Load32(out) == 1);  // open always: created
  const uint32_t attrs = kScratch + 0xC00;
  g.Store32(attrs, 0);
  g.Store32(attrs + 4, attrs + 0x10);
  g.Store16(attrs + 0x10, 13);
  g.Store32(attrs + 0x14, attrs + 0x20);
  std::memcpy(g.host(attrs + 0x20), "save:\\game.sav", 14);
  g.Store16(attrs + 0x10, 14);
  CHECK(Files().CreateFile(g, out + 8, 0xC0000000, attrs, 0, 0, kCreate, 0) == 0);
  CHECK(Objects().Close(g.Load32(out + 8)) == 0 && fs::exists(content / "00000001" / "slot0" / "game.sav"));
  CHECK(XamState().ContentClose(g, str, 0) == 0 && XamState().ContentClose(g, str, 0) == kErrorFileNotFound);
  CHECK(Files().CreateFile(g, out + 8, 0x80000000, attrs, 0, 0, kOpen, 0) == kStatusObjectPathNotFound);
  CHECK(XamState().ContentCreate(g, 0, str, data, 1, out, 0, 0) == kErrorAlreadyExists);  // create new: exists
  CHECK(XamState().ContentCreateEnumerator(g, 0, 0, 1, 0, 1, out + 12, out + 16) == 0 && g.Load32(out + 12) == 0x134);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x134, out + 20, 0) == 0 && g.Load32(out + 20) == 1 &&
        std::memcmp(g.host(buf + 0x108), "slot0", 5) == 0 && g.Load32(buf + 4) == 1);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x134, out + 20, 0) == kErrorNoMoreFiles && g.Load32(out + 20) == 0);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x10, out + 20, 0) == kErrorInsufficientBuffer);
  CHECK(XamState().GetPrivateEnumStructure(g, g.Load32(out + 16), out + 24) == 0 && g.Load32(out + 24) != 0);
  CHECK(XamState().ContentDelete(g, 0, data, 0) == 0 && !fs::exists(content / "00000001" / "slot0"));
  CHECK(XamState().ContentGetDeviceState(g, 1, 0) == 0 && XamState().ContentGetDeviceState(g, 7, 0) == kErrorDeviceNotConnected);
  // Controllers: controller 0 by default, a state from the host, vibration to the host.
  CHECK(XamState().InputGetCapabilities(g, 0, 1, buf) == 0 && g.Load8(buf) == 1);
  CHECK(XamState().InputGetCapabilities(g, 1, 1, buf) == kErrorDeviceNotConnected);
  CHECK(XamState().InputGetState(g, 0xFF, 0, buf) == 0 && g.Load32(buf) == 1 && g.Load16(buf + 4) == 0);
  XamState().host().controller_state = [](uint32_t, GamepadState& s) {
    s.buttons = 0x1000;
    s.thumb_lx = -5;
    return true;
  };
  CHECK(XamState().InputGetState(g, 0, 0, buf) == 0 && g.Load32(buf) == 2 && g.Load16(buf + 4) == 0x1000 && int16_t(g.Load16(buf + 8)) == -5);
  static uint32_t vibrated = 0;
  XamState().host().vibrate = [](uint32_t, uint16_t left, uint16_t right) { vibrated = uint32_t(left) << 16 | right; };
  g.Store16(buf, 0x1234);
  g.Store16(buf + 2, 0x5678);
  CHECK(XamState().InputSetState(g, 0, buf) == 0 && vibrated == 0x12345678);
  CHECK(XamState().InputGetKeystroke(g, 0, 0, buf) == kErrorEmpty);
  // The title module from a synthetic XEX header: inline, pointer and offset fields; privileges; the execution id.
  const uint32_t xex = kScratch + 0xE00;
  std::memset(g.host(xex), 0, 0x100);
  g.Store32(xex, 0x58455832);  // "XEX2"
  g.Store32(xex + 0x14, 3);
  g.Store32(xex + 0x18, 0x00030000);  // system flags, inline
  g.Store32(xex + 0x1C, 0x00000410);
  g.Store32(xex + 0x20, 0x00040006);  // execution info, at an offset
  g.Store32(xex + 0x24, 0x80);
  g.Store32(xex + 0x28, 0x00020001);  // one word, by address
  g.Store32(xex + 0x2C, 0xABCD);
  g.Store32(xex + 0x80 + 12, 0x4D5307E6);  // title id in the execution info
  XamState().title().xex_header = xex;
  XamState().title().module_handle = xex;
  CHECK(XamState().ImageXexHeaderField(g, xex, 0x00030000) == 0x410);
  CHECK(XamState().ImageXexHeaderField(g, xex, 0x00040006) == xex + 0x80);
  CHECK(XamState().ImageXexHeaderField(g, xex, 0x00020001) == xex + 0x2C);
  CHECK(XamState().ImageXexHeaderField(g, xex, 0x12345600) == 0);
  CHECK(XamState().CheckExecutablePrivilege(g, 4) == 1 && XamState().CheckExecutablePrivilege(g, 10) == 1 &&
        XamState().CheckExecutablePrivilege(g, 5) == 0 && XamState().CheckExecutablePrivilege(g, 40) == 0);
  CHECK(XamState().GetExecutionId(g, out) == 0 && g.Load32(g.Load32(out) + 12) == 0x4D5307E6);
  std::memcpy(g.host(str), "Default.XEX", 12);
  CHECK(XamState().GetModuleHandle(g, str, out) == 0 && g.Load32(out) == xex);
  CHECK(XamState().GetModuleHandle(g, 0, out) == 0 && g.Load32(out) == xex);
  std::memcpy(g.host(str), "xboxkrnl.exe", 13);
  CHECK(XamState().GetModuleHandle(g, str, out) == 0xC0000225u);
  CHECK(XamState().GetProcedureAddress(g, 0x100, 5, out) == 0xC000007Au && g.Load32(out) == 0);
  XamState().host().procedure = [](const char*, uint32_t ordinal) { return ordinal == 5 ? 0x82001000u : 0u; };
  CHECK(XamState().GetProcedureAddress(g, 0x100, 5, out) == 0 && g.Load32(out) == 0x82001000);
  // Stops go to the host; thread names are not exceptions.
  g.Store32(buf, 0x406D1388);
  XamState().RaiseException(g, buf);
  CHECK(g_stop_reason == nullptr);
  g.Store32(buf, 0xE06D7363);
  XamState().RaiseException(g, buf);
  CHECK(g_stop_reason && g_stop_code == 0xE06D7363);
  XamState().Stop("bug check", 0xDEAD);
  CHECK(g_stop_code == 0xDEAD);
  // The offline network.
  CHECK(XamState().WsaStartup(g, 0x0202, buf) == 0 && g.Load16(buf) == 0x0202);
  CHECK(XamState().NetStartup() == 0);
  CHECK(XamState().Socket() == 0xFFFFFFFF && XamState().WsaGetLastError() == 10050);
  CHECK(XamState().SocketFailure(0xFFFFFFFF) == 0xFFFFFFFF && XamState().WsaGetLastError() == 10038);
  CHECK(XamState().XNetGetTitleXnAddr(g, buf) == 1 && g.Load32(buf) == 0);
  XamState().Reset();
  Files().Reset();
  fs::remove_all(content, ec);
}

// The calls Sonic '06 still needed.
void SonicTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  Files().Reset();
  std::vector<uint8_t> memory(0x200000);
  const Guest gm{memory.data()};
  Memory().Reset({{0x00000000, 0x80000, 0x1000, false}, {0x80000, 0x80000, 0x10000, false}});
  Objects().SetKernelHeap(0x100000, 0x10000);
  Objects().SetGuest(gm);
  Objects().Reset();
  const uint32_t s = 0x1000, out = 0x1800, buf = 0x2000;
  // SHA-1 of the classic test vector, split over three buffers.
  std::memcpy(gm.host(s), "abc", 3);
  XeCryptSha(gm, s, 1, s + 1, 1, s + 2, 1, out, 20);
  const uint8_t abc[20] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
  CHECK(std::memcmp(gm.host(out), abc, 20) == 0);
  XeCryptSha(gm, 0, 0, 0, 0, 0, 0, out, 4);
  const uint8_t empty[4] = {0xDA, 0x39, 0xA3, 0xEE};
  CHECK(std::memcmp(gm.host(out), empty, 4) == 0);
  CHECK(XeCryptBnQwBeSigVerify() == 1);
  gm.Store32(out, 16);
  CHECK(XeKeysGetKey(gm, buf, out) == 0xC0000001u);
  // Strings the kernel allocates, freed again; NT semantics of the length and of characters above 0xFF.
  const uint32_t wide = 0x3000, ustr = 0x3100, astr = 0x3200;
  const uint16_t text[] = {'S', 'o', 'n', 'i', 'c', 0x00E9, 0x4E2D};
  for (uint32_t i = 0; i < 7; ++i) gm.Store16(wide + 2 * i, text[i]);
  gm.Store16(ustr, 14);
  gm.Store16(ustr + 2, 16);
  gm.Store32(ustr + 4, wide);
  CHECK(RtlUnicodeStringToAnsiString(gm, astr, ustr, 1) == 0);
  const uint32_t allocated = gm.Load32(astr + 4);
  CHECK(allocated && gm.Load16(astr) == 7 && gm.Load16(astr + 2) == 8 && std::memcmp(gm.host(allocated), "Sonic\xE9?", 8) == 0);
  RtlFreeAnsiString(gm, astr);
  CHECK(gm.Load32(astr + 4) == 0 && Memory().QueryAllocationSize(allocated) == 0);
  gm.Store16(astr + 2, 4);
  gm.Store32(astr + 4, buf);
  CHECK(RtlUnicodeStringToAnsiString(gm, astr, ustr, 0) == 0x80000005u && gm.Load16(astr) == 3 && std::strcmp(reinterpret_cast<const char*>(gm.host(buf)), "Son") == 0);
  CHECK(RtlUpcaseUnicodeChar('a') == 'A' && RtlUpcaseUnicodeChar(0xE9) == 0xC9 && RtlUpcaseUnicodeChar(0xF7) == 0xF7 &&
        RtlUpcaseUnicodeChar(0xFF) == 0x178 && RtlUpcaseUnicodeChar('1') == '1');
  // Pool blocks: whole pages, freed by any address inside.
  const uint32_t pool = XamState().AllocatePool(gm, 100);
  CHECK(pool && pool % 0x1000 == 0 && Memory().QueryAllocationSize(pool) == 0x1000);
  XamState().FreePool(pool + 0x10);
  CHECK(Memory().QueryAllocationSize(pool) == 0);
  // Launch data.
  CHECK(XamState().LoaderGetLaunchDataSize(gm, out) == kErrorNotFound && gm.Load32(out) == 0);
  std::memcpy(gm.host(s), "level=3", 7);
  CHECK(XamState().LoaderSetLaunchData(gm, s, 7) == 0);
  CHECK(XamState().LoaderGetLaunchDataSize(gm, out) == 0 && gm.Load32(out) == 7);
  CHECK(XamState().LoaderGetLaunchData(gm, buf, 16) == 0 && std::memcmp(gm.host(buf), "level=3", 7) == 0);
  // Resource sections from the XEX header.
  const uint32_t xex = 0x4000;
  std::memset(gm.host(xex), 0, 0x200);
  gm.Store32(xex + 0x14, 1);
  gm.Store32(xex + 0x18, 0x000002FF);
  gm.Store32(xex + 0x1C, 0x100);
  gm.Store32(xex + 0x100, 4 + 2 * 16);
  std::memcpy(gm.host(xex + 0x104), "SONICRES", 8);
  gm.Store32(xex + 0x10C, 0x82A00000);
  gm.Store32(xex + 0x110, 0x4000);
  std::memcpy(gm.host(xex + 0x114), "4D5307E6", 8);
  gm.Store32(xex + 0x11C, 0x82B00000);
  gm.Store32(xex + 0x120, 0x200);
  XamState().title().xex_header = xex;
  XamState().title().module_handle = xex;
  std::memcpy(gm.host(s), "4D5307E6", 9);
  CHECK(XamState().GetModuleSection(gm, xex, s, out, out + 4) == 0 && gm.Load32(out) == 0x82B00000 && gm.Load32(out + 4) == 0x200);
  std::memcpy(gm.host(s), "NOPE", 5);
  CHECK(XamState().GetModuleSection(gm, xex, s, out, out + 4) == 0xC0000225u);
  CHECK(XamState().GetModuleSection(gm, 0x1234, s, out, out + 4) == 0xC0000008u);
  // Devices, the creator of a save, the cache size, a driver's device object.
  CHECK(XamState().ContentGetDeviceData(gm, 1, buf) == 0 && gm.Load32(buf) == 1 && gm.Load64(buf + 8) > 0 && gm.Load16(buf + 24) == 'H');
  CHECK(XamState().ContentGetDeviceData(gm, 2, buf) == 0 && gm.Load64(buf + 16) == 0);
  CHECK(XamState().ContentGetDeviceData(gm, 9, buf) == kErrorDeviceNotConnected);
  const fs::path content = fs::temp_directory_path() / "any360-sys-content2";
  std::error_code ec;
  fs::remove_all(content, ec);
  fs::create_directories(content / "00000001" / "slot1");
  XamState().title().content_root = content;
  const uint32_t data = 0x5000;
  std::memset(gm.host(data), 0, 0x134);
  gm.Store32(data + 4, 1);
  std::memcpy(gm.host(data + 0x108), "slot1", 5);
  CHECK(XamState().ContentGetCreator(gm, 0, data, out, out + 8, 0) == 0 && gm.Load32(out) == 1 && gm.Load64(out + 8) == XamState().title().xuid);
  std::memcpy(gm.host(data + 0x108), "slot9", 5);
  CHECK(XamState().ContentGetCreator(gm, 0, data, out, out + 8, 0) == kErrorFileNotFound);
  fs::remove_all(content, ec);
  CHECK(XamState().SetCacheElementCount(64) == 0 && XamState().GetCacheElementCount() == 64);
  CHECK(XamState().CreateDevice(gm, out) == 0 && gm.Load32(gm.Load32(out) + 0x18) != 0);
  // Scatter reads: a file read into pages listed one pointer each.
  const fs::path root = fs::temp_directory_path() / "any360-sys-scatter";
  fs::remove_all(root, ec);
  fs::create_directories(root);
  {
    std::ofstream f(root / "big.bin", std::ios::binary);
    for (int i = 0; i < 0x1800; ++i) f.put(char(i * 7));
  }
  Files().Mount("\\Device\\Cdrom0", root, true);
  const uint32_t attrs = 0x6000;
  gm.Store32(attrs, 0);
  gm.Store32(attrs + 4, attrs + 0x10);
  gm.Store16(attrs + 0x10, 22);
  gm.Store32(attrs + 0x14, attrs + 0x20);
  std::memcpy(gm.host(attrs + 0x20), "\\Device\\Cdrom0\\big.bin", 22);
  CHECK(Files().CreateFile(gm, out, 0x80000000, attrs, 0, 0, kOpen, 0) == 0);
  const uint32_t pages = 0x7000, page_a = 0x10000, page_b = 0x20000, iosb = 0x7100;
  gm.Store32(pages, page_a);
  gm.Store32(pages + 4, page_b);
  CHECK(Files().ReadFileScatter(gm, gm.Load32(out), 0, iosb, pages, 0x1800, 0) == 0 && gm.Load32(iosb + 4) == 0x1800);
  CHECK(gm.Load8(page_a + 5) == uint8_t(5 * 7) && gm.Load8(page_b + 0x7FF) == uint8_t(0x17FF * 7));
  CHECK(Files().ReadFileScatter(gm, gm.Load32(out), 0, iosb, pages, 0x10, 0) == kStatusEndOfFile);
  CHECK(Objects().Close(gm.Load32(out)) == 0);
  // Ob references keep an object counted.
  CHECK(Objects().CreateEvent(gm, out, 0, 0, 0) == 0);
  const uint32_t ev = Objects().Lookup(gm.Load32(out));
  Objects().ReferenceObject(ev);
  CHECK(Objects().DereferenceObject(gm, ev) == 0);
  Files().Reset();
  fs::remove_all(root, ec);
  XamState().Reset();
  Memory().Reset();
}

}  // namespace

int main() {
  SonicTests();
  XamTests();
  FileTests();
  MemoryTests();
  SchedulerTests();
  Timers();
  MultipleWaits();
  Events();
  Semaphores();
  Mutants();
  References();
  std::printf("sys_object_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
