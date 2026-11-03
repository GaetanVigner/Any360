// Scenario tests of the object manager against the NT kernel's documented semantics (the reference runtime keeps
// this state in host objects and is not an oracle for it, see docs/SYS.md): sequences of calls checked on statuses,
// returned states and the guest fields. Handles are abstracted: a test keeps what it was given.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/audio.h"
#include "any360/sys/files.h"
#include "any360/sys/graphics.h"
#include "any360/sys/memory.h"
#include <filesystem>
#include <fstream>
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/threading.h"
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
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, 0, 0, 0, out + 16) == 0 && g.Load32(out + 16) == 0 && g.Load8(timer + 3) == 1);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == kStatusTimeout);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &half_second) == 0);
  CHECK(g.Load32(timer + 4) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &zero) == 0);
  // Re-arming reports the timer was not armed any more; cancelling an armed one reports it was.
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, 0, 0, 0, out + 16) == 0 && g.Load32(out + 16) == 0 && g.Load32(timer + 4) == 0);
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, 0, 0, 0, out + 16) == 0 && g.Load32(out + 16) == 1);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 0);
  CHECK(Objects().WaitForHandle(g, h, kMainThread, &ten_ms) == kStatusTimeout);
  // A synchronization timer is consumed by the wait; a periodic one re-arms.
  CHECK(Objects().CreateTimer(g, out + 24, 0, 1) == 0);
  const uint32_t hs = g.Load32(out + 24), sync = Objects().Lookup(hs);
  CHECK(g.Load8(sync) == kSynchronizationTimer);
  CHECK(Objects().NtSetTimerEx(g, hs, out + 8, 0, 0, 0, 5, 0) == 0);
  CHECK(Objects().WaitForHandle(g, hs, kMainThread, &half_second) == 0 && g.Load32(sync + 4) == 0 && g.Load8(sync + 3) == 1);
  CHECK(Objects().WaitForHandle(g, hs, kMainThread, &half_second) == 0);
  CHECK(Objects().NtCancelTimer(g, hs, 0) == 0);
  // An absolute due time in the past is due at once; KeInitializeTimerEx on a guest structure.
  const uint32_t kt = kScratch;
  Objects().InitializeTimer(g, kt, 0, 2);
  CHECK(g.Load8(kt) == kNotificationTimer && g.Load8(kt + 2) == 2 && g.Load64(kt + 16) == 0);
  CHECK(Objects().SetTimer(g, kt, int64_t(SystemTime() - 10), 0) == 0);
  CHECK(Objects().WaitForObject(g, kt, kMainThread, &zero) == 0 && g.Load32(kt + 4) == 1);
  CHECK(Objects().NtSetTimerEx(g, h, 0, 0, 0, 0, 0, 0) == kStatusInvalidParameter);
  CHECK(Objects().NtSetTimerEx(g, kHandleBase + 0x800, out + 8, 0, 0, 0, 0, 0) == kStatusInvalidHandle);
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
// Code that needs more than two arguments (APCs, DPCs) is looked up in g_code_args first.
std::map<uint32_t, std::function<uint64_t(uint32_t pcr, uint64_t, uint64_t)>> g_code;
std::map<uint32_t, std::function<uint64_t(uint32_t pcr, uint32_t stack, std::vector<uint64_t>)>> g_code_args;
uint64_t Dispatch(uint32_t address, uint32_t stack, uint32_t pcr, const uint64_t* args, uint32_t count, void*) {
  if (auto it = g_code_args.find(address); it != g_code_args.end()) return it->second(pcr, stack, {args, args + count});
  auto it = g_code.find(address);
  return it == g_code.end() ? 0xDEAD : it->second(pcr, count > 0 ? args[0] : 0, count > 1 ? args[1] : 0);
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
  CHECK(XamState().GetModuleHandle(g, str, out) == 0 && g.Load32(out) != 0);  // a system module, loaded
  std::memcpy(g.host(str), "xbdm.xex", 9);
  CHECK(XamState().GetModuleHandle(g, str, out) == 0xC0000225u);  // not on a retail console
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
  // Winsock events: manual reset, waits by index, timeouts, bad handles.
  {
    const uint32_t a = XamState().WsaCreateEvent(), b = XamState().WsaCreateEvent();
    CHECK(a && b && a != b);
    g.Store32(buf, a);
    g.Store32(buf + 4, b);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 0, 0, kMainThread) == 0x102);
    CHECK(Objects().NtSetEvent(g, b, 0) == 0);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 0, 0, kMainThread) == 1);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 0, 0, kMainThread) == 1);  // manual reset: still signalled
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 1, 0, kMainThread) == 0x102);  // all: a is not
    CHECK(Objects().NtSetEvent(g, a, 0) == 0);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 1, 5, kMainThread) == 0);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 0, buf, 0, 0, kMainThread) == 0xFFFFFFFF && XamState().WsaGetLastError() == 87);
    CHECK(XamState().WsaCloseEvent(a) == 1 && XamState().WsaCloseEvent(b) == 1);
    CHECK(XamState().WsaCloseEvent(a) == 0 && XamState().WsaGetLastError() == 6);
    CHECK(XamState().WsaWaitForMultipleEvents(g, 2, buf, 0, 0, kMainThread) == 0xFFFFFFFF);
  }
  // XNet: random bytes and system link keys.
  std::memset(g.host(buf), 0, 64);
  CHECK(XamState().XNetRandom(g, buf, 64) == 0);
  {
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < 64; ++i) nonzero += g.Load8(buf + i) != 0;
    CHECK(nonzero > 32);
  }
  CHECK(XamState().XNetCreateKey(g, buf, buf + 8) == 0 && (g.Load8(buf) & 0xF0) == 0);
  CHECK(XamState().XNetQosRelease(0) == kStatusInvalidParameter);
  // The profile's Live answers, guide screens, gamer pictures, voice.
  CHECK(XamState().UserAreUsersFriends(g, 0, buf, 0) == 0 && g.Load32(buf) == 0);
  CHECK(XamState().UserAreUsersFriends(g, 1, buf, 0) == kErrorNoSuchUser);
  CHECK(XamState().UserAreUsersFriends(g, 4, buf, 0) == kErrorInvalidParameter);
  CHECK(XamState().UserCreateAchievementEnumerator(g, 0, 0, 8, out, out + 4) == 0 && g.Load32(out) == 8 * 0x24);
  CHECK(XamState().Enumerate(g, g.Load32(out + 4), 0, buf, 8 * 0x24, out + 8, 0) == kErrorNoMoreFiles);
  CHECK(XamState().UserCreateAchievementEnumerator(g, 0, 0, 0, out, out + 4) == kErrorInvalidParameter);
  CHECK(XamState().ShowLiveUI() == 0);
  CHECK(XamState().ParseGamerTileKey(g, buf, out, out + 4, out + 8) == 0 && g.Load32(out) == 0xC0DE0001);
  CHECK(XamState().ReadTileToTexture(g, 0, buf, 16, 4, 0) == kErrorInvalidParameter);
  CHECK(XamState().ReadTileToTexture(g, 0xC0DE0002, buf, 16, 4, 0) == 0 && g.Load32(buf + 60) == 0xFFFFFFFF);
  g.Store32(out, 0xAAAA);
  CHECK(XamState().VoiceCreate(g, out) == kErrorAccessDenied && g.Load32(out) == 0);
  // Images: the title's own and the system modules, by name; nothing else loads.
  std::memcpy(g.host(str), "d:\\Default.xex", sizeof("d:\\Default.xex"));
  CHECK(XamState().LoadImage(g, str, out) == 0 && g.Load32(out) == XamState().title().module_handle);
  std::memcpy(g.host(str), "xam.xex", sizeof("xam.xex"));
  CHECK(XamState().LoadImage(g, str, out) == 0 && g.Load32(out) != 0);
  const uint32_t xam_module = g.Load32(out);
  CHECK(XamState().GetModuleHandle(g, str, out) == 0 && g.Load32(out) == xam_module);
  CHECK(XamState().UnloadImage(xam_module) == 0 && XamState().UnloadImage(0x1234) == kStatusInvalidHandle);
  std::memcpy(g.host(str), "game:\\dlc.xex", sizeof("game:\\dlc.xex"));
  CHECK(XamState().LoadImage(g, str, out) == kStatusNoSuchFile && g.Load32(out) == 0);
  // The cache partition's disk queries.
  CHECK(Files().DeviceIoControl(g, ov, 0x70000, buf, 8) == 0 && g.Load32(buf) == 0xFF000 / 512 && g.Load32(buf + 4) == 512 &&
        g.Load32(ov + 4) == 8);
  CHECK(Files().DeviceIoControl(g, 0, 0x74004, buf, 16) == 0 && g.Load64(buf + 8) == 0xFF000);
  CHECK(Files().DeviceIoControl(g, 0, 0x74004, buf, 8) == 0xC0000023u);
  CHECK(Files().DeviceIoControl(g, 0, 0x12345, buf, 16) == kStatusInvalidParameter);
  // The title id comes from the execution info of the XEX header the title set.
  if (XamState().GetExecutionId(g, out) == 0) {
    CHECK(XamState().TitleId(g) == g.Load32(g.Load32(out) + 12));
  } else {
    CHECK(XamState().TitleId(g) == 0);
  }
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

