// Scenario tests of the object manager against the NT kernel's documented semantics (the reference runtime keeps
// this state in host objects and is not an oracle for it, see docs/SYS.md): sequences of calls checked on statuses,
// returned states and the guest fields. Handles are abstracted: a test keeps what it was given.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/objects.h"

namespace {

using namespace any360::sys;

std::vector<uint8_t> g_memory(0x20000);
const Guest g{g_memory.data()};
constexpr uint32_t kHeap = 0x10000, kScratch = 0x1000, kThread = 0x8000, kOtherThread = 0x8100;
int g_failures = 0;

#define CHECK(condition)                                                                   \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
      ++g_failures;                                                                        \
    }                                                                                      \
  } while (0)

void Fresh() {
  std::memset(g_memory.data(), 0, g_memory.size());
  Objects().SetKernelHeap(kHeap, 0x8000);
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
  CHECK(Objects().WaitForObject(g, ev, kThread, &zero) == 0 && g.Load32(ev + 4) == 0);
  CHECK(Objects().WaitForObject(g, ev, kThread, &zero) == kStatusTimeout);
  Objects().InitializeEvent(g, ev, kNotificationEvent, 1);
  CHECK(Objects().WaitForObject(g, ev, kThread, &zero) == 0 && g.Load32(ev + 4) == 1);
  CHECK(Objects().WaitForObject(g, ev, kThread, &zero) == 0);
  // A short relative timeout on an unsignalled event times out.
  Objects().ResetEvent(g, ev);
  const int64_t ten_ms = -100000;
  CHECK(Objects().WaitForObject(g, ev, kThread, &ten_ms) == kStatusTimeout);

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
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == kStatusTimeout);
  Objects().NtSetEvent(g, h, 0);
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == 0);
  // Duplicate, close: the object lives while a handle does.
  CHECK(Objects().Duplicate(g, h, out + 16, 0) == 0);
  const uint32_t h2 = g.Load32(out + 16);
  CHECK(h2 != h && Objects().Lookup(h2) == object);
  CHECK(Objects().Close(h) == 0 && Objects().Lookup(h) == 0 && Objects().Lookup(h2) == object);
  CHECK(Objects().Close(h) == kStatusInvalidHandle);
  CHECK(Objects().NtSetEvent(g, h, 0) == kStatusInvalidHandle);
  CHECK(Objects().Close(h2) == 0 && Objects().Lookup(h2) == 0);
  CHECK(Objects().WaitForHandle(g, h2, kThread, &zero) == kStatusInvalidHandle);
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
  for (int i = 0; i < 5; ++i) CHECK(Objects().WaitForObject(g, sem, kThread, &zero) == 0);
  CHECK(g.Load32(sem + 4) == 0 && Objects().WaitForObject(g, sem, kThread, &zero) == kStatusTimeout);

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
  CHECK(Objects().CreateMutant(g, out, 0, 1, kThread) == 0);
  const uint32_t h = g.Load32(out), m = Objects().Lookup(h);
  CHECK(g.Load8(m) == kMutant && g.Load32(m + 4) == 0 && g.Load32(m + kMutantOwner) == kThread);
  CHECK(Objects().NtReleaseMutant(g, h, kOtherThread) == kStatusMutantNotOwned);
  const int64_t zero = 0;
  CHECK(Objects().WaitForHandle(g, h, kOtherThread, &zero) == kStatusTimeout);
  // The owner may acquire again (recursion) and must release as many times.
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == 0 && int32_t(g.Load32(m + 4)) == -1);
  CHECK(Objects().NtReleaseMutant(g, h, kThread) == 0 && g.Load32(m + 4) == 0 && g.Load32(m + kMutantOwner) == kThread);
  CHECK(Objects().NtReleaseMutant(g, h, kThread) == 0 && g.Load32(m + 4) == 1 && g.Load32(m + kMutantOwner) == 0);
  CHECK(Objects().NtReleaseMutant(g, h, kThread) == kStatusMutantNotOwned);
  // Free: the other thread takes it.
  CHECK(Objects().WaitForHandle(g, h, kOtherThread, &zero) == 0 && g.Load32(m + kMutantOwner) == kOtherThread);
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == kStatusTimeout);
  // Created free.
  CHECK(Objects().CreateMutant(g, out + 4, 0, 0, kThread) == 0);
  const uint32_t m2 = Objects().Lookup(g.Load32(out + 4));
  CHECK(g.Load32(m2 + 4) == 1 && g.Load32(m2 + kMutantOwner) == 0);
}

