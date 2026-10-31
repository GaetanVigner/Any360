// Scenario tests of the object manager against the NT kernel's documented semantics (the reference runtime keeps
// this state in host objects and is not an oracle for it, see docs/SYS.md): sequences of calls checked on statuses,
// returned states and the guest fields. Handles are abstracted: a test keeps what it was given.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <thread>

namespace {

using namespace any360::sys;

std::vector<uint8_t> g_memory(0x20000);
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
  g_memory.resize(0x80000);  // stacks and thread objects need room
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

}  // namespace

int main() {
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