// The kernel side of graphics and audio: Vd* setup handed to a GPU backend, the interrupt callback run at dispatch
// level on a context of its own, XAudio clients pumped and their frames deinterleaved to a sink, XMA contexts.
struct FakeGpu : GpuBackend {
  uint32_t ring = 0, ring_log2 = 0, write_back = 0, block_log2 = 0;
  void InitializeRingBuffer(Guest, uint32_t base, uint32_t size_log2) override { ring = base, ring_log2 = size_log2; }
  void EnableReadPointerWriteBack(Guest, uint32_t address, uint32_t log2) override { write_back = address, block_log2 = log2; }
};
struct FakeSink : AudioSink {
  int frames = 0;
  uint32_t client = 99;
  std::vector<float> last;
  void Submit(uint32_t c, const float* interleaved) override {
    ++frames, client = c;
    last.assign(interleaved, interleaved + kAudioFrameSamples * kAudioChannels);
  }
};
struct FakeXma : XmaBackend {
  int allocated = 0, released = 0, enabled = 0, disabled = 0;
  void ContextAllocated(Guest, uint32_t) override { ++allocated; }
  void ContextReleased(Guest, uint32_t) override { ++released; }
  void ContextEnabled(Guest, uint32_t) override { ++enabled; }
  void ContextDisabled(Guest, uint32_t) override { ++disabled; }
};

uint32_t FloatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