void References() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
  const uint32_t h = g.Load32(out), object = Objects().Lookup(h);
  // A reference keeps the object alive after its handle closes.
  CHECK(Objects().ReferenceObjectByHandle(g, h, kThread, out + 4) == 0 && g.Load32(out + 4) == object);
  CHECK(Objects().Close(h) == 0 && Objects().Lookup(h) == 0);
  CHECK(Objects().SetEvent(g, object) == 0 && g.Load32(object + 4) == 1);
  CHECK(Objects().DereferenceObject(g, object) == 0);
  // The current-thread pseudo handle.
  CHECK(Objects().ReferenceObjectByHandle(g, kCurrentThreadHandle, kThread, out + 8) == 0 && g.Load32(out + 8) == kThread);
  CHECK(Objects().ReferenceObjectByHandle(g, kHandleBase + 0x1000, kThread, out + 8) == kStatusInvalidHandle);
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
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == kStatusTimeout);
  CHECK(Objects().WaitForHandle(g, h, kThread, &half_second) == 0);
  CHECK(g.Load32(timer + 4) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().WaitForHandle(g, h, kThread, &zero) == 0);
  // Re-arming reports the timer was not armed any more; cancelling an armed one reports it was.
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, out + 16) == 0 && g.Load32(out + 16) == 0 && g.Load32(timer + 4) == 0);
  CHECK(Objects().NtSetTimerEx(g, h, out + 8, 0, out + 16) == 0 && g.Load32(out + 16) == 1);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 1 && g.Load8(timer + 3) == 0);
  CHECK(Objects().NtCancelTimer(g, h, out + 20) == 0 && g.Load32(out + 20) == 0);
  CHECK(Objects().WaitForHandle(g, h, kThread, &ten_ms) == kStatusTimeout);
  // A synchronization timer is consumed by the wait; a periodic one re-arms.
  CHECK(Objects().CreateTimer(g, out + 24, 0, 1) == 0);
  const uint32_t hs = g.Load32(out + 24), sync = Objects().Lookup(hs);
  CHECK(g.Load8(sync) == kSynchronizationTimer);
  CHECK(Objects().NtSetTimerEx(g, hs, out + 8, 5, 0) == 0);
  CHECK(Objects().WaitForHandle(g, hs, kThread, &half_second) == 0 && g.Load32(sync + 4) == 0 && g.Load8(sync + 3) == 1);
  CHECK(Objects().WaitForHandle(g, hs, kThread, &half_second) == 0);
  CHECK(Objects().NtCancelTimer(g, hs, 0) == 0);
  // An absolute due time in the past is due at once; KeInitializeTimerEx on a guest structure.
  const uint32_t kt = kScratch;
  Objects().InitializeTimer(g, kt, 0, 2);
  CHECK(g.Load8(kt) == kNotificationTimer && g.Load8(kt + 2) == 2 && g.Load64(kt + 16) == 0);
  CHECK(Objects().SetTimer(g, kt, int64_t(SystemTime() - 10), 0) == 0);
  CHECK(Objects().WaitForObject(g, kt, kThread, &zero) == 0 && g.Load32(kt + 4) == 1);
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
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAny, kThread, &zero) == 1);
  CHECK(g.Load32(s2 + 4) == 1);
  Objects().SetEvent(g, e0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAny, kThread, &zero) == 0 && g.Load32(e0 + 4) == 0);
  // Wait-all needs every object and consumes them together; nothing is taken when one is missing.
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kThread, &zero) == kStatusTimeout);
  CHECK(g.Load32(s2 + 4) == 1 && g.Load32(e1 + 4) == 1);
  Objects().SetEvent(g, e0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kThread, &zero) == 0);
  CHECK(g.Load32(e0 + 4) == 0 && g.Load32(e1 + 4) == 1 && g.Load32(s2 + 4) == 0);
  CHECK(Objects().WaitForHandles(g, 3, handles, kWaitAll, kThread, &ten_ms) == kStatusTimeout);
  // An unknown handle in the list is a parameter error; Ke* on guest objects goes through the same path.
  g.Store32(handles + 12, kHandleBase + 0x900);
  CHECK(Objects().WaitForHandles(g, 4, handles, kWaitAny, kThread, &zero) == kStatusInvalidParameter);
  const uint32_t objects[] = {e0, e1};
  CHECK(Objects().WaitForObjects(g, 2, objects, kWaitAny, kThread, &zero) == 1);
  (void)out;
}

}  // namespace

int main() {
  Timers();
  MultipleWaits();
  Events();
  Semaphores();
  Mutants();
  References();
  std::printf("sys_object_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