void GraphicsAudioTests() {
  Fresh();
  std::vector<uint8_t> memory(0x200000);
  const Guest gm{memory.data()};
  Memory().Reset({{0x00000000, 0x80000, 0x1000, false}, {0x100000, 0x40000, 0x1000, true}});
  Objects().SetKernelHeap(0x80000, 0x70000);
  Objects().SetGuest(gm);
  Objects().Reset();
  Threads().SetDispatcher(&Dispatch, nullptr);

  // Graphics.
  Graphics().Reset();
  FakeGpu gpu;
  Graphics().SetBackend(&gpu);
  CHECK(Graphics().InitializeEngines() == 1);
  Graphics().InitializeRingBuffer(gm, 0x1100000, 12);
  Graphics().EnableReadPointerWriteBack(gm, 0x1000F00, 6);
  CHECK(gpu.ring == 0x1100000 && gpu.ring_log2 == 12 && gpu.write_back == 0x1000F00 && gpu.block_log2 == 6);
  CHECK(Graphics().ring_base() == 0x1100000 && Graphics().write_back() == 0x1000F00);
  CHECK(!Graphics().RaiseInterrupt(gm, 0));  // no callback yet
  std::atomic<int> interrupts{0};
  uint64_t source = 99, user = 0;
  uint32_t irql = 0, dpc = 0, interrupt_pcr = 0;
  g_code[0x5000] = [&](uint32_t pcr, uint64_t a0, uint64_t a1) -> uint64_t {
    source = a0, user = a1, interrupt_pcr = pcr;
    irql = gm.Load8(pcr + 0x18), dpc = gm.Load32(pcr + 0x150);
    ++interrupts;
    return 0;
  };
  Graphics().SetInterruptCallback(gm, 0x5000, 0x1234);
  CHECK(Graphics().interrupt_callback() == 0x5000);
  CHECK(Graphics().RaiseInterrupt(gm, 1));
  CHECK(interrupts == 1 && source == 1 && user == 0x1234 && irql == 2 && dpc == 1);
  CHECK(interrupt_pcr && gm.Load8(interrupt_pcr + 0x18) == 0 && gm.Load32(interrupt_pcr + 0x150) == 0);
  // Its context is a KTHREAD of the title process, current on its PCR.
  const uint32_t kthread = gm.Load32(interrupt_pcr + 0x100);
  CHECK(kthread && gm.Load32(kthread + 0x84) == Threads().process() && gm.Load32(interrupt_pcr + 0x30 + 4) == interrupt_pcr);
  Graphics().StartVblank(1000.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  Graphics().StopVblank();
  CHECK(interrupts > 5 && source == 0);
  // A host's interrupt dispatch runs the callback instead of the layer's context.
  {
    struct Raised {
      uint32_t callback = 0, source = 0, user_data = 0, count = 0;
    } raised;
    Graphics().SetInterruptDispatch(
        [](uint32_t callback, uint32_t source, uint32_t user_data, void* user) {
          auto& r = *static_cast<Raised*>(user);
          r = {callback, source, user_data, r.count + 1};
        },
        &raised);
    const int before = interrupts;
    CHECK(Graphics().RaiseInterrupt(gm, 1));
    CHECK(raised.count == 1 && raised.callback == 0x5000 && raised.source == 1 && raised.user_data == 0x1234);
    CHECK(interrupts == before);
    Graphics().SetInterruptDispatch(nullptr, nullptr);
  }
  // VdPersistDisplay: a 64-byte physical block, then 1.
  const uint32_t out = 0x1800;
  CHECK(Graphics().PersistDisplay(gm, out) == 1);
  // VdSwap: the front buffer (a physical allocation) through its fetch constant, written into the ring as the GPU's
  // swap packets.
  {
    const uint32_t front = Memory().AllocatePhysical(gm, 0, 0x1000, 0x4, 0, 0xFFFFFFFF, 0x1000);
    const uint32_t fetch = 0x2400, ring = 0x2800, w = 0x2440, h = 0x2444;
    const uint32_t d[6] = {0x80400002, 0x00000086 | front, 0x0059E4FF, 0x00000D10, 0, 0x00000200};
    for (uint32_t i = 0; i < 6; ++i) gm.Store32(fetch + 4 * i, d[i]);
    gm.Store32(w, 1280);
    gm.Store32(h, 720);
    CHECK(Graphics().Swap(gm, ring, fetch, 0, w, h));
    const uint32_t physical = Memory().PhysicalAddress(front);
    CHECK(gm.Load32(ring) == 0x00054800 && gm.Load32(ring + 4) == d[0] && gm.Load32(ring + 8) == (0x86u | physical));
    CHECK(gm.Load32(ring + 24) == d[5] && gm.Load32(ring + 28) == 0xC0036400 && gm.Load32(ring + 32) == 0x53574150);
    CHECK(gm.Load32(ring + 36) == physical && gm.Load32(ring + 40) == 1280 && gm.Load32(ring + 44) == 720);
    CHECK(gm.Load32(ring + 48) == 0x80000000 && gm.Load32(ring + 63 * 4) == 0x80000000);
    gm.Store32(fetch + 4, 0x00000086 | 0x7000);  // not physical memory: refused
    CHECK(!Graphics().Swap(gm, ring, fetch, 0, w, h));
  }
  const uint32_t persisted = gm.Load32(out);
  CHECK(persisted >= 0x100000 && persisted < 0x140000 && Memory().QueryProtect(persisted) != 0);
  Memory().FreePhysical(0, persisted);
  CHECK(Memory().QueryProtect(persisted) == 0);

  // Audio clients.
  Audio().Reset();
  FakeSink sink;
  Audio().SetSink(&sink);
  const uint32_t callback = 0x2000, driver = 0x2010, driver2 = 0x2014;
  CHECK(Audio().RegisterClient(gm, 0, driver) == 0x80070057);
  CHECK(Audio().RegisterClient(gm, callback, driver) == 0x80070057);  // a null callback
  std::atomic<int> mixes{0};
  uint64_t argument = 0;
  g_code[0x6000] = [&](uint32_t, uint64_t a0, uint64_t) -> uint64_t {
    argument = a0;
    ++mixes;
    return 0;
  };
  gm.Store32(callback, 0x6000);
  gm.Store32(callback + 4, 0xABCD);
  CHECK(Audio().RegisterClient(gm, callback, driver) == 0 && gm.Load32(driver) == 0x41550000);
  CHECK(Audio().RegisterClient(gm, callback, driver2) == 0 && gm.Load32(driver2) == 0x41550001);
  Audio().Pump(gm);
  CHECK(mixes == 2 && argument == 0xABCD);
  CHECK(Audio().UnregisterClient(0x41550001) == 0);
  CHECK(Audio().UnregisterClient(0x12340001) == 0x80070057);
  Audio().Pump(gm);
  CHECK(mixes == 3);
  // A frame: channel after channel in the guest, sample after sample for the sink.
  const uint32_t samples = 0x3000;
  for (uint32_t ch = 0; ch < kAudioChannels; ++ch)
    for (uint32_t s = 0; s < kAudioFrameSamples; ++s) gm.Store32(samples + 4 * (ch * kAudioFrameSamples + s), FloatBits(float(ch * 1000 + s)));
  CHECK(Audio().SubmitFrame(gm, 0x41550000, samples) == 0);
  CHECK(sink.frames == 1 && sink.client == 0 && sink.last.size() == kAudioFrameSamples * kAudioChannels);
  CHECK(sink.last[0] == 0.0f && sink.last[1] == 1000.0f && sink.last[5] == 5000.0f && sink.last[6] == 1.0f &&
        sink.last[255 * 6 + 3] == 3255.0f);
  CHECK(Audio().SubmitFrame(gm, 0x41550001, samples) == 0x80070057);  // unregistered
  CHECK(sink.frames == 1);
  gm.Store32(out, 0xFFFFFFFF);
  CHECK(Audio().GetVoiceCategoryVolumeChangeMask(gm, 0x41550000, out) == 0 && gm.Load32(out) == 0);
  CHECK(Audio().GetVoiceCategoryVolume(gm, out) == 0 && gm.Load32(out) == 0x3F800000);
  CHECK(Audio().GetSpeakerConfig(gm, out) == 0 && gm.Load32(out) == 0x00010001);
  // The audio clock calls the client every 5.33 ms.
  const int before = mixes;
  Audio().Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  Audio().Stop();
  CHECK(mixes - before >= 5);

  // XMA contexts: 64-byte blocks in physical memory, reused once released.
  FakeXma xma;
  Audio().SetXmaBackend(&xma);
  CHECK(Audio().XmaCreateContext(gm, out) == 0);
  const uint32_t a = gm.Load32(out);
  CHECK(Audio().XmaCreateContext(gm, out) == 0);
  const uint32_t b = gm.Load32(out);
  CHECK(a >= 0x100000 && a < 0x140000 && a % kXmaContextSize == 0 && b == a + kXmaContextSize);
  gm.Store32(a, 0x12345678);
  CHECK(Audio().XmaReleaseContext(gm, a) == 0 && gm.Load32(a) == 0);
  CHECK(Audio().XmaReleaseContext(gm, a) == 0);  // twice: nothing more
  CHECK(Audio().XmaCreateContext(gm, out) == 0 && gm.Load32(out) == a);
  CHECK(xma.allocated == 3 && xma.released == 1);

  // A context initialised from its init block: buffers as physical addresses, fields packed into their words.
  const uint32_t init = 0x2000;
  std::memset(gm.host(init), 0, 56);
  gm.Store32(init, 0x110000);       // input buffer 0
  gm.Store32(init + 4, 3);          // its packets
  gm.Store32(init + 0x10, 0x40);    // input read offset, in bits
  gm.Store32(init + 0x14, 0x120000);  // output buffer
  gm.Store32(init + 0x18, 8);       // output blocks
  gm.Store32(init + 0x24, 1);       // stereo
  gm.Store32(init + 0x28, 2);       // 44.1 kHz
  gm.Store32(init + 0x2C, 0x100);   // loop start
  gm.Store8(init + 0x34, 255);      // loop count
  gm.Store32(a, 0xFFFFFFFF);
  CHECK(Audio().XmaInitializeContext(gm, a, init) == 0);
  CHECK(gm.Load32(a) == (3u | 255u << 12 | 8u << 22) && gm.Load32(a + 4) == (2u << 27 | 1u << 29) && gm.Load32(a + 8) == 0x40);
  CHECK(gm.Load32(a + 12) == 0x100 && gm.Load32(a + 20) == 0x10000 && gm.Load32(a + 24) == 0 && gm.Load32(a + 28) == 0x20000);
  gm.Store32(init + 0x14, 0x1000);  // an output buffer outside physical memory
  CHECK(Audio().XmaInitializeContext(gm, a, init) == 0x80000000u && gm.Load32(a + 28) == 0x20000);
  // Buffer validity and offsets, each in its own bits.
  CHECK(Audio().XmaGet(gm, a, XmaField::kInput0Valid) == 0 && Audio().XmaGet(gm, a, XmaField::kOutputValid) == 0);
  Audio().XmaSet(gm, a, XmaField::kInput0Valid, 1);
  Audio().XmaSet(gm, a, XmaField::kOutputValid, 1);
  Audio().XmaSet(gm, a, XmaField::kOutputReadOffset, 33);  // 5 bits: wraps to 1
  CHECK(Audio().XmaGet(gm, a, XmaField::kInput0Valid) == 1 && Audio().XmaGet(gm, a, XmaField::kInput1Valid) == 0 &&
        Audio().XmaGet(gm, a, XmaField::kInput0PacketCount) == 3 && gm.Load32(a + 4) >> 31 == 1);
  CHECK(Audio().XmaGet(gm, a, XmaField::kOutputReadOffset) == 1 && Audio().XmaGet(gm, a, XmaField::kOutputWriteOffset) == 0);
  CHECK(Audio().XmaSetInputBuffer(gm, a, 1, 0x130000, 5) == 0 && gm.Load32(a + 24) == 0x30000 &&
        Audio().XmaGet(gm, a, XmaField::kInput1PacketCount) == 5);
  CHECK(Audio().XmaSetInputBuffer(gm, a, 1, 0x1000, 5) == 0x80000000u);
  // Enabled and disabled for the decoder; with no work buffer nothing is in use.
  CHECK(Audio().XmaEnableContext(gm, a) == 0 && Audio().XmaEnabled(a) && !Audio().XmaEnabled(b));
  CHECK(Audio().XmaDisableContext(gm, a) == 0 && !Audio().XmaEnabled(a) && xma.enabled == 1 && xma.disabled == 1);
  CHECK(Audio().XmaBlockWhileInUse(gm, a) == 0);

  Audio().Reset();
  Graphics().Reset();
  Threads().Shutdown();
  Memory().Reset();
}

}  // namespace

// The calls Test Drive Unlimited still needed.
void TduTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  Files().Reset();
  std::error_code ec;
  const uint32_t out = kScratch + 0x200, ov = kScratch + 0x300, buf = kScratch + 0x400, str = kScratch + 0x800;
  // DPCs: initialised as the kernel lays them out, run in order on a context of their own at dispatch level; one queued
  // behind a running DPC is queued once and can be removed.
  Threads().SetDispatcher(&Dispatch, nullptr);
  const uint32_t dpc = 0x30000, dpc2 = 0x30020;
  std::memset(g.host(dpc), 0xEE, 0x1C);
  KeInitializeDpc(g, dpc, 0x82000000, 0x1234);
  KeInitializeDpc(g, dpc2, 0x82000100, 0x5678);
  CHECK(g.Load16(dpc) == 19 && g.Load32(dpc + 4) == 0 && g.Load32(dpc + 0xC) == 0x82000000 && g.Load32(dpc + 0x10) == 0x1234);
  std::atomic<bool> release{false}, started{false};
  std::vector<uint64_t> seen;
  uint32_t dpc_irql = 0, dpc_active = 0;
  g_code_args[0x82000000] = [&](uint32_t pcr, uint32_t, std::vector<uint64_t> args) -> uint64_t {
    seen = args, dpc_irql = g.Load8(pcr + 0x18), dpc_active = g.Load32(pcr + 0x150);
    started = true;
    while (!release) std::this_thread::yield();
    return 0;
  };
  std::atomic<int> second_runs{0};
  g_code_args[0x82000100] = [&](uint32_t, uint32_t, std::vector<uint64_t>) -> uint64_t { return ++second_runs, 0; };
  CHECK(KeInsertQueueDpc(g, dpc, 7, 8) == 1 && g.Load32(dpc + 0x14) == 7 && g.Load32(dpc + 0x18) == 8);
  for (int i = 0; i < 1000 && !started; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(started && seen == std::vector<uint64_t>({dpc, 0x1234, 7, 8}) && dpc_irql == 2 && dpc_active == 1);
  CHECK(KeInsertQueueDpc(g, dpc2, 1, 2) == 1 && KeInsertQueueDpc(g, dpc2, 9, 9) == 0 && g.Load32(dpc2 + 0x14) == 1);
  CHECK(KeRemoveQueueDpc(g, dpc2) == 1 && KeRemoveQueueDpc(g, dpc2) == 0);
  CHECK(KeInsertQueueDpc(g, dpc2, 3, 4) == 1);
  release = true;
  for (int i = 0; i < 1000 && second_runs == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(second_runs == 1 && KeRemoveQueueDpc(g, dpc2) == 0);
  ResetDpcs();
  g_code_args.clear();
  // Named objects: opened (a new handle) and referenced by name.
  CHECK(Objects().CreateEvent(g, out, Named("\\TduEvent"), 0, 0) == 0);
  const uint32_t named = g.Load32(out);
  CHECK(Objects().OpenObjectByName(g, Named("\\TduEvent"), out + 4) == 0);
  CHECK(g.Load32(out + 4) != named && Objects().Lookup(g.Load32(out + 4)) == Objects().Lookup(named));
  CHECK(Objects().ReferenceObjectByName(g, kScratch + 0x120, out + 8) == 0 && g.Load32(out + 8) == Objects().Lookup(named));
  CHECK(Objects().DereferenceObject(g, g.Load32(out + 8)) == 0);
  CHECK(Objects().OpenObjectByName(g, Named("\\Missing"), out + 4) == 0xC0000034u);
  CHECK(Objects().ReferenceObjectByName(g, kScratch + 0x120, out + 8) == 0xC0000034u);
  CHECK(Objects().OpenObjectByName(g, 0, out + 4) == kStatusInvalidParameter);
  // Completing a request by hand, then reading its result back; a pending one, with and without waiting.
  CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
  const uint32_t event = g.Load32(out);
  std::memset(g.host(ov), 0, 0x1C);
  g.Store32(ov + 0x0C, event);
  CHECK(XamState().MsgCompleteIORequest(g, ov, 0, 0x5, 12) == 0 && g.Load32(ov) == 0 && g.Load32(ov + 4) == 12 && g.Load32(ov + 0x18) == 5);
  const int64_t zero = 0;
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &zero) == 0);
  CHECK(XamState().GetOverlappedResult(g, ov, out + 4, 0, kMainThread) == 0 && g.Load32(out + 4) == 12);
  CHECK(Objects().NtClearEvent(g, event) == 0);
  g.Store32(ov, kErrorIoPending);
  CHECK(XamState().GetOverlappedResult(g, ov, out + 4, 0, kMainThread) == 0x3E4);
  std::thread completer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    XamState().MsgCompleteIORequest(g, ov, kErrorNoMoreFiles, 0, 0);
  });
  CHECK(XamState().GetOverlappedResult(g, ov, out + 4, 1, kMainThread) == kErrorNoMoreFiles);
  completer.join();
  // The virtual keyboard accepts its default text at once, truncated to the buffer.
  const uint32_t l = XamState().NotifyCreateListener(g, 1, 0);
  const char16_t hello[] = u"Hello";
  for (int i = 0; i < 6; ++i) g.Store16(str + 2 * i, uint16_t(hello[i]));
  std::memset(g.host(buf), 0xEE, 32);
  CHECK(XamState().ShowKeyboardUI(g, str, buf, 3, 0) == 0 && g.Load16(buf) == 'H' && g.Load16(buf + 2) == 'e' && g.Load16(buf + 4) == 0);
  CHECK(XamState().ShowKeyboardUI(g, str, buf, 16, 0) == 0 && g.Load16(buf + 8) == 'o' && g.Load16(buf + 10) == 0 && g.Load16(buf + 30) == 0);
  CHECK(XamState().ShowKeyboardUI(g, 0, buf, 16, 0) == 0 && g.Load16(buf) == 0);
  CHECK(XamState().ShowKeyboardUI(g, str, 0, 16, 0) == kErrorInvalidParameter);
  std::memset(g.host(ov), 0xEE, 0x1C);
  g.Store32(ov + 0x0C, 0);
  CHECK(XamState().ShowKeyboardUI(g, str, buf, 16, ov) == kErrorIoPending && g.Load32(ov) == 0);
  CHECK(XamState().NotifyGetNext(g, l, kNotifySystemUi, out + 4, out + 8) == 1 && g.Load32(out + 8) == 1);
  // Content: the license mask, a package's thumbnail.
  CHECK(XamState().ContentGetLicenseMask(g, out, 0) == 0 && g.Load32(out) == 0);
  CHECK(XamState().ContentGetLicenseMask(g, out, ov) == kErrorIoPending && g.Load32(ov) == 0);
  const fs::path content = fs::temp_directory_path() / "any360-sys-content3";
  fs::remove_all(content, ec);
  fs::create_directories(content / "00000001" / "slot1");
  XamState().title().content_root = content;
  const uint32_t data = 0x6000;
  std::memset(g.host(data), 0, 0x134);
  g.Store32(data + 4, 1);
  std::memcpy(g.host(data + 0x108), "slot1", 5);
  std::memcpy(g.host(buf), "\x89PNG", 4);
  CHECK(XamState().ContentSetThumbnail(g, data, buf, 4, 0) == 0 && fs::file_size(content / "00000001" / "slot1.png", ec) == 4);
  std::memcpy(g.host(data + 0x108), "slot9", 5);
  CHECK(XamState().ContentSetThumbnail(g, data, buf, 4, 0) == kErrorFileNotFound);
  // Gather writes and the image headers of an XEX, on a mounted folder.
  const fs::path root = fs::temp_directory_path() / "any360-sys-tdu";
  fs::remove_all(root, ec);
  fs::create_directories(root);
  {
    std::ofstream f(root / "out.bin", std::ios::binary);
    const uint8_t xex[0x40] = {'X', 'E', 'X', '2', 0, 0, 0, 1, 0, 0, 0, 0x20};
    std::ofstream x(root / "default.xex", std::ios::binary);
    x.write(reinterpret_cast<const char*>(xex), sizeof xex);
    std::ofstream bad(root / "bad.xex", std::ios::binary);
    bad.write("MZ\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 24);
  }
  Files().Mount("\\Device\\Harddisk0\\Partition1", root, false);
  const uint32_t attrs = 0x6400;
  const auto path = [&](const char* text) {
    g.Store32(attrs, 0);
    g.Store32(attrs + 4, attrs + 0x10);
    g.Store16(attrs + 0x10, uint16_t(std::strlen(text)));
    g.Store32(attrs + 0x14, attrs + 0x20);
    std::memcpy(g.host(attrs + 0x20), text, std::strlen(text));
    return attrs + 0x10;  // the ANSI_STRING
  };
  path("\\Device\\Harddisk0\\Partition1\\out.bin");
  CHECK(Files().CreateFile(g, out, 0x40000000, attrs, 0, 0, kOpen, 0) == 0);
  const uint32_t pages = 0x6800, page_a = 0x20000, page_b = 0x21000, iosb = 0x6900;
  for (uint32_t i = 0; i < 0x1000; ++i) g.Store8(page_a + i, uint8_t(i)), g.Store8(page_b + i, uint8_t(i * 3));
  g.Store32(pages, page_a);
  g.Store32(pages + 4, page_b);
  CHECK(Files().WriteFileGather(g, g.Load32(out), 0, iosb, pages, 0x1800, 0) == 0 && g.Load32(iosb + 4) == 0x1800);
  CHECK(Objects().Close(g.Load32(out)) == 0);
  {
    std::ifstream f(root / "out.bin", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() == 0x1800 && uint8_t(bytes[0x10]) == 0x10 && uint8_t(bytes[0x1010]) == uint8_t(0x10 * 3));
  }
  CHECK(Files().WriteFileGather(g, 0x1234, 0, iosb, pages, 0x10, 0) == kStatusInvalidHandle);
  std::memset(g.host(buf), 0, 0x40);
  CHECK(XamState().LoadImageHeaders(g, path("\\Device\\Harddisk0\\Partition1\\default.xex"), buf, 0x40) == 0 && g.Load32(buf) == 0x58455832 &&
        g.Load32(buf + 8) == 0x20);
  CHECK(XamState().LoadImageHeaders(g, path("\\Device\\Harddisk0\\Partition1\\default.xex"), buf, 0x10) == 0xC0000023u);
  CHECK(XamState().LoadImageHeaders(g, path("\\Device\\Harddisk0\\Partition1\\bad.xex"), buf, 0x40) == 0xC000007Bu);
  CHECK(XamState().LoadImageHeaders(g, path("\\Device\\Harddisk0\\Partition1\\none.xex"), buf, 0x40) == kStatusNoSuchFile);
  // The synchronous form of the disk queries.
  CHECK(Files().SynchronousDeviceIoControl(g, 0x70000, buf, 8, out) == 0 && g.Load32(out) == 8 && g.Load32(buf + 4) == 512);
  CHECK(Files().SynchronousDeviceIoControl(g, 0x12345, buf, 8, out) == kStatusInvalidParameter);
  Files().Reset();
  XamState().Reset();
  fs::remove_all(root, ec);
  fs::remove_all(content, ec);
}

// The calls The Simpsons Game still needed outside graphics and audio.
void SimpsonsTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  Files().Reset();
  std::error_code ec;
  std::vector<uint8_t> memory(0x200000);
  const Guest gm{memory.data()};
  Memory().Reset({{0x00000000, 0x80000, 0x1000, false}, {0x80000, 0x80000, 0x10000, false}});
  Objects().SetKernelHeap(0x100000, 0x10000);
  Objects().SetGuest(gm);
  Objects().Reset();
  const uint32_t out = 0x1800, ov = 0x1900, buf = 0x2000;
  // Winsock events set and reset by handle; a bad handle is FALSE and ERROR_INVALID_HANDLE.
  const uint32_t ev = XamState().WsaCreateEvent();
  CHECK(XamState().WsaSetEvent(gm, ev) == 1 && gm.Load32(Objects().Lookup(ev) + 4) == 1);
  CHECK(XamState().WsaResetEvent(gm, ev) == 1 && gm.Load32(Objects().Lookup(ev) + 4) == 0);
  CHECK(XamState().WsaSetEvent(gm, 0x1234) == 0 && XamState().WsaGetLastError() == 6);
  XamState().WsaSetLastError(10035);
  CHECK(XamState().WsaGetLastError() == 10035);
  // Lookups complete at once with no answer; their records go back to the pool.
  CHECK(XamState().XNetDnsLookup(gm, ev, out) == 0 && gm.Load32(gm.Load32(out)) == 10050 && gm.Load32(gm.Load32(out) + 4) == 0);
  CHECK(gm.Load32(Objects().Lookup(ev) + 4) == 1 && XamState().XNetQosRelease(gm.Load32(out)) == 0);
  CHECK(XamState().WsaResetEvent(gm, ev) == 1);
  CHECK(XamState().XNetQosServiceLookup(gm, ev, out) == 0 && gm.Load32(out) != 0 && gm.Load32(gm.Load32(out)) == 0);
  CHECK(gm.Load32(Objects().Lookup(ev) + 4) == 1 && XamState().XNetQosRelease(gm.Load32(out)) == 0);
  // Startup parameters read back; another option, or a short buffer, refused.
  gm.Store8(buf, 13);
  gm.Store8(buf + 1, 0x01);
  gm.Store8(buf + 2, 7);
  CHECK(XamState().XNetStartup(gm, buf) == 0);
  gm.Store32(out, 4);
  CHECK(XamState().XNetGetOpt(gm, 1, buf + 0x100, out) == 10040 && gm.Load32(out) == 13);
  CHECK(XamState().XNetGetOpt(gm, 1, buf + 0x100, out) == 0 && gm.Load8(buf + 0x100) == 13 && gm.Load8(buf + 0x102) == 7);
  CHECK(XamState().XNetGetOpt(gm, 5, buf + 0x100, out) == 10022);
  CHECK(XamState().LogonGetMachineId(gm, out) == 0 && gm.Load64(out) == XamState().title().machine_id);
  CHECK(XamState().MsgSystemProcessCall(0xFE, 0x1234, buf, 4) == 0);
  // Devices content can be stored on: the hard drive, by enumerator and by name.
  CHECK(XamState().ContentCreateDeviceEnumerator(gm, 4, out, out + 4) == 0 && gm.Load32(out) == 80 * 4);
  CHECK(XamState().Enumerate(gm, gm.Load32(out + 4), 0, buf, 80 * 4, out + 8, 0) == 0 && gm.Load32(out + 8) == 1 && gm.Load32(buf) == 1 &&
        gm.Load16(buf + 24) == 'H');
  CHECK(XamState().ContentGetDeviceName(gm, 1, buf, 32) == 0 && gm.Load16(buf) == 'H' && gm.Load16(buf + 20) == 0);
  CHECK(XamState().ContentGetDeviceName(gm, 1, buf, 5) == kErrorInsufficientBuffer);
  CHECK(XamState().ContentGetDeviceName(gm, 7, buf, 32) == kErrorDeviceNotConnected);
  // A package's thumbnail read back, and its files reached through the path it resolves to.
  const fs::path content = fs::temp_directory_path() / "any360-sys-content4";
  fs::remove_all(content, ec);
  fs::create_directories(content / "00000001" / "slot1");
  {
    std::ofstream f(content / "00000001" / "slot1" / "save.dat", std::ios::binary);
    f << "data";
  }
  XamState().title().content_root = content;
  const uint32_t data = 0x5000;
  std::memset(gm.host(data), 0, 0x134);
  gm.Store32(data + 4, 1);
  std::memcpy(gm.host(data + 0x108), "slot1", 5);
  gm.Store32(out, 64);
  CHECK(XamState().ContentGetThumbnail(gm, data, buf, out, 0) == kErrorFileNotFound);
  std::memcpy(gm.host(buf), "\x89PNG!", 5);
  CHECK(XamState().ContentSetThumbnail(gm, data, buf, 5, 0) == 0);
  std::memset(gm.host(buf), 0, 8);
  gm.Store32(out, 0);
  CHECK(XamState().ContentGetThumbnail(gm, data, 0, out, 0) == 0 && gm.Load32(out) == 5);
  gm.Store32(out, 4);
  CHECK(XamState().ContentGetThumbnail(gm, data, buf, out, 0) == kErrorInsufficientBuffer);
  gm.Store32(out, 64);
  gm.Store32(ov + 0x0C, 0);
  CHECK(XamState().ContentGetThumbnail(gm, data, buf, out, ov) == kErrorIoPending && gm.Load32(ov) == 0 && gm.Load8(buf + 4) == '!');
  CHECK(XamState().ContentResolve(gm, data, buf, 8) == kErrorInsufficientBuffer);
  CHECK(XamState().ContentResolve(gm, data, buf, 260) == 0);
  const std::string resolved(reinterpret_cast<const char*>(gm.host(buf)));
  CHECK(resolved == "\\Device\\Content\\00000001\\slot1");
  CHECK(Files().Resolve(resolved + "\\save.dat") == content / "00000001" / "slot1" / "save.dat");
  std::memcpy(gm.host(data + 0x108), "slot9", 5);
  CHECK(XamState().ContentResolve(gm, data, buf, 260) == kErrorFileNotFound);
  // Protection changes on committed pages, rounded out to whole pages; never execute rights, never free pages.
  gm.Store32(out, 0x10000);
  gm.Store32(out + 4, 0x3000);
  CHECK(Memory().AllocateVirtual(gm, out, out + 4, 0x3000, kPageReadWrite, 0) == 0);
  gm.Store32(out, 0x10800);
  gm.Store32(out + 4, 0x1000);
  CHECK(Memory().ProtectVirtual(gm, out, out + 4, kPageReadOnly, out + 8) == 0 && gm.Load32(out + 8) == kPageReadWrite);
  CHECK(gm.Load32(out) == 0x10000 && gm.Load32(out + 4) == 0x2000 && Memory().QueryProtect(0x11000) == kPageReadOnly &&
        Memory().QueryProtect(0x12000) == kPageReadWrite);
  gm.Store32(out, 0x10000);
  gm.Store32(out + 4, 0x1000);
  CHECK(Memory().ProtectVirtual(gm, out, out + 4, kPageExecuteRead, 0) == 0xC0000045u);
  gm.Store32(out + 4, 0x8000);
  CHECK(Memory().ProtectVirtual(gm, out, out + 4, kPageReadOnly, 0) == 0xC000002Du);
  gm.Store32(out + 4, 0);
  CHECK(Memory().ProtectVirtual(gm, out, out + 4, kPageReadOnly, 0) == kStatusInvalidParameter);
  // Signal one object and wait on another.
  CHECK(Objects().CreateEvent(gm, out, 0, 1, 0) == 0 && Objects().CreateSemaphore(gm, out + 4, 0, 1, 2) == 0);
  const uint32_t sync = gm.Load32(out), sem = gm.Load32(out + 4);
  const int64_t zero = 0;
  CHECK(Objects().SignalAndWait(gm, sync, sem, kMainThread, &zero) == 0 && gm.Load32(Objects().Lookup(sync) + 4) == 1);
  CHECK(Objects().SignalAndWait(gm, sem, sync, kMainThread, &zero) == 0 && gm.Load32(Objects().Lookup(sync) + 4) == 0 &&
        gm.Load32(Objects().Lookup(sem) + 4) == 1);
  CHECK(Objects().SignalAndWait(gm, sync, 0x1234, kMainThread, &zero) == kStatusInvalidHandle);
  // Handles on objects by address; an unknown address is refused.
  CHECK(Objects().OpenObjectByPointer(gm, Objects().Lookup(sync), out + 8) == 0 && gm.Load32(out + 8) != sync &&
        Objects().Lookup(gm.Load32(out + 8)) == Objects().Lookup(sync));
  CHECK(Objects().OpenObjectByPointer(gm, 0x7000, out + 8) == 0xC0000001u);
  // APCs queue on a thread by handle; an event handle is no thread.
  const uint32_t kthread = Objects().AllocateObject(0x200);
  gm.Store8(kthread, kThread);
  const uint32_t thread_handle = Objects().OpenHandle(kthread);
  CHECK(Threads().QueueApc(gm, thread_handle, 0, 0x82000000, 1, 2, 3) == 0 && Threads().PendingApcs(kthread) == 1);
  CHECK(Threads().QueueApc(gm, kCurrentThreadHandle, kthread, 0x82000000, 1, 2, 3) == 0 && Threads().PendingApcs(kthread) == 2);
  CHECK(Threads().QueueApc(gm, sync, 0, 0x82000000, 0, 0, 0) == kStatusObjectTypeMismatch);
  CHECK(Threads().QueueApc(gm, 0x1234, 0, 0x82000000, 0, 0, 0) == kStatusInvalidHandle);
  // Alertable delays and waits end once an APC is queued; delivery runs the thread's APCs in order below its stack.
  const uint32_t pcr = Objects().AllocateObject(0x200);
  gm.Store32(pcr + 0x100, kthread);
  Threads().SetDispatcher(&Dispatch, nullptr);
  std::vector<std::vector<uint64_t>> delivered;
  uint32_t apc_stack = 0;
  g_code_args[0x82000000] = [&](uint32_t, uint32_t stack, std::vector<uint64_t> args) -> uint64_t {
    delivered.push_back(args), apc_stack = stack;
    return 0;
  };
  const int64_t second = -10000000;
  CHECK(Threads().Delay(gm, pcr, second, true) == kStatusUserApc);
  CHECK(Objects().WaitForHandle(gm, sync, kthread, &second, true) == kStatusUserApc);
  CHECK(Threads().DeliverApcs(gm, pcr, 0x7F000) == 2 && Threads().PendingApcs(kthread) == 0);
  CHECK(delivered.size() == 2 && delivered[0] == std::vector<uint64_t>({1, 2, 3}) && apc_stack == 0x7EC00);
  const int64_t short_wait = -20000;  // 2 ms
  CHECK(Threads().Delay(gm, pcr, short_wait, true) == 0);
  CHECK(Objects().WaitForHandle(gm, sync, kthread, &short_wait, true) == kStatusTimeout);
  // An APC queued by another thread ends an alertable delay already under way.
  std::thread queuer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Threads().QueueApc(gm, thread_handle, 0, 0x82000000, 4, 5, 6);
  });
  CHECK(Threads().Delay(gm, pcr, second, true) == kStatusUserApc);
  queuer.join();
  CHECK(Threads().DeliverApcs(gm, pcr, 0x7F000) == 1 && delivered.back() == std::vector<uint64_t>({4, 5, 6}));
  // A timer's APC is queued on the thread that set it when it expires, with the system time; an alertable delay sees it.
  CHECK(Objects().CreateTimer(gm, out, 0, 0) == 0);
  const uint32_t timer_handle = gm.Load32(out);
  gm.Store64(out + 8, uint64_t(int64_t(-100000)));  // 10 ms
  const uint64_t before = SystemTime();
  CHECK(Objects().NtSetTimerEx(gm, timer_handle, out + 8, 0x82000000, 9, kthread, 0, 0) == 0);
  CHECK(Threads().Delay(gm, pcr, second, true) == kStatusUserApc && Threads().DeliverApcs(gm, pcr, 0x7F000) == 1);
  const uint64_t fired = delivered.back()[1] | delivered.back()[2] << 32;
  CHECK(delivered.back()[0] == 9 && fired >= before && fired <= SystemTime());
  // I/O completion APCs: queued for a successful transfer with a routine (low bit dropped) and a context only.
  Threads().QueueIoApc(kthread, 0x82000001, 0x55, 0x1800, 0);
  Threads().QueueIoApc(kthread, 0x82000000, 0x55, 0x1800, 0xC0000011);  // end of file
  Threads().QueueIoApc(kthread, 0x82000000, 0, 0x1800, 0);
  Threads().QueueIoApc(kthread, 1, 0x55, 0x1800, 0);
  CHECK(Threads().PendingApcs(kthread) == 1 && Threads().DeliverApcs(gm, pcr, 0x7F000) == 1 &&
        delivered.back() == std::vector<uint64_t>({0x55, 0x1800, 0}));
  // Kernel APCs: laid out as the reference, queued once on a thread that may queue APCs, removable.
  const uint32_t kapc = Objects().AllocateObject(40), kapc2 = Objects().AllocateObject(40);
  Threads().KeInitializeApc(gm, kapc, kthread, 0x82000200, 0, 0x82000000, 1, 0x66);
  Threads().KeInitializeApc(gm, kapc2, kthread, 0x82000200, 0, 0, 1, 5);
  CHECK(gm.Load16(kapc) == 18 && gm.Load8(kapc + 2) == 1 && gm.Load32(kapc + 4) == kthread && gm.Load32(kapc + 28) == 0x66);
  CHECK(gm.Load8(kapc2 + 2) == 0 && gm.Load32(kapc2 + 28) == 0);  // no normal routine: kernel mode, no context
  CHECK(Threads().KeInsertQueueApc(gm, kapc, 7, 8) == 0);  // this thread may not queue APCs yet
  gm.Store8(kthread + 0x8B, 1);
  CHECK(Threads().KeInsertQueueApc(gm, kapc, 7, 8) == 1 && gm.Load8(kapc + 3) == 1 && gm.Load32(kapc + 32) == 7);
  CHECK(Threads().KeInsertQueueApc(gm, kapc, 9, 9) == 0 && gm.Load32(kapc + 32) == 7);
  CHECK(Threads().KeRemoveQueueApc(gm, kapc) == 1 && gm.Load8(kapc + 3) == 0 && Threads().KeRemoveQueueApc(gm, kapc) == 0);
  // Kernel-mode APCs run at any wait, user-mode ones at alertable waits; the kernel routine may change the call.
  std::vector<uint32_t> kernel_calls;
  g_code_args[0x82000200] = [&](uint32_t, uint32_t, std::vector<uint64_t> args) -> uint64_t {
    kernel_calls.push_back(uint32_t(args[0]));
    if (args[0] == kapc) gm.Store32(uint32_t(args[2]), 0x99);  // the normal context
    return 0;
  };
  CHECK(Threads().KeInsertQueueApc(gm, kapc, 7, 8) == 1 && Threads().KeInsertQueueApc(gm, kapc2, 1, 2) == 1);
  delivered.clear();
  CHECK(Threads().DeliverApcs(gm, pcr, 0x7F000, false) == 0 && kernel_calls == std::vector<uint32_t>({kapc2}) &&
        delivered.empty() && Threads().PendingApcs(kthread) == 1 && gm.Load8(kapc2 + 3) == 0);
  CHECK(Threads().DeliverApcs(gm, pcr, 0x7F000) == 1 && kernel_calls == std::vector<uint32_t>({kapc2, kapc}) &&
        delivered.size() == 1 && delivered[0] == std::vector<uint64_t>({0x99, 7, 8}) && gm.Load8(kapc + 3) == 0);
  g_code_args.clear();
  CHECK(Threads().LookupThreadById(gm, 0x7777, out) == 0xC0000225u);
  Threads().Shutdown();
  Files().Reset();
  XamState().Reset();
  Memory().Reset();
  fs::remove_all(content, ec);
}

int main() {
  SimpsonsTests();
  TduTests();
  GraphicsAudioTests();
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
