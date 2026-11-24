// Scenario tests of the object manager against the NT kernel's documented semantics (the reference runtime keeps
// this state in host objects and is not an oracle for it, see docs/SYS.md): sequences of calls checked on statuses,
// returned states and the guest fields. Handles are abstracted: a test keeps what it was given.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/activity.h"
#include "any360/sys/alloc_meter.h"
#include "any360/sys/audio.h"
#include "any360/sys/files.h"
#include "any360/sys/graphics.h"
#include "any360/sys/memory.h"
#include <filesystem>
#include <fstream>
#include "any360/sys/info.h"
#include "any360/sys/input.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/spin_pacer.h"
#include "any360/sys/threading.h"
#include "any360/sys/video.h"
#include "any360/sys/config.h"
#include "any360/sys/crypt.h"
#include "any360/sys/rtl.h"
#include "any360/sys/variables.h"
#include "any360/sys/xam.h"
#include "any360/sys/win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <thread>

// watched_memory.cpp: memory whose watched pages are read-only until code writes to them, as a renderer keeps the
// pages it drew from.
namespace any360::sys::test {
uint8_t* WatchedMemoryCreate(size_t size);
void WatchedMemoryWatch(size_t offset);
int WatchedMemoryFaults();
void WatchedMemoryDestroy();
}  // namespace any360::sys::test

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

// Nt-created objects return to the kernel heap once no handle or Ob reference keeps them.
void Lifetimes() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  auto event = [&]() {
    CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
    return g.Load32(out);
  };
  const uint32_t e1 = event(), e2 = event(), e3 = event();
  const uint32_t a1 = Objects().Lookup(e1), a2 = Objects().Lookup(e2), a3 = Objects().Lookup(e3);
  CHECK(a1 == kHeap && a2 > a1 && a3 > a2);
  // A closed object's memory is reused; a duplicate handle or an Ob reference keeps it.
  CHECK(Objects().Close(e2) == 0 && Objects().Lookup(event()) == a2);
  uint32_t duplicate = 0;
  CHECK(Objects().Duplicate(g, e1, out + 4, 0) == 0);
  duplicate = g.Load32(out + 4);
  CHECK(Objects().ReferenceObjectByHandle(g, e3, kMainThread, out + 8) == 0 && g.Load32(out + 8) == a3);
  CHECK(Objects().Close(e1) == 0 && Objects().Close(e3) == 0);
  const uint32_t e4 = event();
  CHECK(Objects().Lookup(e4) != a1 && Objects().Lookup(e4) != a3);
  CHECK(Objects().Close(duplicate) == 0 && Objects().Lookup(event()) == a1);
  CHECK(Objects().DereferenceObject(g, a3) == 0 && Objects().Lookup(event()) == a3);
  // A wait by handle keeps its object while another thread closes the handle.
  const uint32_t waited = event(), waited_at = Objects().Lookup(waited);
  std::thread closer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(Objects().Close(waited) == 0);
    CHECK(Objects().AllocateObject(16, true) != waited_at);
    g.Store32(waited_at + 4, 1);
  });
  const int64_t second = -10000000;
  CHECK(Objects().WaitForHandle(g, waited, kMainThread, &second) == 0);
  closer.join();
  // An armed timer is cancelled with it; its memory serves again.
  CHECK(Objects().CreateTimer(g, out, 0, 0) == 0);
  const uint32_t timer = g.Load32(out), timer_at = Objects().Lookup(timer);
  g.Store64(out + 8, uint64_t(int64_t(-10000)));  // 1 ms
  CHECK(Objects().NtSetTimerEx(g, timer, out + 8, 0, 0, 0, 0, 0) == 0 && Objects().Close(timer) == 0);
  CHECK(Objects().AllocateObject(kTimerSize, true) == timer_at);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  Objects().Poll(g);
  CHECK(g.Load32(timer_at + 4) == 0);
  // Objects of the other managers stay unless created freed-on-close.
  const uint32_t kept = Objects().AllocateObject(16);
  const uint32_t kept_handle = Objects().OpenHandle(kept);
  CHECK(Objects().Close(kept_handle) == 0 && Objects().AllocateObject(16) != kept);
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
  // Absolute timeouts: a system time already past does not wait, a later one waits until then.
  CHECK(Objects().CreateEvent(g, out + 28, 0, 0, 0) == 0);
  const uint32_t unsignalled = g.Load32(out + 28);
  const int64_t past = int64_t(SystemTime() - 10);
  CHECK(Objects().WaitForHandle(g, unsignalled, kMainThread, &past) == kStatusTimeout);
  const uint32_t several[] = {Objects().Lookup(unsignalled)};
  CHECK(Objects().WaitForObjects(g, 1, several, kWaitAny, kMainThread, &past) == kStatusTimeout);
  const auto started = std::chrono::steady_clock::now();
  const int64_t soon = int64_t(SystemTime() + 200000);  // 20 ms
  CHECK(Objects().WaitForHandle(g, unsignalled, kMainThread, &soon) == kStatusTimeout);
  CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(15));
  const int64_t later = int64_t(SystemTime() + 50000000);  // 5 s, ended by the event
  std::thread setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g.Store32(Objects().Lookup(unsignalled) + 4, 1);
  });
  CHECK(Objects().WaitForObjects(g, 1, several, kWaitAny, kMainThread, &later) == 0);
  setter.join();
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
  // The clock advances and sits after 2020.
  const uint64_t t0 = SystemTime();
  CHECK(t0 > 132223104000000000ull && SystemTime() >= t0);
}

// A sleeping wait is woken by what ends it and does not wake a thousand times a second to look: with looks up to
// 200 ms apart, a signal, a timer, an APC and a time-out each end their wait on time; a state written straight into
// guest memory is still found, within the longest look.
void WaitsThatSleep() {
  Fresh();
  Objects().SetWaitLook(std::chrono::milliseconds(200));
  const uint32_t out = kScratch + 0x200;
  const int64_t five_seconds = -50000000;
  const auto elapsed_ms = [](std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
  };
  CHECK(Objects().CreateEvent(g, out, 0, 1, 0) == 0);
  const uint32_t event = g.Load32(out), event_at = Objects().Lookup(event);
  // A signal from another thread, long after the looks have spread out.
  ObjectManager::WaitCounts before = Objects().wait_counts();
  auto started = std::chrono::steady_clock::now();
  std::thread setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Objects().NtSetEvent(g, event, 0);
  });
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &five_seconds) == 0);
  setter.join();
  CHECK(elapsed_ms(started) >= 110 && elapsed_ms(started) < 180);
  ObjectManager::WaitCounts after = Objects().wait_counts();
  CHECK(after.on_signal == before.on_signal + 1 && after.by_looking == before.by_looking && after.slept == before.slept + 1);
  CHECK(after.looks - before.looks <= 9);  // 1, 2, 4 ... 64 ms and the signal: not 120
  // A timer that comes due while the wait sleeps.
  CHECK(Objects().CreateTimer(g, out + 4, 0, 0) == 0);
  const uint32_t timer = g.Load32(out + 4);
  g.Store64(out + 8, uint64_t(int64_t(-1500000)));  // 150 ms
  CHECK(Objects().NtSetTimerEx(g, timer, out + 8, 0, 0, 0, 0, 0) == 0);
  started = std::chrono::steady_clock::now();
  CHECK(Objects().WaitForHandle(g, timer, kMainThread, &five_seconds) == 0);
  CHECK(elapsed_ms(started) >= 140 && elapsed_ms(started) < 200);
  // A wait on something else wakes for the timer too and makes it fire: it is signalled by its due time.
  g.Store64(out + 8, uint64_t(int64_t(-1500000)));
  CHECK(Objects().NtSetTimerEx(g, timer, out + 8, 0, 0, 0, 0, 0) == 0);
  const int64_t quarter_second = -2500000;
  started = std::chrono::steady_clock::now();
  std::thread watcher([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    CHECK(g.Load32(Objects().Lookup(timer) + 4) == 1);
  });
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &quarter_second) == kStatusTimeout);
  watcher.join();
  // A time-out ends on time, not at the next look.
  CHECK(elapsed_ms(started) >= 240 && elapsed_ms(started) < 300);
  // An APC queued to the thread ends its alertable wait at once.
  started = std::chrono::steady_clock::now();
  std::thread queuer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Threads().QueueApcTo(kMainThread, 0x82000000, 1, 2, 3);
  });
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &five_seconds, true) == kStatusUserApc);
  queuer.join();
  CHECK(elapsed_ms(started) >= 110 && elapsed_ms(started) < 180);
  // A state written straight into guest memory, with nothing told to the layer: found by looking.
  before = Objects().wait_counts();
  started = std::chrono::steady_clock::now();
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g.Store32(event_at + 4, 1);
  });
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &five_seconds) == 0);
  writer.join();
  CHECK(elapsed_ms(started) >= 20 && elapsed_ms(started) < 20 + 200 + 60);
  after = Objects().wait_counts();
  CHECK(after.by_looking == before.by_looking + 1 && after.looked_types[kSynchronizationEvent] == before.looked_types[kSynchronizationEvent] + 1);
  // With the first look as late as the longest: a wait a signal ends wakes once, for the signal.
  Objects().SetWaitLook(std::chrono::milliseconds(200), std::chrono::milliseconds(200));
  before = Objects().wait_counts();
  started = std::chrono::steady_clock::now();
  std::thread second_setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Objects().NtSetEvent(g, event, 0);
  });
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &five_seconds) == 0);
  second_setter.join();
  after = Objects().wait_counts();
  CHECK(elapsed_ms(started) >= 110 && elapsed_ms(started) < 180 && after.looks - before.looks == 1);
  // A timer armed by another thread while a wait already sleeps on it: the wait ends at the timer's due time.
  started = std::chrono::steady_clock::now();
  std::thread armer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g.Store64(out + 16, uint64_t(int64_t(-500000)));  // 50 ms from then
    Objects().NtSetTimerEx(g, timer, out + 16, 0, 0, 0, 0, 0);
  });
  CHECK(Objects().NtCancelTimer(g, timer, 0) == 0);
  g.Store32(Objects().Lookup(timer) + 4, 0);
  CHECK(Objects().WaitForHandle(g, timer, kMainThread, &five_seconds) == 0);
  armer.join();
  CHECK(elapsed_ms(started) >= 95 && elapsed_ms(started) < 160);
  // With a look every millisecond, as before: the same waits, a wake-up at every look (as many as the host's timer
  // gives in a tenth of a second: 6 at its default 15.6 ms, 100 where a title has asked for a millisecond).
  Objects().SetWaitLook(std::chrono::milliseconds(1));
  before = Objects().wait_counts();
  const int64_t tenth = -1000000;
  CHECK(Objects().WaitForHandle(g, event, kMainThread, &tenth) == kStatusTimeout);
  after = Objects().wait_counts();
  CHECK(after.looks - before.looks >= 5 && after.timed_out == before.timed_out + 1);
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
  CHECK(gm.Load32(kthread + 0x84) == Threads().process() && gm.Load8(kthread + 0x73) == 1);
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
  // A thread spinning on zero delays (Sleep(0): the delay's fast path, no lock while its suspend count is 0) stops at
  // its next delay once suspended, and goes on once resumed.
  {
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> spins{0};
    g_code[0x1A00] = [&](uint32_t pcr, uint64_t, uint64_t) {
      while (!stop) {
        Threads().Delay(gm, pcr, 0);
        ++spins;
      }
      return 0;
    };
    CHECK(Threads().CreateThread(gm, out, 0, 0, 0, 0x1A00, 0, 0) == 0);
    const uint32_t spinner = gm.Load32(out);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (spins < 100 && std::chrono::steady_clock::now() < give_up) std::this_thread::yield();
    CHECK(Threads().NtSuspendThread(gm, spinner, out + 8) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // its delay under way when suspended returns
    const uint32_t held = spins;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const uint32_t still = spins;
    CHECK(Threads().NtResumeThread(gm, spinner, out + 8) == 0);
    while (spins < still + 100 && std::chrono::steady_clock::now() < give_up) std::this_thread::yield();
    const uint32_t resumed = spins;
    stop = true;
    CHECK(Objects().WaitForHandle(gm, spinner, kMainThread, &second) == 0 && Objects().Close(spinner) == 0);
    std::printf("sys_object_tests: zero delays: %u spins while suspended (30 ms), %u after the resume\n", still - held,
                resumed - still);
    CHECK(held >= 100 && still - held <= 1 && resumed >= still + 100);
  }
  // Thread ids from a first id the host sets, apart from its runtime's own threads (numbered from 1): two threads
  // with one id took each other for themselves (Mirror's Edge's render thread had its main thread's id).
  {
    Threads().SetFirstThreadId(0x1000);
    g_code[0x1900] = [&](uint32_t, uint64_t, uint64_t) { return 0; };
    CHECK(Threads().CreateThread(gm, out, 0, out + 4, 0, 0x1900, 0, 0) == 0);
    const uint32_t first = gm.Load32(out + 4), h1 = gm.Load32(out);
    CHECK(Threads().CreateThread(gm, out, 0, out + 4, 0, 0x1900, 0, 0) == 0);
    const uint32_t second_id = gm.Load32(out + 4), h2 = gm.Load32(out);
    CHECK(first == 0x1000 && second_id == 0x1001 && gm.Load32(Objects().Lookup(h1) + 0x14C) == 0x1000);
    CHECK(Objects().WaitForHandle(gm, h1, kMainThread, &second) == 0 && Objects().WaitForHandle(gm, h2, kMainThread, &second) == 0);
    CHECK(Objects().Close(h1) == 0 && Objects().Close(h2) == 0);
    Threads().SetFirstThreadId(1);  // as the other tests expect
    std::printf("sys_object_tests: thread ids from the host's first id: %X, %X\n", first, second_id);
  }
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
  // A thread created with no processor mask gets the next hardware thread in turn (its KTHREAD and PCR say which).
  const uint8_t first_cpu = gm.Load8(kx + 0xBF);
  CHECK(first_cpu < 6);
  {  // a processor mask in the creation flags' top byte: its highest bit, in the KTHREAD and the PCR
    GuestContext masked{};
    CHECK(Threads().CreateContext(gm, 0x4000, masked, 0x0C000000) == 0);
    CHECK(gm.Load8(masked.kthread + 0xBF) == 3 && gm.Load8(masked.pcr + 0x10C) == 3);
    // A title thread is of the title (user) process: type 1, in both of the thread's fields.
    CHECK(gm.Load8(masked.kthread + 0x73) == 1 && gm.Load8(masked.kthread + 0x72) == 1);
  }
  CHECK(Threads().SetAffinity(gm, kx, 0x4, out + 12) == 0 && gm.Load32(out + 12) == (1u << first_cpu) && gm.Load8(kx + 0xBF) == 2);
  CHECK(Threads().SetAffinity(gm, kx, 0x2, out + 12) == 0 && gm.Load32(out + 12) == 4);
  CHECK(Threads().QueryBasePriority(gm, kx) == 8);
  CHECK(Threads().SetBasePriority(gm, kx, 12) == 8 && Threads().QueryBasePriority(gm, kx) == 12);
  CHECK(Threads().SetBasePriority(gm, kx, uint32_t(-5)) == 12 && Threads().QueryBasePriority(gm, kx) == 0);
  // KeSetPriorityThread: the previous priority back, the new one set, a negative one as 0.
  CHECK(Threads().SetPriority(gm, kx, 17) == 0 && gm.Load8(kx + 0x70) == 17);
  CHECK(Threads().SetPriority(gm, kx, uint32_t(-3)) == 17 && gm.Load8(kx + 0x70) == 0);
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

// Thread stacks: a thread created with no size gets the default the host set (the title's); every stack is filled
// and has a red zone below it, so the stack watch tells how deep a thread went and logs a thread that used more than
// 3/4 of its stack, then one that ran past its end (which writes over the kernel heap's other blocks), once each.
void StackTests() {
  Fresh();
  Objects().SetKernelHeap(0x10000, 0x70000);
  Objects().SetGuest(Guest{g_memory.data()});
  Objects().Reset();
  const Guest gm{g_memory.data()};
  Threads().SetDispatcher(&Dispatch, nullptr);
  const uint32_t out = kScratch + 0x200;
  const int64_t second = -10000000;
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  Threads().SetStackLog(
      [&](const std::string& line) {
        std::lock_guard lock(lines_mutex);
        lines.push_back(line);
      },
      std::chrono::milliseconds(0), true);
  auto count = [&](const char* text) {
    std::lock_guard lock(lines_mutex);
    return std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
  };
  // The host's default (the title's XEX), 0 keeping it; a size asked wins.
  Threads().SetDefaultStackSize(0x8000);
  Threads().SetDefaultStackSize(0);
  CHECK(Threads().default_stack_size() == 0x8000);
  uint32_t limit = 0, base = 0;
  std::atomic<int> step{0};
  g_code_args[0x5000] = [&](uint32_t pcr, uint32_t stack, std::vector<uint64_t>) {
    const uint32_t kt = gm.Load32(pcr + 0x100);
    limit = gm.Load32(kt + 0x60);
    base = gm.Load32(kt + 0x5C);
    CHECK(base - limit == 0x8000 && stack < base && stack > base - 0x100);
    // Untouched stack and red zone hold the fill.
    CHECK(gm.Load32(limit) == 0xBEBEBEBE && gm.Load32(limit - Scheduler::kStackRedZone) == 0xBEBEBEBE);
    CHECK(Threads().StackDepth(gm, kt) < 0x100);
    // Half the stack: nothing logged.
    gm.Store32(base - 0x4000, 1);
    Threads().ReportStacks(gm);
    CHECK(count("used") == 0 && Threads().StackDepth(gm, kt) == 0x4000);
    // Past 3/4: logged once.
    gm.Store32(base - 0x7000, 1);
    Threads().ReportStacks(gm);
    Threads().ReportStacks(gm);
    CHECK(count("used 28 of its 32 KB stack") == 1);
    // Past its end into the red zone: logged once, with how far.
    gm.Store32(limit - 0x10, 1);
    Threads().ReportStacks(gm);
    Threads().ReportStacks(gm);
    CHECK(count("ran 16 bytes past the end of its 32 KB stack") == 1);
    CHECK(Threads().StackDepth(gm, kt) == 0x8010);
    step = 1;
    return 0;
  };
  CHECK(Threads().CreateThread(gm, out, 0, 0, 0, 0x5000, 0, 0) == 0);
  CHECK(Objects().WaitForHandle(gm, gm.Load32(out), kMainThread, &second) == 0 && step == 1);
  CHECK(count("starts at 00005000") == 1 && count("32 KB (the default)") == 1);
  // A size asked is kept; the red zone sits inside the stack's own block, below its limit, so two threads' stacks and
  // red zones never overlap.
  g_code_args[0x5100] = [&](uint32_t pcr, uint32_t, std::vector<uint64_t>) {
    const uint32_t kt = gm.Load32(pcr + 0x100);
    CHECK(gm.Load32(kt + 0x5C) - gm.Load32(kt + 0x60) == 0x6000);
    return 0;
  };
  CHECK(Threads().CreateThread(gm, out, 0x6000, 0, 0, 0x5100, 0, 0) == 0);
  CHECK(Objects().WaitForHandle(gm, gm.Load32(out), kMainThread, &second) == 0);
  CHECK(count("24 KB (asked)") == 1);
  // A thread that only ever calls zero delays (Sleep(0)) is still watched: the monitor looks at its first and every
  // 64th zero delay.
  g_code_args[0x5200] = [&](uint32_t pcr, uint32_t, std::vector<uint64_t>) {
    const uint32_t kt = gm.Load32(pcr + 0x100);
    gm.Store32(gm.Load32(kt + 0x5C) - 0x5800, 1);  // past 3/4 of its 24 KB
    for (int i = 0; i < 64; ++i) Threads().Delay(gm, pcr, 0);
    return 0;
  };
  CHECK(Threads().CreateThread(gm, out, 0x6000, 0, 0, 0x5200, 0, 0) == 0);
  CHECK(Objects().WaitForHandle(gm, gm.Load32(out), kMainThread, &second) == 0);
  CHECK(count("used 22 of its 24 KB stack") == 1);
  {
    GuestContext a{}, b{};
    CHECK(Threads().CreateContext(gm, 0x4000, a) == 0 && Threads().CreateContext(gm, 0x4000, b) == 0);
    const uint32_t a_limit = gm.Load32(a.kthread + 0x60), a_base = gm.Load32(a.kthread + 0x5C);
    const uint32_t b_limit = gm.Load32(b.kthread + 0x60), b_base = gm.Load32(b.kthread + 0x5C);
    CHECK(a_base <= b.kthread && b_limit - Scheduler::kStackRedZone > b.kthread && a_base - a_limit == 0x4000 &&
          b_base - b_limit == 0x4000);
  }
  Threads().SetStackLog(nullptr, std::chrono::milliseconds(0), false);
  Threads().SetDefaultStackSize(0x10000);
  Threads().Shutdown();
}

// The memory manager on a small layout the test's buffer can hold: a 4 KB virtual heap, a 64 KB one, a 4 KB physical
// view and a 64 KB physical view sharing one physical space.
// Ended threads go back to the kernel heap with their PCR, TLS block and stack once no handle keeps them; XAM's objects
// forget their state and are freed with their last handle.
// Read/write locks across threads: readers wait for a writer and come in together, a writer waits for the readers.
// SpinPacer on a simulated clock (no real sleep): a title's frame limiter loops `while (now < deadline) Sleep(0)`
// and does its frame's work between waits; each zero delay yields (1 us) or sleeps what the pacer says plus the host's
// overshoot (0-300 us, now and then more). What matters: the limiter never leaves its wait later than it would with
// yields alone (one yield past its deadline), and most of the wait is slept instead of spun.
void SpinPacerTests() {
  using namespace std::chrono;
  using Pacer = SpinPacer;
  struct Run {
    uint32_t frames = 0, late = 0;      // frames whose wait ended more than one yield past the deadline
    nanoseconds worst_late{0};
    double waited_ns = 0, slept_ns = 0;  // over the frames counted
  };
  const nanoseconds yield_cost = microseconds(1);
  uint32_t seed = 1;
  auto random = [&](uint32_t below) { return seed = seed * 1664525u + 1013904223u, (seed >> 8) % below; };
  // One limiter: `frames` frames of `work` then a wait to `period` after the last deadline; `overshoot` the host's
  // extra time a sleep takes; frames from `count_from` counted.
  auto limiter = [&](Pacer& pacer, Pacer::Clock::time_point& t, Pacer::Clock::time_point& deadline, uint32_t frames,
                     nanoseconds period, const std::function<nanoseconds(uint32_t)>& work,
                     const std::function<nanoseconds()>& overshoot, uint32_t count_from, Run& run) {
    for (uint32_t f = 0; f < frames; ++f) {
      t += work(f);
      deadline += period;
      const auto wait_start = t;
      double slept = 0;
      while (t < deadline) {
        const nanoseconds d = pacer.Delay(t);
        const auto before = t;
        t += d > nanoseconds::zero() ? d + overshoot() : yield_cost;
        if (d > nanoseconds::zero()) slept += double((t - before).count());
        pacer.Returned(t);
      }
      if (f < count_from) continue;
      ++run.frames;
      // Late by the pacer: past the deadline or, when the work itself ran past it, past the wait's start.
      const nanoseconds late = t - std::max(deadline, wait_start);
      if (late > yield_cost) ++run.late, run.worst_late = std::max(run.worst_late, late);
      run.waited_ns += double((t - wait_start).count());
      run.slept_ns += slept;
    }
  };
  const auto start = Pacer::Clock::time_point{} + seconds(10);
  // 62 FPS (16.13 ms), 11 ms +- 1 ms of work: the cadence is learnt within ~10 frames, then most of each ~5 ms wait
  // is slept, never past the deadline.
  {
    Pacer pacer;
    auto t = start, deadline = start;
    Run run;
    limiter(pacer, t, deadline, 300, microseconds(16129), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] { return microseconds(random(300)); }, 20, run);
    const double slept = run.slept_ns / run.waited_ns;
    std::printf("sys_object_tests: spin pacer at 62 FPS: %u of %u frames late (worst %.3f ms), %.0f%% of the wait "
                "slept, margin %.2f ms, %llu sleeps, %llu unsafe ends\n",
                run.late, run.frames, run.worst_late.count() / 1e6, 100 * slept, pacer.margin().count() / 1e6,
                (unsigned long long)pacer.stats().sleeps, (unsigned long long)pacer.stats().unsafe_ends);
    CHECK(run.late == 0 && slept > 0.75 && pacer.predicting());
  }
  // Now and then a sleep overshoots by 3 ms (a busy host): the frame it lands in may be late, the margin grows, and
  // the frames after are not late with the usual overshoot.
  {
    Pacer pacer;
    auto t = start, deadline = start;
    Run run;
    uint32_t spikes = 0;
    limiter(pacer, t, deadline, 400, microseconds(16129), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] {
              if (random(200) == 0) return ++spikes, microseconds(3000);
              return microseconds(random(300));
            },
            20, run);
    std::printf("sys_object_tests: spin pacer with 3 ms overshoots: %u spikes, %u of %u frames late (worst %.3f ms), "
                "%.0f%% slept, margin %.2f ms\n",
                spikes, run.late, run.frames, run.worst_late.count() / 1e6, 100 * run.slept_ns / run.waited_ns,
                pacer.margin().count() / 1e6);
    CHECK(spikes > 0 && run.late <= spikes && run.worst_late <= microseconds(3000) && pacer.stats().unsafe_ends <= spikes &&
          run.slept_ns / run.waited_ns > 0.5);
  }
  // A job system's yields: bursts of zero delays at random times (no cadence): never a sleep.
  {
    Pacer pacer;
    auto t = start;
    for (int burst = 0; burst < 2000; ++burst) {
      t += microseconds(300 + random(5000));
      for (uint32_t i = 0, n = 1 + random(50); i < n; ++i) {
        CHECK(pacer.Delay(t) == nanoseconds::zero());
        t += yield_cost;
        pacer.Returned(t);
      }
    }
    std::printf("sys_object_tests: spin pacer on a job system's yields: %llu sleeps over %llu zero delays\n",
                (unsigned long long)pacer.stats().sleeps, (unsigned long long)pacer.stats().calls);
    CHECK(pacer.stats().sleeps == 0);
  }
  // The title goes from 30 to 62 FPS (a menu to the game): the first wait of the new cadence was predicted a whole
  // 33 ms period after the last one; at most that frame is late, and the new cadence is learnt and slept again.
  {
    Pacer pacer;
    auto t = start, deadline = start;
    Run before, switched, after;
    limiter(pacer, t, deadline, 100, microseconds(33333), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] { return microseconds(random(300)); }, 20, before);
    limiter(pacer, t, deadline, 30, microseconds(16129), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] { return microseconds(random(300)); }, 0, switched);
    limiter(pacer, t, deadline, 100, microseconds(16129), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] { return microseconds(random(300)); }, 0, after);
    std::printf("sys_object_tests: spin pacer from 30 to 62 FPS: %u late before, %u of the 30 frames after the switch "
                "(worst %.2f ms), %u late later with %.0f%% slept\n",
                before.late, switched.late, switched.worst_late.count() / 1e6, after.late,
                100 * after.slept_ns / after.waited_ns);
    CHECK(before.late == 0 && switched.late <= 1 && after.late == 0 && after.slept_ns / after.waited_ns > 0.75);
  }
  // Heavy frames (work past the period: no wait at all) break the cadence: no sleep is predicted from it, and none
  // is late; the cadence comes back with the waits.
  {
    Pacer pacer;
    auto t = start, deadline = start;
    Run run;
    limiter(pacer, t, deadline, 300, microseconds(16129),
            [&](uint32_t f) { return microseconds((f / 20) % 2 ? 15000 + random(4000) : 10000 + random(2000)); },
            [&] { return microseconds(random(300)); }, 20, run);
    std::printf("sys_object_tests: spin pacer with heavy stretches: %u of %u frames late, %.0f%% slept\n", run.late,
                run.frames, 100 * run.slept_ns / std::max(1.0, run.waited_ns));
    CHECK(run.late == 0);
  }
  // Windows' high-resolution timer sleeps in 0.5 ms steps (sys_abi_bench: 50-200 us asked take ~513 us, 500 us
  // ~1004 us, 1 ms ~1517 us), now and then a step more.
  auto windows_sleep = [&](nanoseconds asked) {
    const int64_t step = 500000, steps = (asked.count() + 13000 + step - 1) / step;
    return nanoseconds(steps * step + 13000 + (random(100) == 0 ? step : 0)) - asked;  // the overshoot
  };
  // The frame limiter with the idle back-off on and Windows' sleeps: its cadence is still learnt (its waits are
  // roughly periodic from the start, so the back-off leaves them to yield), and no frame is late after.
  {
    Pacer pacer(true);
    auto t = start, deadline = start;
    Run run;
    Pacer::Duration asked{};
    limiter(pacer, t, deadline, 300, microseconds(16129), [&](uint32_t) { return microseconds(10000 + random(2000)); },
            [&] { return windows_sleep(pacer.margin()); }, 40, run);
    std::printf("sys_object_tests: spin pacer, back-off on, Windows sleeps: %u of %u frames late, %.0f%% of the wait "
                "slept, margin %.2f ms, %llu back-off slices\n",
                run.late, run.frames, 100 * run.slept_ns / run.waited_ns, pacer.margin().count() / 1e6,
                (unsigned long long)pacer.stats().idle_sleeps);
    (void)asked;
    CHECK(run.late == 0 && pacer.predicting() && run.slept_ns / run.waited_ns > 0.5 && !pacer.backing_off());
  }
  // A render thread waiting for each frame's commands: they come once a frame (16.13 ms), up to 1 ms early or late
  // (the game thread's work varies), after ~11 ms of waiting. Its ends stray more than kJitter: not predicted, and
  // roughly periodic: no back-off either. It yields, and sees every frame's work at once (sleeping such waits to a
  // margin plus their spread cost Mirror's Edge's 1% low 56 -> 34 FPS).
  {
    Pacer pacer(true);
    auto t = start;
    auto frame = start;
    double idle_ns = 0, slept_ns = 0;
    uint32_t late = 0, frames = 0;
    nanoseconds worst{0};
    for (int f = 0; f < 600; ++f) {
      frame += microseconds(16129);
      const auto posted = frame + microseconds(int(random(2000)) - 1000);
      const auto idle_start = t;
      double slept = 0;
      while (t < posted) {
        const nanoseconds d = pacer.Delay(t);
        const auto before = t;
        t += d > nanoseconds::zero() ? d + windows_sleep(d) : yield_cost;
        if (d > nanoseconds::zero()) slept += double((t - before).count());
        pacer.Returned(t);
      }
      if (f >= 40) {
        ++frames;
        idle_ns += double((t - idle_start).count());
        slept_ns += slept;
        if (t - posted > yield_cost) ++late, worst = std::max(worst, nanoseconds(t - posted));
      }
      t += microseconds(4000 + random(1000));  // the frame's rendering
    }
    std::printf("sys_object_tests: spin pacer on a render thread's waits (16.13 ms +- 1 ms): %.0f%% of the idle time "
                "slept, %u of %u frames' work seen late (worst %.3f ms)\n",
                100 * slept_ns / idle_ns, late, frames, worst.count() / 1e6);
    CHECK(late == 0 && !pacer.predicting() && !pacer.backing_off());
  }
  // A thread polling memory for work with no cadence (a render thread waiting for its commands): work is posted
  // after random gaps (most under 2 ms, some 2-8 ms, a few 20-150 ms of idleness). With the back-off it sleeps most
  // of its long waits in slices and sees the work at most a slice late (Windows' sleeps: ~1 ms); waits under 2 ms
  // still yield (seen at once). Without it, it only yields.
  for (const bool backoff : {false, true}) {
    Pacer pacer(backoff);
    auto t = start;
    double idle_ns = 0, slept_ns = 0;
    nanoseconds worst_short{0}, worst{0};
    uint32_t jobs = 0;
    for (int job = 0; job < 4000; ++job) {
      const uint32_t kind = random(100);
      const nanoseconds gap = kind < 70 ? microseconds(100 + random(1400))
                              : kind < 95 ? microseconds(2000 + random(6000))
                                          : milliseconds(20 + random(130));
      const auto posted = t + gap;
      const auto idle_start = t;
      while (t < posted) {
        const nanoseconds d = pacer.Delay(t);
        const auto before = t;
        t += d > nanoseconds::zero() ? d + windows_sleep(d) : yield_cost;
        if (d > nanoseconds::zero()) slept_ns += double((t - before).count());
        pacer.Returned(t);
      }
      const nanoseconds late = t - posted;
      if (job >= 100) {
        ++jobs;
        idle_ns += double((t - idle_start).count());
        worst = std::max(worst, late);
        if (gap < milliseconds(2)) worst_short = std::max(worst_short, late);
      }
      t += microseconds(200);  // the work
    }
    std::printf("sys_object_tests: idle back-off %s: %u jobs, %.0f%% of the idle time slept, work seen at worst %.3f "
                "ms late (%.3f ms after waits under 2 ms), %llu slices over %llu zero delays\n",
                backoff ? "on" : "off", jobs, 100 * slept_ns / idle_ns, worst.count() / 1e6, worst_short.count() / 1e6,
                (unsigned long long)pacer.stats().idle_sleeps, (unsigned long long)pacer.stats().calls);
    if (backoff) {
      CHECK(pacer.backing_off() && slept_ns / idle_ns > 0.7 && worst <= microseconds(1600) && worst_short <= yield_cost);
    } else {
      CHECK(pacer.stats().idle_sleeps == 0 && slept_ns == 0 && worst <= yield_cost);
    }
  }
}

void RwLockTests() {
  Fresh();
  const uint32_t lock = kScratch + 0x800, pcr1 = kScratch + 0x1000, pcr2 = kScratch + 0x1400;
  g.Store32(pcr1 + 0x100, kMainThread);
  g.Store32(pcr2 + 0x100, kMainThread + 0x40);
  std::memset(g.host(lock), 0xEE, 0x38);
  ExInitializeReadWriteLock(g, lock);
  CHECK(g.Load32(lock) == 0xFFFFFFFF && g.Load32(lock + 4) == 0 && g.Load32(lock + 8) == 0 && g.Load32(lock + 0xC) == 0);
  CHECK(g.Load8(lock + 0x10) == kSynchronizationEvent && g.Load32(lock + 0x14) == 0 && g.Load8(lock + 0x20) == kSemaphore &&
        g.Load32(lock + 0x24) == 0 && g.Load32(lock + 0x30) == 0x7FFFFFFF && g.Load32(lock + 0x34) == 0);
  auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); };
  // A writer in; a reader waits until the writer leaves.
  CHECK(ExTryToAcquireReadWriteLockExclusive(g, pcr1, lock) == 1 && ExTryToAcquireReadWriteLockExclusive(g, pcr1, lock) == 0);
  CHECK(ExTryToAcquireReadWriteLockShared(g, pcr1, lock) == 0);
  std::atomic<bool> reader_in{false};
  std::thread reader([&] {
    ExAcquireReadWriteLockShared(g, pcr2, lock);
    reader_in = true;
  });
  settle();
  CHECK(!reader_in && g.Load32(lock + 8) == 1);
  ExReleaseReadWriteLock(g, pcr1, lock);
  reader.join();
  CHECK(reader_in && g.Load32(lock + 8) == 0 && g.Load32(lock + 0xC) == 1 && g.Load32(lock) == 0);
  // Another reader joins at once; a writer then waits for both to leave.
  CHECK(ExTryToAcquireReadWriteLockShared(g, pcr1, lock) == 1 && g.Load32(lock + 0xC) == 2);
  std::atomic<bool> writer_in{false};
  std::thread writer([&] {
    ExAcquireReadWriteLockExclusive(g, pcr2, lock);
    writer_in = true;
  });
  settle();
  CHECK(!writer_in && g.Load32(lock + 4) == 1 && ExTryToAcquireReadWriteLockShared(g, pcr1, lock) == 0);  // a writer waits
  ExReleaseReadWriteLock(g, pcr1, lock);
  settle();
  CHECK(!writer_in);
  ExReleaseReadWriteLock(g, pcr1, lock);
  writer.join();
  CHECK(writer_in && g.Load32(lock + 4) == 0 && g.Load32(lock + 0xC) == 0);
  ExReleaseReadWriteLock(g, pcr2, lock);
  CHECK(g.Load32(lock) == 0xFFFFFFFF && g.Load32(lock + 0x34) == 0);
  // Interlocked lists: pushed and popped in LIFO order, flushed whole.
  const uint32_t head = kScratch + 0x900, e1 = kScratch + 0x910, e2 = kScratch + 0x920;
  g.Store64(head, 0);
  CHECK(InterlockedPushEntrySList(g, head, e1) == 0 && InterlockedPushEntrySList(g, head, e2) == e1);
  CHECK(g.Load32(head) == e2 && g.Load16(head + 4) == 2 && g.Load16(head + 6) == 2 && g.Load32(e2) == e1);
  CHECK(InterlockedPopEntrySList(g, head) == e2 && g.Load32(head) == e1 && g.Load16(head + 4) == 1 && g.Load16(head + 6) == 2);
  CHECK(InterlockedPushEntrySList(g, head, e2) == e1 && InterlockedFlushSList(g, head) == e2 && g.Load64(head) == 0);
  CHECK(InterlockedPopEntrySList(g, head) == 0);
}

void ReclaimTests() {
  Fresh();
  Objects().SetKernelHeap(0x10000, 0x70000);
  Objects().Reset();
  const Guest gm{g_memory.data()};
  Threads().SetDispatcher(&Dispatch, nullptr);
  const uint32_t out = kScratch + 0x200;
  // The heap's first free address, found by a block allocated and freed at once.
  auto first_free = [] {
    const uint32_t probe = Objects().AllocateObject(16, true);
    Objects().FreeObject(probe);
    return probe;
  };
  auto settles_at = [&](uint32_t expected) {
    for (int i = 0; i < 1000; ++i) {
      if (first_free() == expected) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  g_code[0x3000] = [](uint32_t, uint64_t, uint64_t) -> uint64_t { return 5; };
  // A thread waited on, then its handle closed.
  CHECK(Threads().CreateThread(gm, out, 0, out + 4, 0, 0x3000, 0, 0) == 0);
  // The process the first thread creates stays; its KTHREAD comes right after.
  const uint32_t handle = gm.Load32(out), base = Objects().Lookup(handle);
  CHECK(base != 0 && first_free() != base);
  CHECK(Objects().WaitForHandle(gm, handle, kMainThread, nullptr) == 0 && Objects().Close(handle) == 0);
  CHECK(settles_at(base) && Threads().LookupThreadById(gm, gm.Load32(out + 4), out + 8) == 0xC0000225u);
  // A thread with no handle frees itself when it ends.
  CHECK(Threads().CreateThread(gm, 0, 0, 0, 0, 0x3000, 0, 0) == 0);
  CHECK(settles_at(base));
  // A thread whose handle is the KTHREAD itself stays.
  CHECK(Threads().CreateThread(gm, out, 0, 0, 0, 0x3000, 0, kCreateHandleIsObject) == 0);
  const uint32_t kept = gm.Load32(out);
  CHECK(Objects().WaitForObject(gm, kept, kMainThread, nullptr) == 0 && kept == base);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(first_free() != base);
  Threads().Shutdown();
  // XAM: a closed listener's and enumerator's memory serves again, their state gone.
  Objects().Reset();
  XamState().Reset();
  const uint32_t listener = XamState().NotifyCreateListener(gm, 1, 0), listener_at = Objects().Lookup(listener);
  CHECK(Objects().Close(listener) == 0 && Objects().Lookup(XamState().NotifyCreateListener(gm, 1, 0)) == listener_at);
  const uint32_t enumerator = XamState().CreateEnumerator(gm, 4, 1, {{1, 2, 3, 4}}), enumerator_at = Objects().Lookup(enumerator);
  CHECK(Objects().Close(enumerator) == 0);
  const uint32_t again = XamState().CreateEnumerator(gm, 4, 1, {{5, 6, 7, 8}});
  CHECK(Objects().Lookup(again) == enumerator_at);
  CHECK(XamState().Enumerate(gm, again, 0, kScratch + 0x400, 4, out, 0) == 0 && gm.Load8(kScratch + 0x400) == 5);
  XamState().Reset();
  g_code.clear();
}

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
  // A commit over a range part of which is committed: only the newly committed pages read as zeros (a heap growing
  // its committed part over the pages it already uses keeps them).
  {
    gm.Store32(io, 0);
    gm.Store32(io + 4, 0x4000);
    CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemReserve, kPageReadWrite, 0) == 0);
    const uint32_t region = gm.Load32(io);
    gm.Store32(io, region + 0x2000);  // the third page committed and used
    gm.Store32(io + 4, 0x1000);
    CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit, kPageReadWrite, 0) == 0);
    gm.Store32(region + 0x2000, 0x12345678);
    std::memset(memory.data() + region + 0x1000, 0xAA, 0x1000);  // garbage in the second, still reserved page
    gm.Store32(io, region + 0x1000);  // then the second to the fourth: the first of them new
    gm.Store32(io + 4, 0x3000);
    CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemCommit, kPageReadWrite, 0) == 0);
    CHECK(gm.Load32(region + 0x2000) == 0x12345678 && gm.Load32(region + 0x1000) == 0 && gm.Load32(region + 0x3FFC) == 0);
    gm.Store32(io, region);
    gm.Store32(io + 4, 0);
    CHECK(Memory().FreeVirtual(gm, io, io + 4, kMemRelease, 0) == 0);
  }
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
  // A fragmented heap: every 4 KB page taken one by one (top down, around the 64 KB view's physical pages), then every
  // other one released: no two free pages in a row; one more released makes a run of three, and the search, skipping
  // whole windows past each taken page, finds the highest pair as a page-by-page search would.
  {
    std::vector<uint32_t> singles;
    for (uint32_t a; (a = Memory().AllocatePhysical(gm, 0, 0x1000, kPageReadWrite, 0, 0xFFFFFFFF, 0)) != 0;) singles.push_back(a);
    CHECK(singles.size() >= 8);
    for (size_t i = 0; i < singles.size(); i += 2) Memory().FreePhysical(0, singles[i]);
    CHECK(Memory().AllocatePhysical(gm, 0, 0x2000, kPageReadWrite, 0, 0xFFFFFFFF, 0) == 0);
    const bool contiguous = singles.size() >= 3 && singles[0] - singles[2] == 0x2000;
    CHECK(contiguous);
    Memory().FreePhysical(0, singles[1]);
    const uint32_t pair = Memory().AllocatePhysical(gm, 0, 0x2000, kPageReadWrite, 0, 0xFFFFFFFF, 0);
    CHECK(pair == singles[1]);
    // Aligned to 8 KB: the run [singles[2], singles[0]] holds one 8 KB-aligned pair, or none.
    Memory().FreePhysical(0, pair);
    const uint32_t aligned = Memory().AllocatePhysical(gm, 0, 0x2000, kPageReadWrite, 0, 0xFFFFFFFF, 0x2000);
    CHECK(aligned == 0 || (aligned % 0x2000 == 0 && aligned >= singles[2] && aligned + 0x2000 <= singles[0] + 0x1000));
    if (aligned) Memory().FreePhysical(0, aligned);
    for (size_t i = 3; i < singles.size(); i += 2) Memory().FreePhysical(0, singles[i]);
  }
  // Statistics and XamAlloc.
  gm.Store32(io + 0x40, 104);
  CHECK(Memory().QueryStatistics(gm, io + 0x40) == 0 && gm.Load32(io + 0x44) == 0x20000 && gm.Load32(io + 0x4C) < 0x20000);
  gm.Store32(io + 0x40, 100);
  CHECK(Memory().QueryStatistics(gm, io + 0x40) == 0xC0000023u);
  CHECK(Memory().XamAlloc(gm, 0, 100, io + 8) == 0);
  const uint32_t x = gm.Load32(io + 8);
  CHECK(x != 0 && x % 0x1000 == 0 && Memory().QueryAllocationSize(x) == 0x1000 && memory[x] == 0);
  CHECK(Memory().XamFree(gm, x) == 0 && Memory().QueryAllocationSize(x) == 0);
  // The search on random fragmentation, both directions, against what a page-by-page search would find:
  // - bottom up (AllocatePages, 4 KB pages, unbounded): no free run of the size below the address it returns (or
  //   anywhere when it fails), read back through QueryRegion;
  // - top down, physical, bounded and aligned (the 4 KB view, its physical pages shared with the 64 KB view): no aligned
  //   window above the address it returns, inside the bounds, that a one-window request would get.
  {
    Memory().Reset({{0x00000000, 0x100000, 0x1000, false}, {0x100000, 0x40000, 0x1000, true}, {0x140000, 0x80000, 0x10000, true}});
    gm.Store32(io, io & ~0xFFFu);
    gm.Store32(io + 4, 0x1000);
    CHECK(Memory().AllocateVirtual(gm, io, io + 4, kMemReserve, kPageReadWrite, 0) == 0);  // the in/out words' page
    uint32_t seed = 0x2026u;
    auto random = [&](uint32_t n) {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) % n;
    };
    // A free run of at least `bytes` in [from, to) of the 4 KB virtual heap.
    auto free_run = [&](uint32_t from, uint32_t to, uint32_t bytes) {
      RegionInfo info;
      for (uint32_t a = from; a < to && Memory().QueryRegion(a, info) && info.region_size;) {
        if (info.state == kMemFree && std::min(info.region_size, to - a) >= bytes) return true;
        a += info.region_size;
      }
      return false;
    };
    constexpr uint32_t kAlignments[] = {0, 0x2000, 0x4000};
    std::vector<uint32_t> pages, small, large;
    int bottom_up = 0, top_down = 0, missed = 0, out_of_bounds = 0, failed = 0;
    const uint64_t allocs_before = SysActivity().memory_allocs.load();
    for (int step = 0; step < 3000; ++step) {
      const uint32_t what = random(10);
      if (what < 3) {  // bottom up
        const uint32_t bytes = (1 + random(6)) * 0x1000;
        const uint32_t a = Memory().AllocatePages(gm, bytes);
        ++bottom_up;
        if (a) pages.push_back(a);
        else ++failed;
        if (free_run(0x1000, a ? a : 0x100000, bytes)) ++missed;
      } else if (what < 6) {  // top down, physical, bounded, aligned
        const uint32_t bytes = (1 + random(4)) * 0x1000, alignment = kAlignments[random(3)];
        const uint32_t min = random(64) * 0x1000, max = std::min<uint32_t>(0x3FFFF, min + random(64) * 0x1000 + 0xFFF);
        const uint32_t step_bytes = std::max<uint32_t>(alignment, 0x1000);
        const uint32_t a = Memory().AllocatePhysical(gm, 0, bytes, kPageReadWrite, min, max, alignment);
        ++top_down;
        const uint32_t physical = a ? a - 0x100000 : 0;
        if (a) {
          small.push_back(a);
          if (physical % step_bytes || physical < min || physical + bytes - 1 > max) ++out_of_bounds;
        } else {
          ++failed;
        }
        // Every aligned window above the result (or every one when it failed), alone.
        for (uint32_t c = a ? physical + step_bytes : (std::max<uint32_t>(min, 0x1000) + step_bytes - 1) / step_bytes * step_bytes;
             c + bytes - 1 <= max; c += step_bytes) {
          if (const uint32_t probe = Memory().AllocatePhysical(gm, 0, bytes, kPageReadWrite, c, c + bytes - 1, alignment)) {
            ++missed;
            Memory().FreePhysical(0, probe);
          }
        }
      } else if (what < 7) {  // 64 KB pages over the shared physical range
        const uint32_t min = random(4) * 0x10000;
        if (const uint32_t a = Memory().AllocatePhysical(gm, 0, 0x10000, kPageReadWrite | kMemLargePages, min, 0x3FFFF, 0)) large.push_back(a);
      } else {  // free something
        std::vector<uint32_t>& list = what < 8 ? pages : what < 9 ? small : large;
        if (!list.empty()) {
          const size_t i = random(uint32_t(list.size()));
          if (&list == &pages) Memory().FreePages(list[i]);
          else Memory().FreePhysical(0, list[i]);
          list[i] = list.back();
          list.pop_back();
        }
      }
    }
    CHECK(missed == 0 && out_of_bounds == 0);
    CHECK(failed > 0 && bottom_up > 500 && top_down > 500);  // the heaps did fill up
    CHECK(SysActivity().memory_allocs.load() > allocs_before + uint64_t(top_down));  // the activity counter counts calls
    std::printf("sys_object_tests: allocation search on random fragmentation: %d bottom up, %d top down (bounded, aligned), "
                "%d failed, %d missed windows, %d out of bounds\n",
                bottom_up, top_down, failed, missed, out_of_bounds);
  }
  Memory().Reset();
}

// The file system on a temporary directory: a read-only "game" device and a writable "hdd" one.
// The Win32 file calls XAM exports, on the same file system.
// The local profile's gamertag and parental controls.
void UserProfileTests() {
  Fresh();
  XamState().Reset();
  const uint32_t buf = kScratch + 0x400, out = kScratch + 0x200, ov = kScratch + 0x300;
  auto wide = [&](uint32_t at) {
    std::string text;
    for (; g.Load16(at); at += 2) text += char(g.Load16(at));
    return text;
  };
  XamState().title().gamertag = "A very long gamertag";
  CHECK(XamState().UserGetGamerTag(g, 0, buf, 32) == 0 && wide(buf) == "A very long gam");  // 15 characters and the end
  XamState().title().gamertag = "Player";
  CHECK(XamState().UserGetGamerTag(g, 0, buf, 16) == 0 && wide(buf) == "Player");
  CHECK(XamState().UserGetGamerTag(g, 0, buf, 8) == 0x80070057u && XamState().UserGetGamerTag(g, 4, buf, 16) == 0x80070057u);
  CHECK(XamState().UserGetGamerTag(g, 1, buf, 16) == 0x80070525u);
  CHECK(XamState().UserContentRestrictionGetFlags(g, 0, out) == 0 && g.Load32(out) == 0);
  CHECK(XamState().UserContentRestrictionGetRating(g, 0, out, out + 4) == 0 && g.Load32(out) == 0x3F);
  CHECK(XamState().UserContentRestrictionGetRating(g, 2, out, out + 4) == kErrorNoSuchUser);
  g.Store32(out, 0);
  CHECK(XamState().UserContentRestrictionCheckAccess(g, out, 0) == 0 && g.Load32(out) == 1);
  CHECK(XamState().UserContentRestrictionCheckAccess(g, out, ov) == kErrorIoPending && g.Load32(ov) == 0);
  CHECK(XamState().UserGetDeviceContext(g, 0x1FF, out) == 0 && XamState().UserGetDeviceContext(g, 2, out) == 0x8007048Fu);
  // The empty online schema: one pair (pointer, 44 bytes) whatever the number of calls.
  const uint32_t schema = XamState().GetOnlineSchema(g);
  CHECK(schema != 0 && XamState().GetOnlineSchema(g) == schema && g.Load32(schema) == schema + 8 && g.Load32(schema + 4) == 44 &&
        g.Load8(schema + 8 + 15) == 0x2C);
  XamState().Reset();
}

// The kernel's exported variables, created once in the kernel heap.
void VariableTests() {
  Fresh();
  XamState().Reset();
  Variables().Reset();
  const uint32_t info = Variables().Address(g, "XboxHardwareInfo");
  CHECK(info != 0 && Variables().Address(g, "XboxHardwareInfo") == info && g.Load32(info) == 0x20 && g.Load8(info + 4) == 6);
  const uint32_t version = Variables().Address(g, "XboxKrnlVersion");
  CHECK(version != 0 && g.Load16(version) == 2 && g.Load16(version + 2) == 0xFFFF && g.Load8(version + 6) == 0x80);
  const uint32_t region = Variables().Address(g, "ExConsoleGameRegion");
  CHECK(region != 0 && g.Load32(region) == 0xFFFFFFFF);
  const uint32_t event_type = Variables().Address(g, "ExEventObjectType"), thread_type = Variables().Address(g, "ExThreadObjectType");
  CHECK(event_type && thread_type && event_type != thread_type && g.Load32(event_type + 0x18) == 0x45766E74u);  // "Evnt"
  const uint32_t line = Variables().Address(g, "ExLoadedCommandLine"), image = Variables().Address(g, "ExLoadedImageName");
  CHECK(std::string(reinterpret_cast<const char*>(g.host(line))) == "\"default.xex\"");
  CHECK(std::string(reinterpret_cast<const char*>(g.host(image))) == "\\Device\\Cdrom0\\default.xex");
  CHECK(g.Load32(Variables().Address(g, "KeDebugMonitorData")) == 0 && Variables().Address(g, "NotAVariable") == 0);
  // The first retail kernel's version: 2.0.1888.0.
  const uint32_t base_version = Variables().Address(g, "XboxKrnlBaseVersion");
  CHECK(base_version && g.Load16(base_version) == 2 && g.Load16(base_version + 2) == 0 && g.Load16(base_version + 4) == 1888 &&
        g.Load16(base_version + 6) == 0);
  // The kernel's events: the USB enumeration done (notification, set), the other two (synchronization, not set); a
  // wait on the set one returns at once, on the others it times out.
  const uint32_t usb_done = Variables().Address(g, "UsbdBootEnumerationDoneEvent");
  const uint32_t usb_load = Variables().Address(g, "UsbdDriverLoadRequiredEvent"), stfs = Variables().Address(g, "StfsDeviceErrorEvent");
  CHECK(usb_done && usb_load && stfs && usb_done != usb_load && usb_load != stfs);
  CHECK(g.Load8(usb_done) == kNotificationEvent && g.Load32(usb_done + 4) == 1);
  CHECK(g.Load8(usb_load) == kSynchronizationEvent && g.Load32(usb_load + 4) == 0);
  CHECK(g.Load8(stfs) == kSynchronizationEvent && g.Load32(stfs + 4) == 0);
  const int64_t no_wait = 0;
  CHECK(Objects().WaitForObject(g, usb_done, kMainThread, &no_wait) == 0);
  CHECK(Objects().WaitForObject(g, stfs, kMainThread, &no_wait) == kStatusTimeout);
  // The video variables: a device pointer to a zeroed block, no XAM device, 500 MHz, an initialized critical section.
  const uint32_t device = Variables().Address(g, "VdGlobalDevice");
  CHECK(device && g.Load32(device) != 0 && g.Load32(g.Load32(device)) == 0);
  CHECK(g.Load32(Variables().Address(g, "VdGlobalXamDevice")) == 0 && g.Load32(Variables().Address(g, "VdGpuClockInMHz")) == 500);
  const uint32_t hsio = Variables().Address(g, "VdHSIOCalibrationLock");
  // RTL_CRITICAL_SECTION: a synchronization event's header, the spin count in 256s (10000 -> 40), free (lock count -1).
  CHECK(hsio && g.Load8(hsio) == 1 && g.Load8(hsio + 1) == 40 && g.Load32(hsio + 4) == 0);
  // The time stamp bundle keeps time on its own.
  const uint32_t bundle = Variables().Address(g, "KeTimeStampBundle");
  const uint32_t ticks = g.Load32(bundle + 16);
  const uint64_t system = g.Load64(bundle + 8);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));  // the clock sleeps 1 ms, about 16 on Windows
  CHECK(g.Load32(bundle + 16) >= ticks + 40 && g.Load64(bundle + 8) > system && g.Load64(bundle) > 0);
  Variables().Reset();
  XamState().Reset();
}

void Win32FileTests() {
  Fresh();
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "any360-sys-win32";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "game" / "Media");
  fs::create_directories(root / "hdd");
  { std::ofstream(root / "game" / "Media" / "Data.BIN", std::ios::binary) << "0123456789ABCDEF"; }
  { std::ofstream(root / "game" / "Media" / "b.txt", std::ios::binary) << "b"; }
  { std::ofstream(root / "game" / "Media" / "a.txt", std::ios::binary) << "aa"; }
  Files().Reset();
  Files().Mount("\\Device\\Cdrom0", root / "game", true);
  Files().Mount("\\Device\\Harddisk0\\Partition1", root / "hdd", false);
  Files().Link("game:", "\\Device\\Cdrom0");
  Files().Link("hdd:", "\\Device\\Harddisk0\\Partition1");
  const uint32_t text = kScratch + 0x100, text2 = kScratch + 0x180, out = kScratch + 0x200, buf = kScratch + 0x400,
                 ov = kScratch + 0x300, data = kScratch + 0x600;
  auto str = [&](uint32_t at, const char* p) {
    std::memcpy(g.host(at), p, std::strlen(p) + 1);
    return at;
  };
  const uint32_t kRead = 0x80000000, kWrite = 0x40000000, kOpenExisting = 3, kCreateAlways = 2, kCreateNew = 1;
  // A folder mounted over a folder of a mounted device wins through a link ("game:\Media" is "\Device\Cdrom0\Media",
  // one separator, not two), as a title's redirection of one folder needs.
  fs::create_directories(root / "over");
  { std::ofstream(root / "over" / "data.bin", std::ios::binary) << "OVERLAY"; }
  Files().Mount("\\Device\\Cdrom0\\Media", root / "over", true);
  {
    const uint32_t h = Files().Win32CreateFile(g, str(text, "game:\\Media\\DATA.bin"), 0x80000000, 3);
    CHECK(h != 0xFFFFFFFF && Files().Win32Transfer(g, h, buf, 7, out, 0, false) == 1 && std::memcmp(g.host(buf), "OVERLAY", 7) == 0);
    Objects().Close(h);
  }
  Files().Unmount("\\Device\\Cdrom0\\Media");
  // Open, read in two steps, seek, read at an OVERLAPPED's offset, read past the end (TRUE, no bytes).
  const uint32_t f =Files().Win32CreateFile(g, str(text, "game:\\MEDIA\\data.bin"), kRead, kOpenExisting);
  CHECK(f != 0xFFFFFFFF && Files().Win32GetFileSize(g, f, out) == 16 && g.Load32(out) == 0);
  CHECK(Files().Win32Transfer(g, f, buf, 4, out, 0, false) == 1 && g.Load32(out) == 4 && std::memcmp(g.host(buf), "0123", 4) == 0);
  CHECK(Files().Win32Transfer(g, f, buf, 2, out, 0, false) == 1 && std::memcmp(g.host(buf), "45", 2) == 0);
  CHECK(Files().Win32SetFilePointer(g, f, uint32_t(-3), 0, 2) == 13 && Files().Win32Transfer(g, f, buf, 8, out, 0, false) == 1 &&
        g.Load32(out) == 3 && std::memcmp(g.host(buf), "DEF", 3) == 0);
  CHECK(Files().Win32Transfer(g, f, buf, 8, out, 0, false) == 1 && g.Load32(out) == 0);
  g.Store32(ov + 8, 10);
  g.Store32(ov + 12, 0);
  CHECK(Files().Win32Transfer(g, f, buf, 2, 0, ov, false) == 1 && g.Load32(ov) == 0 && g.Load32(ov + 4) == 2 &&
        std::memcmp(g.host(buf), "AB", 2) == 0);
  CHECK(Files().Win32SetFilePointerEx(g, f, 0, 5, out, 0) == 1 && g.Load32(out) == 5 && g.Load32(out + 4) == 0);
  CHECK(Files().Win32GetFileSizeEx(g, f, out) == 1 && g.Load64(out) == 16);
  CHECK(Files().Win32Transfer(g, f, buf, 1, out, 0, true) == 0);  // a read-only device
  CHECK(Files().Win32CreateFile(g, str(text, "game:\\missing"), kRead, kOpenExisting) == 0xFFFFFFFF);
  // Attributes; the disk's space; a search sorted by name.
  CHECK(Files().Win32GetFileAttributes(g, str(text, "game:\\Media")) == kAttributeDirectory);
  CHECK(Files().Win32GetFileAttributes(g, str(text, "game:\\Media\\nope")) == 0xFFFFFFFF);
  CHECK(Files().Win32GetFileAttributesEx(g, str(text, "game:\\Media\\a.txt"), out) == 1 && g.Load32(out) == kAttributeNormal &&
        g.Load32(out + 32) == 2 && g.Load32(out + 20) != 0);
  CHECK(Files().Win32GetDiskFreeSpace(g, str(text, "hdd:\\"), out, out + 8, out + 16) == 1 && g.Load32(out + 8 + 4) + g.Load32(out + 8) != 0);
  const uint32_t find = Files().Win32FindFirstFile(g, str(text, "game:\\Media\\*.txt"), data);
  CHECK(find != 0xFFFFFFFF && std::string(reinterpret_cast<const char*>(g.host(data + 0x2C))) == "a.txt" && g.Load32(data + 32) == 2);
  CHECK(Files().Win32FindNextFile(g, find, data) == 1 && std::string(reinterpret_cast<const char*>(g.host(data + 0x2C))) == "b.txt");
  CHECK(Files().Win32FindNextFile(g, find, data) == 0 && Objects().Close(find) == 0);
  CHECK(Files().Win32FindFirstFile(g, str(text, "game:\\Media\\*.zip"), data) == 0xFFFFFFFF);
  CHECK(Objects().Close(f) == 0);
  // Write, truncate, copy, move and delete on the hard drive.
  CHECK(Files().Win32CreateDirectory(g, str(text, "hdd:\\save")) == 1 && Files().Win32CreateDirectory(g, text) == 0);
  CHECK(Files().Win32CreateDirectory(g, str(text, "game:\\new")) == 0);
  const uint32_t w = Files().Win32CreateFile(g, str(text, "hdd:\\save\\slot.dat"), kWrite, kCreateAlways);
  std::memcpy(g.host(buf), "hello world", 11);
  CHECK(w != 0xFFFFFFFF && Files().Win32Transfer(g, w, buf, 11, out, 0, true) == 1 && g.Load32(out) == 11);
  CHECK(Files().Win32SetFilePointer(g, w, 5, 0, 0) == 5 && Files().Win32SetEndOfFile(w) == 1 && fs::file_size(root / "hdd" / "save" / "slot.dat", ec) == 5);
  CHECK(Files().Win32FlushFileBuffers(w) == 1 && Objects().Close(w) == 0);
  CHECK(Files().Win32CreateFile(g, text, kWrite, kCreateNew) == 0xFFFFFFFF);  // exists
  CHECK(Files().Win32CopyFile(g, text, str(text2, "hdd:\\save\\copy.dat"), 1) == 1 && Files().Win32CopyFile(g, text, text2, 1) == 0);
  CHECK(Files().Win32CopyFile(g, text, text2, 0) == 1);
  CHECK(Files().Win32MoveFile(g, text2, str(kScratch + 0x1C0, "hdd:\\save\\moved.dat")) == 1 && fs::exists(root / "hdd" / "save" / "moved.dat"));
  CHECK(Files().Win32MoveFile(g, kScratch + 0x1C0, text) == 0);  // the target exists
  CHECK(Files().Win32DeleteFile(g, kScratch + 0x1C0) == 1 && Files().Win32DeleteFile(g, kScratch + 0x1C0) == 0);
  CHECK(Files().Win32DeleteFile(g, str(text2, "hdd:\\save")) == 0);  // a directory
  CHECK(Files().Win32DeleteFile(g, str(text2, "game:\\Media\\a.txt")) == 0);  // read-only
  Files().Reset();
  fs::remove_all(root, ec);
}

// XAM strings the reference cannot be compared on: dates and times of a FILETIME, locators with guest names.
// The video queries' xam forms and the real mode agree with the Vd* ones; the display hardware a host has no
// counterpart for answers success and writes nothing.
void VideoTests() {
  Fresh();
  Video() = {};
  for (const auto& [width, height] : {std::pair{1280u, 720u}, {640u, 480u}, {1920u, 1080u}}) {
    Video().width = width, Video().height = height;
    std::memset(g.host(kScratch), 0xCD, 0x100);
    VdQueryVideoMode(g, kScratch);
    VdQueryRealVideoMode(g, kScratch + 0x80);
    CHECK(std::memcmp(g.host(kScratch), g.host(kScratch + 0x80), 48) == 0);
    CHECK(XGetVideoFlags() == VdQueryVideoFlags());
    CHECK(XGetVideoStandard() == g.Load32(kScratch + 24));
  }
  Video() = {};
  CHECK(XGetVideoCapabilities() == 0);
  const std::vector<uint8_t> before = g_memory;
  CHECK(VdDisplayHardwareRequest() == 0);
  CHECK(g_memory == before);
}

void LocaleTests() {
  Fresh();
  const uint32_t buf = kScratch + 0x400, name = kScratch + 0x300;
  auto wide = [&](uint32_t at) {
    std::string out;
    for (; g.Load16(at); at += 2) out += char(g.Load16(at));
    return out;
  };
  const uint64_t noon_2020 = 132223104000000000ull + 12 * 36000000000ull;  // 2020-01-01 12:00 UTC
  XamFormatDateString(g, noon_2020, buf, 16);
  const std::string date = wide(buf);
  CHECK(date.size() == 10 && date[2] == '/' && date[5] == '/' && (date.substr(0, 5) == "01/01" || date.substr(0, 5) == "12/31"));
  XamFormatTimeString(g, noon_2020, buf, 16);
  const std::string time = wide(buf);
  CHECK(time.size() == 5 && time[2] == ':' && time.substr(3) == "00");
  XamFormatTimeString(g, noon_2020, buf, 3);
  CHECK(wide(buf).size() == 2);
  const char16_t file[] = u"skin.xur";
  for (size_t i = 0; i < 9; ++i) g.Store16(name + 2 * uint32_t(i), uint16_t(file[i]));
  CHECK(XamBuildSystemResourceLocator(g, u"xam", name, buf, 64) == 0 && wide(buf) == "file://media:/xam.xzp#skin.xur");
  CHECK(XamBuildResourceLocator(g, 0x1234ABCD, std::u16string(u"pkg"), std::u16string(u"a.png"), buf, 64) == 0 &&
        wide(buf) == "section://1234ABCD,pkg#a.png");
  CHECK(XamBuildSystemResourceLocator(g, u"xam", name, buf, 8) == 0 && wide(buf) == "file://");
}

// I/O completion ports: packets set by hand or by the transfers of an associated file, removed in order.
void CompletionPortTests() {
  Fresh();
  namespace fs = std::filesystem;
  const uint32_t out = kScratch + 0x200, iosb = kScratch + 0x220, info = kScratch + 0x240, buf = kScratch + 0x400,
                 text = kScratch + 0x100;
  const int64_t zero = 0;
  CHECK(Objects().CreateIoCompletion(g, out) == 0);
  const uint32_t port = g.Load32(out);
  CHECK(g.Load8(Objects().Lookup(port)) == 4);
  CHECK(Objects().SetIoCompletion(g, port, 1, 0x100, 0, 10) == 0 && Objects().SetIoCompletion(g, port, 2, 0x200, 0xC0000011, 0) == 0);
  CHECK(Objects().RemoveIoCompletion(g, port, out, out + 4, iosb, &zero) == 0 && g.Load32(out) == 1 && g.Load32(out + 4) == 0x100 &&
        g.Load32(iosb) == 0 && g.Load32(iosb + 4) == 10);
  CHECK(Objects().RemoveIoCompletion(g, port, out, out + 4, iosb, &zero) == 0 && g.Load32(out) == 2 && g.Load32(iosb) == 0xC0000011u);
  CHECK(Objects().RemoveIoCompletion(g, port, out, out + 4, iosb, &zero) == kStatusTimeout);
  // A remover waiting for ever gets the packet another thread sets.
  std::thread setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Objects().SetIoCompletion(g, port, 3, 0x300, 0, 4);
  });
  CHECK(Objects().RemoveIoCompletion(g, port, out, 0, 0, nullptr) == 0 && g.Load32(out) == 3);
  setter.join();
  // Not a port; not a handle.
  CHECK(Objects().CreateEvent(g, out + 8, 0, 0, 0) == 0);
  CHECK(Objects().SetIoCompletion(g, g.Load32(out + 8), 0, 0, 0, 0) == 0xC0000024u);
  CHECK(Objects().RemoveIoCompletion(g, 0x1234, 0, 0, 0, &zero) == kStatusInvalidHandle);
  // A file associated with the port: each transfer leaves a packet with the file's key.
  const fs::path root = fs::temp_directory_path() / "any360-sys-iocp";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);
  Files().Reset();
  Files().Mount("\\Device\\Harddisk0\\Partition1", root, false);
  Files().Link("hdd:", "\\Device\\Harddisk0\\Partition1");
  std::memcpy(g.host(text), "hdd:\\log.bin", 13);
  const uint32_t file = Files().Win32CreateFile(g, text, 0xC0000000, 2);
  CHECK(file != 0xFFFFFFFF);
  g.Store32(info, port);
  g.Store32(info + 4, 0x77);
  CHECK(Files().SetInformation(g, file, iosb, info, 8, kFileCompletionInformation) == 0);
  g.Store32(info, 0x1234);
  CHECK(Files().SetInformation(g, file, iosb, info, 8, kFileCompletionInformation) == kStatusInvalidHandle);
  std::memcpy(g.host(buf), "abcdef", 6);
  const uint32_t status = Files().WriteFile(g, file, 0, iosb, buf, 6, 0);
  Files().Completed(g, file, 0xABC, status, iosb);
  // An asynchronous file (no FILE_SYNCHRONOUS_IO_*) answers STATUS_PENDING, the transfer done.
  CHECK(status == 0x103 && Objects().RemoveIoCompletion(g, port, out, out + 4, iosb, &zero) == 0 && g.Load32(out) == 0x77 &&
        g.Load32(out + 4) == 0xABC && g.Load32(iosb + 4) == 6);
  // The port lives on while the file holds it, after its own handle is closed.
  CHECK(Objects().Close(port) == 0);
  Files().Completed(g, file, 0xDEF, 0, 0);
  CHECK(Objects().Close(file) == 0);
  Files().Reset();
  fs::remove_all(root, ec);
}

// Kernel odds and ends: symbolic link objects, address checks, kernel stacks, a thread's stack pointers.
void KernelOddsTests() {
  Fresh();
  Memory().Reset({{0x40000, 0x40000, 0x1000, false}});
  const uint32_t out = kScratch + 0x200, attrs = kScratch + 0x100, name = kScratch + 0x120, text = kScratch + 0x140;
  auto path = [&](const char* p) {
    g.Store32(attrs, 0);
    g.Store32(attrs + 4, name);
    g.Store16(name, uint16_t(std::strlen(p)));
    g.Store32(name + 4, text);
    std::memcpy(g.host(text), p, std::strlen(p) + 1);
    return attrs;
  };
  // Symbolic links opened by name and queried for their target, truncated to the buffer.
  Files().Reset();
  Files().Link("\\??\\D:", "\\Device\\Cdrom0");
  CHECK(Files().OpenSymbolicLink(g, out, path("\\??\\D:")) == 0);
  const uint32_t link = g.Load32(out), target = kScratch + 0x300;
  g.Store16(target, 0);
  g.Store16(target + 2, 64);
  g.Store32(target + 4, kScratch + 0x340);
  CHECK(Files().QuerySymbolicLink(g, link, target) == 0 && g.Load16(target) == 14 &&
        std::string(reinterpret_cast<const char*>(g.host(kScratch + 0x340)), 14) == "\\Device\\Cdrom0");
  g.Store16(target + 2, 4);
  CHECK(Files().QuerySymbolicLink(g, link, target) == 0 && g.Load16(target) == 4);
  CHECK(Files().OpenSymbolicLink(g, out, path("Z:")) == kStatusNoSuchFile);
  CHECK(Objects().Close(link) == 0 && Files().QuerySymbolicLink(g, link, target) == kStatusNoSuchFile);
  Files().Reset();
  // Kernel stacks: committed pages, valid addresses, the top returned and the bottom taken back.
  const uint32_t top = Memory().CreateKernelStack(g, 0x3000);
  CHECK(top != 0 && Memory().IsAddressValid(top - 4) == 1 && Memory().IsAddressValid(top - 0x3000) == 1);
  CHECK(Memory().DeleteKernelStack(top - 0x3000) == 0 && Memory().IsAddressValid(top - 4) == 0);
  CHECK(Memory().DeleteKernelStack(top - 0x3000) == 0xC0000001u && Memory().IsAddressValid(0x1000) == 0);
  // Encrypted memory: its own heap of 64 KB pages, 16 MB at most, never handed out otherwise.
  Memory().Reset({{0x40000, 0x10000, 0x1000, false}, {0x50000, 0x30000, 0x10000, false, true}});  // the test's guest memory
  CHECK(Memory().AllocateEncrypted(g, 0x8000, out) == 0 && g.Load32(out) >= 0x50000 && g.Load32(out) % 0x10000 == 0 &&
        Memory().IsAddressValid(g.Load32(out) + 0xFFFC) == 1);
  CHECK(Memory().AllocateEncrypted(g, 0, out + 4) == 0xC000000Du && Memory().AllocateEncrypted(g, 0x1000001, out + 4) == 0xC000000Du);
  CHECK(Memory().AllocateEncrypted(g, 0x10000, out + 4) == 0 && g.Load32(out + 4) == g.Load32(out) + 0x10000);
  const uint32_t first = g.Load32(out);
  CHECK(Memory().FreeEncrypted(g, out) == 0 && Memory().IsAddressValid(first) == 0 && Memory().FreeEncrypted(g, out) == 0xC000000Du);
  CHECK(MemoryManager::ImagePageTableEntry(0x82010000) == 0x201 && MemoryManager::ImagePageTableEntry(0x90003000) == 0x40000003 &&
        MemoryManager::ImagePageTableEntry(0x70000000) == 0);
  Memory().Reset({{0x40000, 0x40000, 0x1000, false}});
  // A string whose buffer the kernel allocated, freed and emptied.
  const uint32_t unicode = kScratch + 0x410, buffer = Memory().AllocatePages(g, 0x100);
  g.Store16(unicode, 6);
  g.Store16(unicode + 2, 0x100);
  g.Store32(unicode + 4, buffer);
  CHECK(buffer != 0 && Memory().IsAddressValid(buffer) == 1);
  RtlFreeUnicodeString(g, unicode);
  CHECK(g.Load32(unicode) == 0 && g.Load32(unicode + 4) == 0 && Memory().IsAddressValid(buffer) == 0);
  // A thread's stack fields replaced.
  const uint32_t pcr = kScratch + 0x800, kthread = kScratch + 0xC00;
  Threads().SetCurrentStackPointers(g, pcr, kthread, 0x50000, 0x58000, 0x54000);
  CHECK(g.Load32(kthread + 0xD0) == 0x50000 && g.Load32(kthread + 0x5C) == 0x58000 && g.Load32(kthread + 0x60) == 0x54000);
  CHECK(g.Load32(pcr + 0x70) == 0x58000 && g.Load32(pcr + 0x74) == 0x54000);
  Memory().Reset();
}

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
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo) == 0);
  CHECK(g.Load32(iosb) == 0 && g.Load32(iosb + 4) == kOpened);
  const uint32_t f = g.Load32(out);
  CHECK(f >= kHandleBase);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == 0 && g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "0123", 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == 0 && std::memcmp(g.host(buf), "4567", 4) == 0);
  g.Store64(out + 8, 12);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 100, out + 8) == 0 && g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "CDEF", 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 4, 0) == kStatusEndOfFile && g.Load32(iosb) == kStatusEndOfFile && g.Load32(iosb + 4) == 0);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 0, 0) == 0);  // zero length at the end succeeds
  // Opened without FILE_SYNCHRONOUS_IO_* (kernel32's FILE_FLAG_OVERLAPPED), a read is done at once but answers
  // STATUS_PENDING, its I/O status block holding the success, as the reference does.
  CHECK(Files().CreateFile(g, out + 16, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen, 0) == 0);
  g.Store64(out + 8, 4);
  CHECK(Files().ReadFile(g, g.Load32(out + 16), 0, iosb, buf, 4, out + 8) == 0x103 && g.Load32(iosb) == 0 &&
        g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "4567", 4) == 0);
  CHECK(Files().ReadFile(g, g.Load32(out + 16), 0, iosb, buf, 4, out + 8) == 0x103);
  g.Store64(out + 8, 100);
  CHECK(Files().ReadFile(g, g.Load32(out + 16), 0, iosb, buf, 4, out + 8) == kStatusEndOfFile);  // a failure is not pending
  // A handle keeps its host file between reads: a file read a byte at a time, its size asked before every byte, as
  // titles do (Crackdown: 22,000 bytes before its title screen). What the handle keeps never hides a change: a write
  // through another handle, or one the host makes outside the file layer, shows at once in the file's size and in
  // its bytes, and the file can still be renamed and removed. What a handle query says of the file is what a path
  // query says at the same moment (a title compares them).
  {
    const char* bytes = "\\Device\\Harddisk0\\Partition1\\bytes.bin";
    CHECK(Files().CreateFile(g, out + 24, kRead | kWrite, path(bytes), iosb, 0, kOverwriteIf, kFileNonDirectoryFile | kFileSynchronousIo) == 0);
    const uint32_t w = g.Load32(out + 24);
    std::memcpy(g.host(buf), "ABCDEFGH", 8);
    CHECK(Files().WriteFile(g, w, 0, iosb, buf, 8, 0) == 0 && g.Load32(iosb + 4) == 8);
    CHECK(Files().CreateFile(g, out + 28, kRead, path(bytes), iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo) == 0);
    const uint32_t r = g.Load32(out + 28);
    std::string seen;
    bool sizes = true;
    for (int i = 0; i < 8; ++i) {
      sizes = sizes && Files().QueryInformation(g, r, iosb, info, 56, kFileNetworkOpenInformation) == 0 && g.Load64(info + 40) == 8;
      if (Files().ReadFile(g, r, 0, iosb, buf, 1, 0) == 0 && g.Load32(iosb + 4) == 1) seen += char(g.Load8(buf));
    }
    CHECK(sizes && seen == "ABCDEFGH");
    CHECK(Files().ReadFile(g, r, 0, iosb, buf, 1, 0) == kStatusEndOfFile);
    std::memcpy(g.host(buf), "IJKL", 4);
    CHECK(Files().WriteFile(g, w, 0, iosb, buf, 4, 0) == 0);
    CHECK(Files().QueryInformation(g, r, iosb, info, 56, kFileNetworkOpenInformation) == 0 && g.Load64(info + 40) == 12);
    CHECK(Files().ReadFile(g, r, 0, iosb, buf, 8, 0) == 0 && g.Load32(iosb + 4) == 4 && std::memcmp(g.host(buf), "IJKL", 4) == 0);
    { std::ofstream(root / "hdd" / "bytes.bin", std::ios::binary | std::ios::app) << "MN"; }
    CHECK(Files().ReadFile(g, r, 0, iosb, buf, 8, 0) == 0 && g.Load32(iosb + 4) == 2 && std::memcmp(g.host(buf), "MN", 2) == 0);
    CHECK(Files().QueryInformation(g, r, iosb, info, 56, kFileNetworkOpenInformation) == 0 && g.Load64(info + 40) == 14);
    // The times and the size by the handle and by the path: the same.
    CHECK(Files().QueryInformation(g, r, iosb, info, 56, kFileNetworkOpenInformation) == 0);
    std::vector<uint8_t> by_handle(g.host(info), g.host(info) + 56);
    CHECK(Files().QueryFullAttributes(g, path(bytes), info) == 0 && std::memcmp(g.host(info), by_handle.data(), 56) == 0);
    const char* moved = "\\Device\\Harddisk0\\Partition1\\moved.bin";
    std::memcpy(g.host(pat), bytes, std::strlen(bytes) + 1);
    std::memcpy(g.host(pat + 0x80), moved, std::strlen(moved) + 1);
    CHECK(Files().Win32MoveFile(g, pat, pat + 0x80) == 1 && fs::exists(root / "hdd" / "moved.bin") && !fs::exists(root / "hdd" / "bytes.bin"));
    CHECK(Objects().Close(r) == 0 && Objects().Close(w) == 0);
    CHECK(Files().Win32DeleteFile(g, pat + 0x80) == 1 && !fs::exists(root / "hdd" / "moved.bin"));
  }
  // A read into guest memory that a renderer watches (a read-only page, a fault handler that opens it when code
  // writes there): the host's kernel does not write to such a page, and the read still gives every byte, the
  // watched page written as the title's own code would write it, once. With the handle keeping its host file and
  // without.
  {
    constexpr uint32_t kPage = 0x1000;
    uint8_t* region = test::WatchedMemoryCreate(16 * kPage);
    const Guest watched{region};
    std::vector<char> content(3 * kPage);
    for (size_t i = 0; i < content.size(); ++i) content[i] = char(i * 7 + (i >> 8));
    { std::ofstream(root / "hdd" / "big.bin", std::ios::binary).write(content.data(), std::streamsize(content.size())); }
    CHECK(Files().CreateFile(g, out + 24, kRead, path("\\Device\\Harddisk0\\Partition1\\big.bin"), iosb, 0, kOpen,
                             kFileNonDirectoryFile | kFileSynchronousIo) == 0);
    const uint32_t big = g.Load32(out + 24);
    for (int keep = 1; keep >= 0; --keep) {
      Files().SetKeepHostFiles(keep != 0);
      std::memset(region, 0, 16 * kPage);
      test::WatchedMemoryWatch(3 * kPage);
      watched.Store64(0x108, 0);
      CHECK(Files().ReadFile(watched, big, 0, 0x100, 2 * kPage, 3 * kPage, 0x108) == 0 && watched.Load32(0x104) == 3 * kPage);
      CHECK(std::memcmp(region + 2 * kPage, content.data(), content.size()) == 0);
      CHECK(test::WatchedMemoryFaults() == 1);
    }
    Files().SetKeepHostFiles(true);
    CHECK(Objects().Close(big) == 0);
    test::WatchedMemoryDestroy();
  }
  // More files open at once than a host lets a process hold (512 streams with MSVC's C library, 1024 descriptors
  // on many a Linux): every handle still reads, and reads again after the others were read. No title at hand holds
  // that many; one that did must not find its reads giving nothing past a limit it knows nothing of.
  {
    std::vector<uint32_t> handles;
    bool opened = true, read = true;
    for (int i = 0; i < 1200; ++i) {
      opened = opened && Files().CreateFile(g, out + 24, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen,
                                            kFileNonDirectoryFile | kFileSynchronousIo) == 0;
      handles.push_back(g.Load32(out + 24));
    }
    for (int pass = 0; pass < 2; ++pass) {
      for (uint32_t h : handles) {
        g.Store64(out + 8, 4 * pass);
        read = read && Files().ReadFile(g, h, 0, iosb, buf, 4, out + 8) == 0 && g.Load32(iosb + 4) == 4 &&
               std::memcmp(g.host(buf), pass ? "4567" : "0123", 4) == 0;
      }
    }
    CHECK(opened && read);
    for (uint32_t h : handles) Objects().Close(h);
  }
  CHECK(Objects().Close(g.Load32(out + 16)) == 0);
  // The read signals an event; an unknown event handle is refused.
  const uint32_t ev = out + 16;
  CHECK(Objects().CreateEvent(g, ev, 0, 0, 0) == 0);
  g.Store64(out + 8, 0);
  CHECK(Files().ReadFile(g, f, g.Load32(ev), iosb, buf, 2, out + 8) == 0 && Objects().WaitForHandle(g, g.Load32(ev), kMainThread, nullptr) == 0);
  CHECK(Files().ReadFile(g, f, kHandleBase + 0xF00, iosb, buf, 2, 0) == 0xC0000008u);
  // Information classes.
  CHECK(Files().QueryInformation(g, f, iosb, info, 56, kFileNetworkOpenInformation) == 0 && g.Load64(info + 40) == 16 &&
        g.Load32(info + 48) == kAttributeNormal && g.Load64(info + 24) > 0);
  CHECK(g.Load64(info + 32) == 0x200);  // the allocation: the size rounded up to a 512-byte sector
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
  // A null device (the hard drive's raw partitions): it opens; reads and writes succeed and move nothing.
  Files().MountNull("\\Device\\Harddisk0\\Cache0");
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("\\Device\\Harddisk0\\Cache0"), iosb, 0, kOpen, kFileSynchronousIo) == 0);
  CHECK(Files().ReadFile(g, g.Load32(out), 0, iosb, buf, 512, 0) == 0 && g.Load32(iosb + 4) == 0);
  CHECK(Files().WriteFile(g, g.Load32(out), 0, iosb, buf, 512, 0) == 0);
  CHECK(Objects().Close(g.Load32(out)) == 0);
  CHECK(Files().CreateFile(g, out, kRead, path("\\Device\\Harddisk0\\Cache0\\file"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media"), iosb, 0, kOpen, kFileNonDirectoryFile) == kStatusFileIsADirectory);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\data.bin"), iosb, 0, kOpen, kFileDirectoryFile) == kStatusNotADirectory);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\..\\x"), iosb, 0, kOpen, 0) == kStatusObjectPathNotFound);
  // Full attributes by path; a path relative to an open directory handle.
  CHECK(Files().QueryFullAttributes(g, path("\\Device\\Cdrom0\\MEDIA\\Levels\\two.lvl"), info) == 0 && g.Load64(info + 40) == 2);
  CHECK(Files().QueryFullAttributes(g, path("game:\\media\\Levels"), info) == 0 && g.Load32(info + 48) == kAttributeDirectory);
  CHECK(Files().QueryFullAttributes(g, path("game:\\media\\x.y"), info) == kStatusObjectNameNotFound);
  CHECK(Files().CreateFile(g, out, kRead, path("game:\\media\\levels"), iosb, 0, kOpen, kFileDirectoryFile) == 0);
  const uint32_t d = g.Load32(out);
  CHECK(Files().CreateFile(g, out, kRead, path("one.lvl", d), iosb, 0, kOpen, kFileSynchronousIo) == 0);
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
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\save.dat"), iosb, 0, kOpenIf, kFileSynchronousIo) == 0 && g.Load32(iosb + 4) == kCreated);
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
  g.Store32(out + 8, 0xAAAA);
  CHECK(XamState().ShowMarketplaceDownloadItemsUI(g, 0, out + 8, ov) == kErrorIoPending && g.Load32(out + 8) == 0);
  CHECK(XamState().ShowDeviceSelectorUI(g, out + 8, 0) == 0 && g.Load32(out + 8) == 1);
  // Content: create a save, write through its root, close, enumerate, delete.
  const uint32_t data = kScratch + 0xA00;
  std::memset(g.host(data), 0, 0x134);
  g.Store32(data, 1);
  g.Store32(data + 4, 1);  // saved game
  std::memcpy(g.host(data + 0x108), "slot0", 5);
  g.Store16(data + 8, 'M');  // the display name, "My" (UTF-16BE)
  g.Store16(data + 10, 'y');
  std::memcpy(g.host(str), "save", 5);
  CHECK(XamState().ContentCreate(g, 0, str, data, 3, out, out + 4, 0) == kErrorFileNotFound);  // open existing: none yet
  // A package whose name is empty (or only dots) would be its type's folder itself: refused by every call, and the
  // packages already there are left alone (a title asked for one, "create always", and lost its saved games).
  {
    fs::create_directories(content / "00000001" / "kept");
    { std::ofstream(content / "00000001" / "kept" / "data.bin", std::ios::binary) << "saved"; }
    const uint32_t nameless = kScratch + 0xE00;
    std::memset(g.host(nameless), 0, 0x134);
    g.Store32(nameless, 1);
    g.Store32(nameless + 4, 1);
    for (const char* name : {"", ".", "..", " . "}) {
      std::memset(g.host(nameless + 0x108), 0, 42);
      std::memcpy(g.host(nameless + 0x108), name, std::strlen(name));
      for (uint32_t mode : {1u, 2u, 3u, 4u, 5u}) CHECK(XamState().ContentCreate(g, 0, str, nameless, mode, out, out + 4, 0) != 0);
      CHECK(XamState().ContentDelete(g, 0, nameless, 0) == kErrorFileNotFound);
      CHECK(fs::exists(content / "00000001" / "kept" / "data.bin"));
    }
    CHECK(!fs::exists(content / "Headers" / "00000001" / ".header"));
    fs::remove_all(content / "00000001" / "kept");
  }
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
  // Through an overlapped structure with no disposition pointer, the disposition is the length (opened: 2).
  CHECK(XamState().ContentCreate(g, 0, str, data, 3, 0, 0, ov) == kErrorIoPending && g.Load32(ov) == 0 && g.Load32(ov + 4) == 2 &&
        g.Load32(ov + 0x18) == 0);
  CHECK(XamState().ContentClose(g, str, 0) == 0);
  CHECK(XamState().ContentCreateEnumerator(g, 0, 0, 1, 0, 1, out + 12, out + 16) == 0 && g.Load32(out + 12) == 0x134);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x134, out + 20, 0) == 0 && g.Load32(out + 20) == 1 &&
        std::memcmp(g.host(buf + 0x108), "slot0", 5) == 0 && g.Load32(buf + 4) == 1);
  // The display name the package was created with, kept in its header (as the runtime keeps it), not its file name.
  CHECK(fs::exists(content / "Headers" / "00000001" / "slot0.header") && g.Load16(buf + 8) == 'M' && g.Load16(buf + 10) == 'y' &&
        g.Load16(buf + 12) == 0);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x134, out + 20, 0) == kErrorNoMoreFiles && g.Load32(out + 20) == 0);
  CHECK(XamState().Enumerate(g, g.Load32(out + 16), 0, buf, 0x10, out + 20, 0) == kErrorInsufficientBuffer);
  CHECK(XamState().GetPrivateEnumStructure(g, g.Load32(out + 16), out + 24) == 0 && g.Load32(out + 24) != 0);
  CHECK(XamState().ContentDelete(g, 0, data, 0) == 0 && !fs::exists(content / "00000001" / "slot0") &&
        !fs::exists(content / "Headers" / "00000001" / "slot0.header"));
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
  // The packet number moves only when the state changes.
  CHECK(XamState().InputGetState(g, 0, 0, buf) == 0 && g.Load32(buf) == 2);
  static uint32_t vibrated = 0;
  XamState().host().vibrate = [](uint32_t, uint16_t left, uint16_t right) { vibrated = uint32_t(left) << 16 | right; };
  g.Store16(buf, 0x1234);
  g.Store16(buf + 2, 0x5678);
  CHECK(XamState().InputSetState(g, 0, buf) == 0 && vibrated == 0x12345678);
  // Keystrokes from the state's transitions: A held (VK_PAD_A down), then nothing until it repeats or is released.
  CHECK(XamState().InputGetKeystroke(g, 0, 0, buf) == 0 && g.Load16(buf) == 0x5800 && g.Load16(buf + 4) == 1 && g.Load8(buf + 6) == 0);
  CHECK(XamState().InputGetKeystroke(g, 0, 0, buf) == kErrorEmpty);
  XamState().host().controller_state = [](uint32_t, GamepadState& s) {
    s.thumb_ly = 0x7000;  // A released, the left stick up
    return true;
  };
  CHECK(XamState().InputGetKeystroke(g, 0xFF, 0, buf) == 0 && g.Load16(buf) == 0x5800 && g.Load16(buf + 4) == 2);
  CHECK(XamState().InputGetKeystroke(g, 0xFF, 0, buf) == 0 && g.Load16(buf) == 0x5820 && g.Load16(buf + 4) == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  CHECK(XamState().InputGetKeystroke(g, 0, 0, buf) == 0 && g.Load16(buf) == 0x5820 && g.Load16(buf + 4) == 5);  // repeat
  CHECK(XamState().InputGetKeystroke(g, 1, 0, buf) == kErrorDeviceNotConnected);
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
  // The launcher's bindings reach the title below XamInputGetState: A and B swapped, the south button held reads B;
  // the identity again once cleared.
  XamState().SetInputBindings(InputBindings::Parse(R"({"version":1,"profiles":{"gamepad":{"xbox:a":["east"],"xbox:b":["south"]}}})"));
  XamState().host().controller_state = [](uint32_t, GamepadState& s) {
    s.buttons = 0x1000;  // the south button
    return true;
  };
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  CHECK(XamState().InputGetState(g, 0, 0, buf) == 0 && g.Load16(buf + 4) == 0x2000);
  XamState().SetInputBindings(InputBindings{});
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  CHECK(XamState().InputGetState(g, 0, 0, buf) == 0 && g.Load16(buf + 4) == 0x1000);
  while (XamState().InputGetKeystroke(g, 0, 0, buf) == 0) {
  }
  // A run recorded and played back: the host's controller (sampled every millisecond) holds A, then the left stick
  // and the right trigger, then nothing, over a 300 ms recording; its file has those three changes in order. Played
  // back from 50 ms on, the first controller reads them at their times while the host's controller (every button
  // held) is left out.
  {
    static std::atomic<int> phase{0};
    XamState().host().controller_state = [](uint32_t, GamepadState& s) {
      const int p = phase.load();
      if (p == 1) s.buttons = 0x1000;
      if (p == 2) s.thumb_lx = 12000, s.right_trigger = 200;
      if (p == 3) s.buttons = 0xFFFF;
      return true;
    };
    const std::string path = (fs::temp_directory_path() / "any360_input_record_test.txt").string();
    CHECK(XamState().SetInputRecorder(path, std::chrono::milliseconds(0), std::chrono::milliseconds(300)));
    XamState().StartInputSampling(std::chrono::microseconds(1000));
    phase = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    phase = 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    phase = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    XamState().StopInputSampling();
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    InputRecording recorded;
    std::string why;
    CHECK(InputRecording::Parse(text, recorded, &why));
    const auto& e = recorded.events();
    std::printf("sys_object_tests: input recording: %zu changes, at %lld, %lld, %lld ms\n", e.size(),
                e.size() > 0 ? (long long)std::chrono::duration_cast<std::chrono::milliseconds>(e[0].at).count() : -1,
                e.size() > 1 ? (long long)std::chrono::duration_cast<std::chrono::milliseconds>(e[1].at).count() : -1,
                e.size() > 2 ? (long long)std::chrono::duration_cast<std::chrono::milliseconds>(e[2].at).count() : -1);
    CHECK(e.size() == 3);
    if (e.size() == 3) {
      CHECK(e[0].state.buttons == 0x1000 && e[1].state.thumb_lx == 12000 && e[1].state.right_trigger == 200 &&
            e[2].state == GamepadState{});
      CHECK(e[1].at - e[0].at >= std::chrono::milliseconds(60) && e[2].at - e[1].at >= std::chrono::milliseconds(60));
      phase = 3;  // the host's controller: every button
      // The changes queued while nothing polled come out one console frame at a time: polled until it shows it.
      for (int i = 0; i < 40 && !(XamState().InputGetState(g, 0, 0, buf) == 0 && g.Load16(buf + 4) == 0xFFFF); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      CHECK(g.Load16(buf + 4) == 0xFFFF);
      const auto start = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
      XamState().SetInputReplay(recorded, std::chrono::milliseconds(50));
      auto read_at = [&](std::chrono::steady_clock::duration since) {
        std::this_thread::sleep_until(start + since);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));  // a console frame's step through the queue
        CHECK(XamState().InputGetState(g, 0, 0, buf) == 0);
      };
      read_at(e[0].at);
      CHECK(g.Load16(buf + 4) == 0x1000 && int16_t(g.Load16(buf + 8)) == 0);
      read_at(e[1].at);
      CHECK(g.Load16(buf + 4) == 0 && g.Load8(buf + 7) == 200 && int16_t(g.Load16(buf + 8)) == 12000);
      read_at(e[2].at);
      CHECK(g.Load16(buf + 4) == 0 && g.Load8(buf + 7) == 0 && int16_t(g.Load16(buf + 8)) == 0);
    }
    XamState().SetInputReplay(InputRecording{}, std::chrono::milliseconds(0));
    phase = 0;
    in.close();
    std::error_code removed;
    std::filesystem::remove(path, removed);
    while (XamState().InputGetKeystroke(g, 0, 0, buf) == 0) {
    }
  }
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
  // Through an overlapped: the extended error is the result as an HRESULT, as the reference writes it for
  // XamEnumerate (Call of Duty: Black Ops reads anything else from an empty enumeration as content that changed).
  CHECK(XamState().UserCreateAchievementEnumerator(g, 0, 0, 8, out, out + 4) == 0);
  CHECK(XamState().Enumerate(g, g.Load32(out + 4), 0, buf, 8 * 0x24, 0, ov) == kErrorIoPending &&
        g.Load32(ov) == kErrorNoMoreFiles && g.Load32(ov + 4) == 0 && g.Load32(ov + 0x18) == 0x80070012);
  CHECK(XamState().UserAreUsersFriends(g, 1, 0, ov) == kErrorIoPending && g.Load32(ov) == kErrorFunctionFailed &&
        g.Load32(ov + 0x18) == (0x80070000u | kErrorNoSuchUser));
  CHECK(XamState().UserCreateAchievementEnumerator(g, 0, 0, 0, out, out + 4) == kErrorInvalidParameter);
  CHECK(XamState().ShowLiveUI() == 0);
  // The marketplace's download screen: S_OK, the overlapped completed (the title waits on it); user 4 refused.
  g.Store32(out, 0xAAAA);
  CHECK(XamState().ShowMarketplaceDownloadItemsUI(g, 0, out, 0) == 0 && g.Load32(out) == 0);
  CHECK(XamState().ShowMarketplaceDownloadItemsUI(g, 4, out, 0) == kErrorInvalidParameter);
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
  // The callback gets the address of a word holding the registered argument, not the argument.
  CHECK(mixes == 2 && argument != 0xABCD && gm.Load32(uint32_t(argument)) == 0xABCD);
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
// The aggregate content enumerator: the hard drive's packages of a type with their owner and title.
void ContentAggregateTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  std::error_code ec;
  const fs::path content = fs::temp_directory_path() / "any360-sys-aggregate";
  fs::remove_all(content, ec);
  fs::create_directories(content / "00000001" / "slotB");
  fs::create_directories(content / "00000001" / "slotA");
  XamState().title().content_root = content;
  XamState().title().title_id = 0x4D5307E6;
  const uint32_t out = kScratch + 0x200, buf = kScratch + 0x400;
  CHECK(XamState().ContentAggregateCreateEnumerator(g, 0, 1, out) == 0);
  const uint32_t handle = g.Load32(out);
  CHECK(XamState().Enumerate(g, handle, 0, buf, 0x148, out + 4, 0) == 0 && g.Load32(out + 4) == 1);
  CHECK(g.Load32(buf) == 1 && g.Load32(buf + 4) == 1 && std::string(reinterpret_cast<const char*>(g.host(buf + 0x108))) == "slotA");
  CHECK(g.Load64(buf + 0x138) == XamState().title().xuid && g.Load32(buf + 0x140) == 0x4D5307E6);
  CHECK(XamState().Enumerate(g, handle, 0, buf, 0x148, out + 4, 0) == 0 &&
        std::string(reinterpret_cast<const char*>(g.host(buf + 0x108))) == "slotB");
  CHECK(XamState().Enumerate(g, handle, 0, buf, 0x148, out + 4, 0) != 0);  // no more
  CHECK(XamState().ContentAggregateCreateEnumerator(g, 2, 1, out) == 0x80070057u);
  XamState().Reset();
  fs::remove_all(content, ec);
}

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
  // A zero alertable delay (SleepEx(0, TRUE)) yields and returns with none queued (the look without a lock), and
  // ends at once with one queued.
  CHECK(Threads().Delay(gm, pcr, 0, true) == 0 && Threads().DeliverApcs(gm, pcr, 0x7F000) == 0);
  CHECK(Threads().QueueApc(gm, thread_handle, 0, 0x82000000, 7, 8, 9) == 0 && Threads().Delay(gm, pcr, 0, true) == kStatusUserApc);
  CHECK(Threads().DeliverApcs(gm, pcr, 0x7F000) == 1 && delivered.back() == std::vector<uint64_t>({7, 8, 9}) &&
        Threads().PendingApcs(kthread) == 0);
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

// Many threads releasing and taking one semaphore and one synchronization event at once: every release is taken once,
// none lost (the dispatcher's lock), and the handle table hands out distinct handles to threads creating events.
void ConcurrentDispatcher() {
  Objects().Reset();
  Objects().SetKernelHeap(0x10000, 0x70000);
  constexpr uint32_t kSem = 0x2000, kEvent = 0x2040, kThreads = 4, kRounds = 20000;
  Objects().InitializeSemaphore(g, kSem, 0, 0x7FFFFFFF);
  std::atomic<uint32_t> taken{0}, timeouts{0};
  std::vector<std::thread> threads;
  const int64_t two_seconds = -20'000'000;
  for (uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([] {
      for (uint32_t i = 0; i < kRounds; ++i) {
        uint32_t status;
        Objects().ReleaseSemaphore(g, kSem, 1, status);
      }
    });
    threads.emplace_back([&, t] {
      for (uint32_t i = 0; i < kRounds; ++i) {
        if (Objects().WaitForObject(g, kSem, kMainThread + 0x10 * t, &two_seconds) == 0) ++taken;
        else ++timeouts;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(taken == kThreads * kRounds && timeouts == 0 && g.Load32(kSem + 4) == 0);
  // A synchronization event set by one thread and taken by another, round after round.
  Objects().InitializeEvent(g, kEvent, 1, 0);
  std::atomic<uint32_t> woken{0};
  std::thread waiter([&] {
    for (uint32_t i = 0; i < 2000; ++i) {
      if (Objects().WaitForObject(g, kEvent, kOtherThread, &two_seconds) == 0) ++woken;
      Objects().SetEvent(g, kEvent + 0x20);  // hand back: an event of its own
    }
  });
  Objects().InitializeEvent(g, kEvent + 0x20, 1, 0);
  for (uint32_t i = 0; i < 2000; ++i) {
    Objects().SetEvent(g, kEvent);
    Objects().WaitForObject(g, kEvent + 0x20, kMainThread, &two_seconds);
  }
  waiter.join();
  CHECK(woken == 2000);
  // Handles from four threads at once: all distinct.
  std::vector<std::vector<uint32_t>> made(kThreads);
  threads.clear();
  for (uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (uint32_t i = 0; i < 500; ++i) {
        const uint32_t out = kScratch + 0x100 * t;
        if (Objects().CreateEvent(g, out, 0, 0, 0) == 0) made[t].push_back(g.Load32(out));
      }
    });
  }
  for (auto& thread : threads) thread.join();
  std::vector<uint32_t> all;
  for (const auto& m : made) all.insert(all.end(), m.begin(), m.end());
  std::sort(all.begin(), all.end());
  CHECK(all.size() == kThreads * 500 && std::adjacent_find(all.begin(), all.end()) == all.end());
}

// A binary profile setting (a title's options) kept on the host survives a new run, as the runtime keeps it.
void ProfileSettingsKept() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  const fs::path profile = fs::temp_directory_path() / "any360-sys-profile";
  std::error_code ec;
  fs::remove_all(profile, ec);
  XamState().title().profile_root = profile;
  const uint32_t setting = kScratch + 0x400, bytes = kScratch + 0x500, ids = kScratch + 0x600, size = kScratch + 0x610;
  const uint32_t buffer = 0x10000;
  std::memset(g.host(setting), 0, 40);
  g.Store32(setting + 16, 0x63E83FFF);
  g.Store8(setting + 24, 6);
  g.Store32(setting + 32, 5);
  g.Store32(setting + 36, bytes);
  std::memcpy(g.host(bytes), "CRAK!", 5);
  CHECK(XamState().UserWriteProfileSettings(g, 0, 0, 1, setting, 0) == 0);
  CHECK(fs::file_size(profile / "63E83FFF", ec) == 5);
  XamState().Reset();  // a new run
  XamState().title().profile_root = profile;
  g.Store32(ids, 0x63E83FFF);
  g.Store32(size, 0x1000);
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 1, ids, size, buffer, 0) == 0);
  CHECK(g.Load32(buffer + 8) == 2 && g.Load32(buffer + 8 + 32) == 5);  // the length written, not the maximum
  CHECK(std::memcmp(g.host(g.Load32(buffer + 8 + 36)), "CRAK!", 5) == 0);
  g.Store32(ids, 0x10040003);  // vibration, never written: on (3), as a new console profile
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 1, ids, size, buffer, 0) == 0 && g.Load32(buffer + 8) == 1 &&
        g.Load32(buffer + 8 + 32) == 3);
  g.Store32(ids, 0x63E83FFE);  // never written: the default
  CHECK(XamState().UserReadProfileSettings(g, 0, 0, 0, 0, 1, ids, size, buffer, 0) == 0 && g.Load32(buffer + 8) == 1);
  fs::remove_all(profile, ec);
}

// The input queue: changes sampled faster than polls arrive, none lost; a step takes changes of different inputs
// together, a second change of the same input waits for the next step; the sticks queue nothing and are live.
// The input script: presses at their times for their hold, several buttons, entries it cannot read reported.
void InputScriptTests() {
  using namespace std::chrono_literals;
  std::string error;
  const InputScript s = InputScript::Parse("20:start,24.5:a/500,30:a+x,bad,31:nope,-1:a,40:b/0", &error);
  CHECK(s.presses().size() == 3);
  CHECK(s.ButtonsAt(19999ms) == 0 && s.ButtonsAt(20s) == 0x0010 && s.ButtonsAt(20199ms) == 0x0010 &&
        s.ButtonsAt(20200ms) == 0);
  CHECK(s.ButtonsAt(24500ms) == 0x1000 && s.ButtonsAt(24999ms) == 0x1000 && s.ButtonsAt(25s) == 0);
  CHECK(s.ButtonsAt(30100ms) == (0x1000 | 0x4000));
  CHECK(error == "bad, 31:nope, -1:a, 40:b/0");
  CHECK(InputScript::Parse("").empty());
  // A recording: changes only (a neutral start and repeats dropped), times never going back, the state at any time,
  // and its text read back the same, every field at its limits.
  InputRecording r;
  const GamepadState a{0x1000, 0, 0, 0, 0, 0, 0}, run{0x0000, 0, 255, -32768, 32767, 1234, -5};
  r.Add(0ms, GamepadState{});
  r.Add(10ms, a);
  r.Add(12ms, a);
  r.Add(500ms, run);
  r.Add(400ms, GamepadState{});  // earlier than the last: kept at the last's time
  CHECK(r.events().size() == 3 && r.events()[0].at == 10ms && r.events()[2].at == 500ms && r.length() == 500ms);
  CHECK(r.StateAt(9ms) == GamepadState{} && r.StateAt(10ms) == a && r.StateAt(499ms) == a && r.StateAt(500ms) == GamepadState{});
  InputRecording back;
  std::string why;
  CHECK(InputRecording::Parse(r.Text(), back, &why) && back.events().size() == 3);
  for (size_t i = 0; i < back.events().size() && i < 3; ++i) {
    CHECK(back.events()[i].at == r.events()[i].at && back.events()[i].state == r.events()[i].state);
  }
  InputRecording two;
  two.Add(1ms, a);
  two.Add(2ms, run);
  CHECK(InputRecording::Parse(two.Text(), back) && back.events().size() == 2 && back.events()[1].state == run);
  CHECK(two.Text(1).find(InputRecording::kHeader) == std::string::npos && two.Text(1).rfind("2000 0000 0 255 -32768 32767 1234 -5", 0) == 0);
  CHECK(!InputRecording::Parse("not a recording\n1 0 0 0 0 0 0 0\n", back, &why) && why.find("not an Any360") == 0);
  CHECK(!InputRecording::Parse(std::string(InputRecording::kHeader) + "\n5 0 0 0 40000 0 0 0\n", back, &why) &&
        why.find("line 2") == 0);
  CHECK(InputRecording::Parse(std::string(InputRecording::kHeader) + "\r\n\r\n", back) && back.empty());
}

void InputQueueTests() {
  using namespace std::chrono_literals;
  const InputQueue::Clock::time_point t0{};
  constexpr uint16_t kX = 0x4000, kY = 0x8000, kRb = 0x0200;
  auto state = [](uint16_t buttons, int16_t lx = 0, int16_t rx = 0) {
    GamepadState s;
    s.buttons = buttons;
    s.thumb_lx = lx;
    s.thumb_rx = rx;
    return s;
  };
  {
    // Within one console frame: RB, X, Y, RB released and pressed again, X released, X again (a double tap).
    InputQueue q;
    q.Sample(state(kRb), t0 + 1ms);
    q.Sample(state(kRb | kX), t0 + 2ms);
    q.Sample(state(kRb | kX | kY), t0 + 3ms);
    q.Sample(state(kX | kY), t0 + 4ms);
    q.Sample(state(kRb | kX | kY), t0 + 5ms);
    q.Sample(state(kRb | kY), t0 + 6ms);
    q.Sample(state(kRb | kX | kY), t0 + 7ms);
    CHECK(q.pending() == 7);
    // Step 1: RB, X and Y together; RB's release waits (it changed already).
    GamepadState s = q.Present(t0 + 8ms);
    CHECK(s.buttons == (kRb | kX | kY) && q.pending() == 4);
    // Polls inside the same console frame present the same state.
    CHECK(q.Present(t0 + 12ms) == s);
    // Step 2: RB released; pressed again waits.
    CHECK(q.Present(t0 + 25ms).buttons == (kX | kY) && q.pending() == 3);
    // Step 3: RB again and X released together.
    CHECK(q.Present(t0 + 42ms).buttons == (kRb | kY) && q.pending() == 1);
    // Step 4: X pressed again (the double tap counted twice); then the live state.
    CHECK(q.Present(t0 + 59ms).buttons == (kRb | kX | kY) && q.pending() == 0);
    // A press and release between two polls far apart: down at one poll, up at the next.
    q.Sample(state(kY), t0 + 100ms);
    q.Sample(state(kX | kY), t0 + 101ms);
    CHECK(q.Present(t0 + 200ms).buttons == kY);
    CHECK(q.Present(t0 + 201ms).buttons == kY && q.pending() == 1);  // inside the frame: held
    CHECK(q.Present(t0 + 217ms).buttons == (kX | kY));
  }
  {
    // The sticks queue nothing and are live, even while buttons wait: a camera stick follows, a released stick stops.
    InputQueue q;
    for (int i = 0; i < 1000; ++i) {
      q.Sample(state(0, 0x7000, int16_t((i % 2) ? 0x7000 : -0x7000)), t0 + std::chrono::microseconds(i * 100));
    }
    CHECK(q.pending() == 0);
    q.Sample(state(kX, 0x7000), t0 + 101ms);
    q.Sample(state(0, 0x7000), t0 + 102ms);
    CHECK(q.Present(t0 + 103ms).buttons == kX && q.pending() == 1);
    q.Sample(state(0, 0, 0x1234), t0 + 104ms);  // the left stick released, the right one moved
    GamepadState s = q.Present(t0 + 105ms);
    CHECK(s.buttons == kX && s.thumb_lx == 0 && s.thumb_rx == 0x1234);
    // A trigger shows the step's value while its change waits, the live value otherwise.
    GamepadState t = state(0);
    t.right_trigger = 200;
    q.Sample(t, t0 + 130ms);
    t.right_trigger = 0;
    q.Sample(t, t0 + 131ms);
    s = q.Present(t0 + 132ms);  // steps: X released and the trigger pressed together
    CHECK(s.buttons == 0 && s.right_trigger == 200 && q.pending() == 1);
    t.left_trigger = 10;  // under its threshold: live
    q.Sample(t, t0 + 133ms);
    CHECK(q.Present(t0 + 134ms).left_trigger == 10);
    // With a step of 0 every poll steps.
    q.Sample(state(kX), t0 + 230ms);
    q.Sample(state(0), t0 + 231ms);
    s = q.Present(t0 + 232ms, {});  // the trigger released and X pressed
    CHECK(s.right_trigger == 0 && s.buttons == kX);
    CHECK(q.Present(t0 + 232ms, {}).buttons == 0 && q.pending() == 0);
  }
  {
    // Keystrokes come from the samples, every one of them, the sticks' directions included, whatever the polls.
    InputQueue q;
    q.Sample(state(0x1000), t0 + 1ms);  // A
    q.Sample(state(0), t0 + 2ms);
    q.Sample(state(0x1000), t0 + 3ms);
    q.Sample(state(0x1000, 0x7000), t0 + 3ms);  // the left stick right
    uint16_t key = 0, flags = 0;
    int downs = 0, ups = 0, right = 0;
    while (q.NextKeystroke(t0 + 4ms, key, flags)) {
      if (key == 0x5800) (flags == 1 ? downs : ups) += 1;
      right += key == 0x5822 && flags == 1;
    }
    CHECK(downs == 2 && ups == 1 && right == 1);
    CHECK(q.NextKeystroke(t0 + 404ms, key, flags) && key == 0x5822 && flags == 5);
  }
}

// Step 12's cryptography (docs/SYS_STUBS.md: implemented by Xenia, stubbed by the reference) against the standards'
// test vectors: MD5 (RFC 1321), SHA-256 (FIPS 180-4), HMAC-MD5 and HMAC-SHA-1 (RFC 2202), DES (the classic vector and
// FIPS 81's CBC example); one call and streamed in uneven pieces.
void CryptStep12Tests() {
  const uint32_t in = kScratch, out = kScratch + 0x400, state = kScratch + 0x600, key = kScratch + 0x800;
  auto put = [](uint32_t at, const std::string& bytes) { std::memcpy(g.host(at), bytes.data(), bytes.size()); };
  auto hex = [](uint32_t at, uint32_t size) {
    std::string text;
    for (uint32_t i = 0; i < size; ++i) text += "0123456789abcdef"[g.Load8(at + i) >> 4], text += "0123456789abcdef"[g.Load8(at + i) & 15];
    return text;
  };
  // MD5, one call: the three buffers as one message; null and empty buffers skipped; the output cut to its size.
  XeCryptMd5(g, 0, 0, 0, 0, 0, 0, out, 16);
  CHECK(hex(out, 16) == "d41d8cd98f00b204e9800998ecf8427e");
  put(in, "abc");
  XeCryptMd5(g, in, 1, in + 1, 1, in + 2, 1, out, 16);
  CHECK(hex(out, 16) == "900150983cd24fb0d6963f7d28e17f72");
  std::memset(g.host(out), 0xEE, 16);
  XeCryptMd5(g, in, 3, 0, 5, in, 0, out, 4);
  CHECK(hex(out, 5) == "90015098ee");
  // MD5 streamed across block boundaries: 80 digits in pieces of 1, 63 and 16; the digest left in the state's words.
  const std::string digits = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
  put(in, digits);
  XeCryptMd5Init(g, state);
  CHECK(g.Load32(state + 4) == 0x67452301 && g.Load32(state + 0x10) == 0x10325476 && g.Load32(state) == 0);
  XeCryptMd5Update(g, state, in, 1);
  XeCryptMd5Update(g, state, in + 1, 63);
  XeCryptMd5Update(g, state, in + 64, 16);
  CHECK(g.Load32(state) == 80);
  XeCryptMd5Final(g, state, out, 16);
  CHECK(hex(out, 16) == "57edf4a22be3c955ac49da2e2107b67a" && hex(state + 4, 16) == "57edf4a22be3c955ac49da2e2107b67a");
  // SHA-256, one call.
  put(in, "abc");
  XeCryptSha256(g, in, 3, 0, 0, 0, 0, out, 32);
  CHECK(hex(out, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  put(in, long_message);
  XeCryptSha256(g, in, 20, in + 20, 30, in + 50, uint32_t(long_message.size()) - 50, out, 32);
  CHECK(hex(out, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // HMAC-MD5 (RFC 2202 cases 1 and 2).
  put(key, std::string(16, '\x0b'));
  put(in, "Hi There");
  XeCryptHmacMd5(g, key, 16, in, 8, 0, 0, 0, 0, out, 16);
  CHECK(hex(out, 16) == "9294727a3638bb1c13f48ef8158bfc9d");
  put(key, "Jefe");
  put(in, "what do ya want for nothing?");
  XeCryptHmacMd5(g, key, 4, in, 10, in + 10, 18, 0, 0, out, 16);
  CHECK(hex(out, 16) == "750c783e6ab0b503eaa86e310a5db738");
  // HMAC-SHA-1 streamed (RFC 2202 cases 1 and 2), and the one-call verify.
  put(key, std::string(20, '\x0b'));
  put(in, "Hi There");
  XeCryptHmacShaInit(g, state, key, 20);
  CHECK(g.Load32(state) == 64 && g.Load32(state + 0x58) == 64);
  XeCryptHmacShaUpdate(g, state, in, 3);
  XeCryptHmacShaUpdate(g, state, in + 3, 5);
  XeCryptHmacShaFinal(g, state, out, 20);
  CHECK(hex(out, 20) == "b617318655057264e28bc0b6fb378c8ef146be00");
  put(key, "Jefe");
  put(in, "what do ya want for nothing?");
  XeCryptHmacShaInit(g, state, key, 4);
  XeCryptHmacShaUpdate(g, state, in, 28);
  XeCryptHmacShaFinal(g, state, out, 20);
  CHECK(hex(out, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
  CHECK(XeCryptHmacShaVerify(g, key, 4, in, 28, 0, 0, 0, 0, out, 20) == 1);
  CHECK(XeCryptHmacShaVerify(g, key, 4, in, 28, 0, 0, 0, 0, out, 8) == 1);
  g.Store8(out + 19, g.Load8(out + 19) ^ 1);
  CHECK(XeCryptHmacShaVerify(g, key, 4, in, 28, 0, 0, 0, 0, out, 20) == 0);
  CHECK(XeCryptHmacShaVerify(g, key, 4, in, 28, 0, 0, 0, 0, out, 19) == 1);
  // Single DES: the classic vector both ways, then FIPS 81's CBC example (the feed left on the last block).
  g.Store64(key, 0x133457799BBCDFF1ull);
  XeCryptDesKey(g, state, key);
  g.Store64(in, 0x0123456789ABCDEFull);
  XeCryptDesEcb(g, state, in, out, 1);
  CHECK(g.Load64(out) == 0x85E813540F0AB405ull);
  XeCryptDesEcb(g, state, out, out + 8, 0);
  CHECK(g.Load64(out + 8) == 0x0123456789ABCDEFull);
  g.Store64(key, 0x0123456789ABCDEFull);
  XeCryptDesKey(g, state, key);
  const uint32_t feed = key + 8;
  g.Store64(feed, 0x1234567890ABCDEFull);
  put(in, "Now is the time for all ");
  XeCryptDesCbc(g, state, in, 24, out, feed, 1);
  CHECK(hex(out, 24) == "e5c7cdde872bf27c43e934008c389c0f683788499a7c05f6");
  CHECK(g.Load64(feed) == 0x683788499A7C05F6ull);
  g.Store64(feed, 0x1234567890ABCDEFull);
  XeCryptDesCbc(g, state, out, 24, out + 0x40, feed, 0);
  CHECK(std::memcmp(g.host(out + 0x40), "Now is the time for all ", 24) == 0);
}

// --- the hashes on every SHA path (crypt.h: ShaPath), DES against a bitwise model ------------------------------------

// FIPS 46-3's DES bit by bit, as its tables give it (crypt.cpp's DES folds the S-boxes and P into tables and permutes
// with swaps): the reference model the table-driven one is checked against.
constexpr uint8_t kRefIp[] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};
constexpr uint8_t kRefFp[] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25,
};
constexpr uint8_t kRefE[] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};
constexpr uint8_t kRefP[] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25,
};
constexpr uint8_t kRefS[] = {
    14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
    0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
    4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
    15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13,
    15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
    3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
    0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
    13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9,
    10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
    13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
    13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
    1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12,
    7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
    13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
    10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
    3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14,
    2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
    14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
    4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
    11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3,
    12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
    10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
    9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
    4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13,
    4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
    13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
    1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
    6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12,
    13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
    1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
    7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
    2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11,
};

uint64_t RefPermute(uint64_t value, int width, const uint8_t* table, int count) {
  uint64_t result = 0;
  for (int i = 0; i < count; ++i) result = result << 1 | ((value >> (width - table[i])) & 1);
  return result;
}

uint64_t RefDes(uint64_t block, const uint64_t sub_keys[16], bool decrypt) {
  block = RefPermute(block, 64, kRefIp, 64);
  uint32_t l = uint32_t(block >> 32), r = uint32_t(block);
  for (int i = 0; i < 16; ++i) {
    const uint64_t s_input = RefPermute(r, 32, kRefE, 48) ^ sub_keys[decrypt ? 15 - i : i];
    uint32_t s_output = 0;
    for (int b = 0; b < 8; ++b) {
      const uint32_t six = uint32_t(s_input >> (42 - 6 * b)) & 0x3F;
      s_output = s_output << 4 | kRefS[64 * b + 16 * ((six >> 4 & 2) | (six & 1)) + (six >> 1 & 0xF)];
    }
    const uint32_t t = r;
    r = l ^ uint32_t(RefPermute(s_output, 32, kRefP, 32)), l = t;
  }
  return RefPermute(uint64_t(r) << 32 | l, 64, kRefFp, 64);
}

void CryptPathTests() {
  const uint32_t in = kScratch, out = kScratch + 0x400, state = kScratch + 0x600, key = kScratch + 0x800;
  const ShaPath restore = CurrentShaPath();
  std::vector<ShaPath> paths;
  for (const ShaPath path : {ShaPath::kPortable, ShaPath::kProcessor, ShaPath::kHost}) {
    if (ShaPathAvailable(path)) paths.push_back(path);
  }
  std::mt19937 rng(12);
  auto hex = [](uint32_t at, uint32_t size) {
    std::string text;
    for (uint32_t i = 0; i < size; ++i) text += "0123456789abcdef"[g.Load8(at + i) >> 4], text += "0123456789abcdef"[g.Load8(at + i) & 15];
    return text;
  };
  auto put = [](uint32_t at, const std::string& bytes) { std::memcpy(g.host(at), bytes.data(), bytes.size()); };
  // Every standard vector on every path the machine has (portable, the processor's SHA-NI, the host's library).
  for (const ShaPath path : paths) {
    SetShaPath(path);
    CHECK(CurrentShaPath() == path);
    CryptStep12Tests();
    // SHA-1 (FIPS 180-4) and MD5 (RFC 1321's suite: lengths 0 to 80, both padding cases).
    put(in, "abc");
    XeCryptSha(g, in, 3, 0, 0, 0, 0, out, 20);
    CHECK(hex(out, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    put(in, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    XeCryptSha(g, in, 56, 0, 0, 0, 0, out, 20);
    CHECK(hex(out, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    const std::pair<const char*, const char*> md5[] = {
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
    };
    for (const auto& [message, digest] : md5) {
      put(in, message);
      XeCryptMd5(g, in, uint32_t(std::strlen(message)), 0, 0, 0, 0, out, 16);
      CHECK(hex(out, 16) == digest);
    }
  }
  // Every other path against the portable one: lengths 0 to 200 (the host's from 64) split at random places, one call
  // and streamed (the state's bytes compared too), and HMAC-SHA-1 with keys of 0 to 100 bytes (hashed past 64).
  for (uint32_t length = 0; length <= 200; ++length) {
    for (uint32_t i = 0; i < length; ++i) g.Store8(in + i, uint8_t(rng()));
    for (uint32_t i = 0; i < 100; ++i) g.Store8(key + i, uint8_t(rng()));
    const uint32_t a = rng() % (length + 1), b = a + rng() % (length - a + 1), key_size = length % 101;
    uint8_t results[3][72];
    uint8_t states[3][0x80];
    for (size_t p = 0; p < paths.size(); ++p) {
      SetShaPath(paths[p]);
      XeCryptSha(g, in, a, in + a, b - a, in + b, length - b, out, 20);
      XeCryptSha256(g, in, a, in + a, b - a, in + b, length - b, out + 20, 32);
      XeCryptHmacSha(g, key, key_size, in, a, in + a, b - a, in + b, length - b, out + 52, 20);
      CHECK(XeCryptHmacShaVerify(g, key, key_size, in, a, in + a, b - a, in + b, length - b, out + 52, 20) == 1);
      std::memcpy(results[p], g.host(out), 72);
      XeCryptShaInit(g, state);
      XeCryptShaUpdate(g, state, in, a);
      XeCryptShaUpdate(g, state, in + a, length - a);
      XeCryptShaFinal(g, state, out, 20);
      CHECK(std::memcmp(g.host(out), results[p], 20) == 0);
      XeCryptSha256Init(g, state + 0x58);
      XeCryptSha256Update(g, state + 0x58, in, b);
      XeCryptSha256Update(g, state + 0x58, in + b, length - b);
      std::memcpy(states[p], g.host(state), 0x80);
      XeCryptSha256Final(g, state + 0x58, out, 32);
      CHECK(std::memcmp(g.host(out), results[p] + 20, 32) == 0);
    }
    for (size_t p = 1; p < paths.size(); ++p) {
      CHECK(std::memcmp(results[0], results[p], 72) == 0);
      CHECK(std::memcmp(states[0], states[p], 0x80) == 0);
    }
  }
  SetShaPath(restore);
  // DES against the bitwise model: random keys and blocks, both ways, single and triple (the triple's schedules are
  // three single ones, as XeCryptDes3Key lays them out).
  for (int round = 0; round < 200; ++round) {
    const uint64_t k = uint64_t(rng()) << 32 | rng(), block = uint64_t(rng()) << 32 | rng();
    g.Store64(key, k);
    XeCryptDesKey(g, state, key);
    uint64_t sub_keys[16];
    std::memcpy(sub_keys, g.host(state), sizeof sub_keys);
    g.Store64(in, block);
    XeCryptDesEcb(g, state, in, out, 1);
    CHECK(g.Load64(out) == RefDes(block, sub_keys, false));
    XeCryptDesEcb(g, state, in, out, 0);
    CHECK(g.Load64(out) == RefDes(block, sub_keys, true));
  }
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 3; ++i) g.Store64(key + 8 * i, uint64_t(rng()) << 32 | rng());
    XeCryptDes3Key(g, state, key);
    uint64_t schedules[3][16];
    std::memcpy(schedules, g.host(state), sizeof schedules);
    const uint64_t block = uint64_t(rng()) << 32 | rng();
    g.Store64(in, block);
    XeCryptDes3Ecb(g, state, in, out, 1);
    CHECK(g.Load64(out) == RefDes(RefDes(RefDes(block, schedules[0], false), schedules[1], true), schedules[2], false));
    XeCryptDes3Ecb(g, state, out, out + 8, 0);
    CHECK(g.Load64(out + 8) == block);
  }
  // CBC both ways against the model over 1 to 9 blocks (decryption runs two blocks at a time, then the odd one), single
  // and triple; the feed left on the last ciphertext block.
  for (int round = 0; round < 60; ++round) {
    const bool triple = round % 2;
    const uint32_t blocks = 1 + rng() % 9;
    for (int i = 0; i < 3; ++i) g.Store64(key + 8 * i, uint64_t(rng()) << 32 | rng());
    triple ? XeCryptDes3Key(g, state, key) : XeCryptDesKey(g, state, key);
    uint64_t schedules[3][16];
    std::memcpy(schedules, g.host(state), triple ? sizeof schedules : sizeof schedules[0]);
    auto model = [&](uint64_t b, bool decrypt) {
      if (!triple) return RefDes(b, schedules[0], decrypt);
      return decrypt ? RefDes(RefDes(RefDes(b, schedules[2], true), schedules[1], false), schedules[0], true)
                     : RefDes(RefDes(RefDes(b, schedules[0], false), schedules[1], true), schedules[2], false);
    };
    const uint64_t iv = uint64_t(rng()) << 32 | rng();
    uint64_t chain = iv;
    std::vector<uint64_t> plain(blocks), cipher(blocks);
    for (uint32_t i = 0; i < blocks; ++i) {
      plain[i] = uint64_t(rng()) << 32 | rng();
      g.Store64(in + 8 * i, plain[i]);
      cipher[i] = chain = model(plain[i] ^ chain, false);
    }
    const uint32_t feed = key + 0x40;
    g.Store64(feed, iv);
    triple ? XeCryptDes3Cbc(g, state, in, 8 * blocks, out, feed, 1) : XeCryptDesCbc(g, state, in, 8 * blocks, out, feed, 1);
    bool same = g.Load64(feed) == cipher[blocks - 1];
    for (uint32_t i = 0; i < blocks; ++i) same = same && g.Load64(out + 8 * i) == cipher[i];
    CHECK(same);
    g.Store64(feed, iv);
    triple ? XeCryptDes3Cbc(g, state, out, 8 * blocks, out + 0x100, feed, 0)
           : XeCryptDesCbc(g, state, out, 8 * blocks, out + 0x100, feed, 0);
    same = g.Load64(feed) == cipher[blocks - 1];
    for (uint32_t i = 0; i < blocks; ++i) same = same && g.Load64(out + 0x100 + 8 * i) == plain[i];
    CHECK(same);
  }
  // HMACs under alternating keys (each thread keeps its last key's pad states): A, B, A, a longer B, A again give
  // each key's own digest every time.
  {
    std::string first_a, first_b;
    for (int pass = 0; pass < 5; ++pass) {
      const bool b = pass % 2;
      const uint32_t key_size = b ? (pass == 3 ? 40u : 20u) : 16u;
      for (uint32_t i = 0; i < 40; ++i) g.Store8(key + i, uint8_t(b ? 0xB0 + i : 0xA0 + i));
      XeCryptHmacMd5(g, key, key_size, in, 100, 0, 0, 0, 0, out, 16);
      XeCryptHmacSha(g, key, key_size, in, 100, 0, 0, 0, 0, out + 16, 20);
      XeCryptHmacShaInit(g, state, key, key_size);
      XeCryptHmacShaUpdate(g, state, in, 100);
      XeCryptHmacShaFinal(g, state, out + 36, 20);
      const std::string digest(reinterpret_cast<const char*>(g.host(out)), 56);
      CHECK(std::memcmp(g.host(out + 16), g.host(out + 36), 20) == 0);
      if (pass == 0) first_a = digest;
      if (pass == 1) first_b = digest;
      if (pass == 2 || pass == 4) CHECK(digest == first_a);
      if (pass == 3) CHECK(digest != first_b);
    }
  }
}

// Step 12's information calls (info.h, xam.h, win32.h), against Xenia's answers (xam_info.cc, xam_locale.cc,
// title_id_utils.h: its static_asserts are among the cases).
void XamInfoStep12Tests() {
  Fresh();
  XamState().Reset();
  const uint32_t buf = kScratch + 0x400, len = kScratch + 0x3F0, ov = kScratch + 0x380, kthread = kScratch + 0x800;
  auto text = [](uint32_t at) {
    std::u16string out;
    for (uint32_t i = 0; g.Load16(at + 2 * i); ++i) out += char16_t(g.Load16(at + 2 * i));
    return out;
  };
  CHECK(XGetAudioFlags() == 0x10001);
  // The language: the console's when it is one of the 18, else the game region's (every region here: English).
  const ConsoleSettings saved = Console();
  CHECK(XamGetLanguage() == 1);
  Console().language = 17;
  CHECK(XamGetLanguage() == 17);
  Console().language = 18;
  CHECK(XamGetLanguage() == 1);
  // The profile's language and country bytes are the console's; a null out is skipped.
  Console().language = 3, Console().country = 24;
  std::memset(g.host(buf), 0xEE, 2);
  XamGetOnlineLanguageAndCountry(g, buf, buf + 1);
  CHECK(g.Load8(buf) == 3 && g.Load8(buf + 1) == 24);
  XamGetOnlineLanguageAndCountry(g, 0, 0);
  Console() = saved;
  // Country features: listed ones (US 12, Japan 8, Argentina 4, UAE 0), 0x80000000 otherwise.
  CHECK(XamGetOnlineCountryFeatures(103) == 12 && XamGetOnlineCountryFeatures(53) == 8);
  CHECK(XamGetOnlineCountryFeatures(4) == 4 && XamGetOnlineCountryFeatures(1) == 0);
  CHECK(XamGetOnlineCountryFeatures(2) == 0x80000000 && XamGetOnlineCountryFeatures(0) == 0x80000000);
  // Title-id classes (Xenia's static_asserts, and each branch).
  CHECK(XamIsSystemTitleId(kDashboardTitleId) && !XamIsSystemTitleId(0x4D5308BC) && !XamIsSystemTitleId(0x5841089A));
  CHECK(XamIsSystemTitleId(0) && XamIsSystemTitleId(0x584E07D1) && XamIsSystemTitleId(0x5841FFFE) == false);
  CHECK(XamIsSystemTitleId(0xFFFE0000) && !XamIsSystemTitleId(0x58410000 | 0x1234));
  CHECK(XamIsXbox1TitleId(0x41430006) && !XamIsXbox1TitleId(0x4D5308BC) && XamIsXbox1TitleId(kDashboardTitleId));
  CHECK(!XamIsXbox1TitleId(0xFF000001));
  CHECK(XamIsSystemExperienceTitleId(0x58480001) && XamIsSystemExperienceTitleId(0x584A0001));
  CHECK(XamIsSystemExperienceTitleId(0x584E07D1) && XamIsSystemExperienceTitleId(0x584E07D2));
  CHECK(!XamIsSystemExperienceTitleId(0x584E07D3) && !XamIsSystemExperienceTitleId(0x4D5308BC));
  // Typefaces.
  CHECK(XamGetLanguageTypeface(g, 17, 0x100, buf) == 0 && text(buf) == u"file://media:/XenonSCLatin.xtt");
  CHECK(XamGetLanguageTypeface(g, 8, 0x100, buf) == 0 && text(buf) == u"file://media:/XenonCLatin.xtt");
  CHECK(XamGetLanguageTypeface(g, 1, 0x100, buf) == 0 && text(buf) == u"file://media:/XenonJKLatin.xtt");
  // Locale fallbacks: one guest copy, made once; empty for English, none past the 18 languages.
  const uint32_t japanese = XamState().LanguageLocaleFallbackString(g, 2);
  CHECK(japanese && text(japanese) == u"ja-JP" && XamState().LanguageLocaleFallbackString(g, 2) == japanese);
  CHECK(text(XamState().LanguageLocaleFallbackString(g, 17)) == u"zh-CHS");
  CHECK(text(XamState().LanguageLocaleFallbackString(g, 10)) == u"zh-CHS");
  CHECK(text(XamState().LanguageLocaleFallbackString(g, 16)) == u"nl-NL");
  CHECK(XamState().LanguageLocaleFallbackString(g, 1) && text(XamState().LanguageLocaleFallbackString(g, 1)).empty());
  CHECK(XamState().LanguageLocaleFallbackString(g, 18) == 0);
  // The dashboard's application info: zero at first, the last set, cleared by a null set; a null get writes nothing.
  std::memset(g.host(buf), 0xEE, 12);
  XamState().GetActiveDashAppInfo(g, buf);
  CHECK(std::all_of(g.host(buf), g.host(buf) + 12, [](uint8_t b) { return b == 0; }));
  for (uint32_t i = 0; i < 12; ++i) g.Store8(buf + i, uint8_t(i + 1));
  XamState().SetActiveDashAppInfo(g, buf);
  std::memset(g.host(buf + 0x20), 0, 12);
  XamState().GetActiveDashAppInfo(g, buf + 0x20);
  CHECK(std::memcmp(g.host(buf), g.host(buf + 0x20), 12) == 0);
  XamState().GetActiveDashAppInfo(g, 0);
  XamState().SetActiveDashAppInfo(g, 0);
  XamState().GetActiveDashAppInfo(g, buf + 0x20);
  CHECK(std::all_of(g.host(buf + 0x20), g.host(buf + 0x20) + 12, [](uint8_t b) { return b == 0; }));
  CHECK(XamState().DashContext() == 0 && XamState().GetDashBackstackData() == 0);
  // The cached title name: the title's for any id, length with the terminator; id 0 refused with an empty string.
  XamState().title().title_name = u"Any360";
  std::memset(g.host(buf), 0xEE, 32);
  CHECK(XamState().GetCachedTitleName(g, 0x4D5308BC, buf, len) == 0 && text(buf) == u"Any360" && g.Load32(len) == 7);
  CHECK(XamState().GetCachedTitleName(g, 0x12345678, buf, len) == 0 && text(buf) == u"Any360");
  CHECK(XamState().GetCachedTitleName(g, 0, buf, len) == kErrorInvalidParameter && g.Load16(buf) == 0 && g.Load32(len) == 1);
  // The overlapped extended error; the thread's last error without one.
  std::memset(g.host(kthread), 0, 0x200);
  SetWin32LastError(g, kthread, 0x57);
  CHECK(XGetOverlappedExtendedError(g, kthread, 0) == 0x57);
  g.Store32(ov, kErrorIoPending);
  g.Store32(ov + 0x18, 0x1234);
  CHECK(XGetOverlappedExtendedError(g, kthread, ov) == 0x3E4);
  g.Store32(ov, 0);
  CHECK(XGetOverlappedExtendedError(g, kthread, ov) == 0x1234);
  XamState().Reset();
}

// Step 12's kernel and XAM calls (rtl.h, scheduler.h, variables.h, info.h), against Xenia's answers.
void KernelStep12Tests() {
  Fresh();
  const uint32_t out = kScratch + 0x400, image = kScratch + 0x1000;
  // RtlDowncaseUnicodeChar undoes RtlUpcaseUnicodeChar wherever that one changes a character; others are themselves.
  for (uint32_t c = 0; c < 0x10000; ++c) {
    const uint32_t upper = RtlUpcaseUnicodeChar(c);
    if (upper != c) CHECK(RtlDowncaseUnicodeChar(upper) == c);
  }
  CHECK(RtlDowncaseUnicodeChar('A') == 'a' && RtlDowncaseUnicodeChar(0xC9) == 0xE9 && RtlDowncaseUnicodeChar(0xD7) == 0xD7);
  CHECK(RtlDowncaseUnicodeChar(0x0410) == 0x0410 && RtlDowncaseUnicodeChar(0x10041) == 'a');
  // RtlRandom: Xenia's sequence from seed 12345 on a fresh table (results and seeds, modelled step by step).
  g.Store32(out, 12345);
  const std::pair<uint32_t, uint32_t> expected[] = {{1960408961, 4457625}, {776561741, 1609203705}, {527237240, 1101953625},
                                                    {839522115, 520784843}};
  for (const auto& [result, seed] : expected) CHECK(RtlRandom(g, out) == result && g.Load32(out) == seed);
  // RtlGetStackLimits: the PCR's stack, or its alternate one when the flag is set.
  const uint32_t pcr = kScratch + 0x800;
  std::memset(g.host(pcr), 0, 0x100);
  g.Store32(pcr + 0x70, 0x70010000), g.Store32(pcr + 0x74, 0x70000000);
  g.Store32(pcr + 0x78, 0x71010000), g.Store32(pcr + 0x7C, 0x71000000);
  RtlGetStackLimits(g, pcr, out, out + 4);
  CHECK(g.Load32(out) == 0x70000000 && g.Load32(out + 4) == 0x70010000);
  g.Store32(pcr + 0x6C, 1);
  RtlGetStackLimits(g, pcr, out, out + 4);
  CHECK(g.Load32(out) == 0x71000000 && g.Load32(out + 4) == 0x71010000);
  // RtlImageDirectoryEntryToData on a small PE32 image (little-endian fields): 4 directory entries, two sections.
  std::memset(g.host(image), 0, 0x400);
  auto le32 = [](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) g.Store8(at + i, uint8_t(v >> (8 * i))); };
  auto le16 = [](uint32_t at, uint32_t v) { g.Store8(at, uint8_t(v)), g.Store8(at + 1, uint8_t(v >> 8)); };
  g.Store8(image, 'M'), g.Store8(image + 1, 'Z');
  le32(image + 0x3C, 0x80);
  const uint32_t nt = image + 0x80, optional = nt + 24;
  g.Store8(nt, 'P'), g.Store8(nt + 1, 'E');
  le16(nt + 6, 2);       // sections
  le16(nt + 20, 0x80);   // SizeOfOptionalHeader: 96 + 4 entries of 8
  le16(optional, 0x10B);
  le32(optional + 60, 0x200);  // SizeOfHeaders
  le32(optional + 92, 4);      // NumberOfRvaAndSizes
  le32(optional + 96 + 8 * 1, 0x100), le32(optional + 100 + 8 * 1, 0x20);    // entry 1 inside the headers
  le32(optional + 96 + 8 * 2, 0x3010), le32(optional + 100 + 8 * 2, 0x40);   // entry 2 in the second section
  le32(optional + 96 + 8 * 3, 0x9000), le32(optional + 100 + 8 * 3, 0x10);   // entry 3 in no section
  const uint32_t section0 = optional + 0x80;
  le32(section0 + 12, 0x1000), le32(section0 + 16, 0x1000);
  le32(section0 + 40 + 12, 0x3000), le32(section0 + 40 + 16, 0x800);
  g.Store32(out, 0xEEEEEEEE);
  CHECK(RtlImageDirectoryEntryToData(g, image, 1, 2, out) == image + 0x3010 && g.Load32(out) == 0x40);
  CHECK(RtlImageDirectoryEntryToData(g, image, 0, 2, out) == image + 0x10);       // its offset in the section
  CHECK(RtlImageDirectoryEntryToData(g, image | 1, 1, 2, out) == image + 0x10);   // an odd base: a file
  CHECK(RtlImageDirectoryEntryToData(g, image, 0x100, 2, out) == image + 0x10);   // the flag is one byte
  CHECK(RtlImageDirectoryEntryToData(g, image, 0, 1, out) == image + 0x100 && g.Load32(out) == 0x20);
  CHECK(RtlImageDirectoryEntryToData(g, image, 0, 3, out) == 0 && g.Load32(out) == 0x10);
  CHECK(RtlImageDirectoryEntryToData(g, image, 1, 0, out) == 0);  // empty entry
  CHECK(RtlImageDirectoryEntryToData(g, image, 1, 4, out) == 0);  // past the directory
  le16(optional, 0x20B);
  CHECK(RtlImageDirectoryEntryToData(g, image, 1, 2, out) == 0);  // not PE32
  CHECK(RtlImageDirectoryEntryToData(g, kScratch + 0x2000, 1, 2, out) == 0);
  // KeQueryInterruptTime: 100-ns units that move with the clock.
  const uint64_t t0 = InterruptTime();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const uint64_t t1 = InterruptTime();
  CHECK(t1 > t0 && t1 - t0 >= 150000 && t1 - t0 < 50000000);
  // KeSetPriorityThread (a thread the scheduler runs: SchedulerTests): null is 0; a KTHREAD the scheduler does not
  // run is read, not written.
  CHECK(Threads().SetPriority(g, 0, 5) == 0);
  g.Store8(out + 0x70, 9);
  CHECK(Threads().SetPriority(g, out, 5) == 9 && g.Load8(out + 0x70) == 9);
  // XamGetLocaleDateFormat: the table's ends and outside it; the AV pack is XGetAVPack's.
  CHECK(XamGetLocaleDateFormat(5) == 2 && XamGetLocaleDateFormat(6) == 1 && XamGetLocaleDateFormat(109) == 5);
  CHECK(XamGetLocaleDateFormat(37) == 0 && XamGetLocaleDateFormat(110) == 3 && XamGetLocaleDateFormat(4) == 3);
  CHECK(XamGetLocaleDateFormat(0) == 3 && XGetAVPack() == 6);
}

// Step 12's guide screens and Kinect calls (xam.h), against Xenia's answers and any360::sys's guide.
void GuideNuiStep12Tests() {
  Fresh();
  XamState().Reset();
  const uint32_t out = kScratch + 0x400;
  // A system listener sees the guide open and close (XN_SYS_UI 1 then 0) for each screen that opens.
  const uint32_t listener = XamState().NotifyCreateListener(g, 1, 0);
  auto events = [&] {
    std::vector<std::pair<uint32_t, uint32_t>> seen;
    while (XamState().NotifyGetNext(g, listener, 0, out, out + 4)) seen.emplace_back(g.Load32(out), g.Load32(out + 4));
    return seen;
  };
  events();
  const std::vector<std::pair<uint32_t, uint32_t>> guide = {{9, 1}, {9, 0}};
  CHECK(XamState().ShowCreateProfileUI() == 0 && events() == guide);
  CHECK(XamState().ShowProfileUI(0) == 0 && events() == guide);
  CHECK(XamState().ShowProfileUI(1) == kErrorAccessDenied && events().empty());
  CHECK(XamState().ShowForcedNameChangeUI(0) == 0 && events() == guide);
  CHECK(XamState().ShowForcedNameChangeUI(2) == kErrorNoSuchUser && XamState().ShowForcedNameChangeUI(4) == kErrorInvalidParameter);
  CHECK(XamState().ShowMultiplayerUpgradeUI() == kErrorFunctionFailed && events().empty());
  CHECK(XamState().ShowNuiHardwareRequiredUI(0) == 0 && XamState().ShowNuiHardwareRequiredUI(1) == kErrorInvalidParameter);
  // The dashboard context: notified once per change.
  CHECK(XamState().SetDashContext(7) == 0 && XamState().DashContext() == 7);
  const std::vector<std::pair<uint32_t, uint32_t>> changed = {{0x8000000C, 0}};
  CHECK(events() == changed);
  CHECK(XamState().SetDashContext(7) == 0 && events().empty());
  // Kinect: setting an engaged player fails, clearing succeeds; versions 0; a stable non-zero session id.
  CHECK(XamState().NuiHudSetEngagedTrackingId(5) == 0x80004005u && XamState().NuiHudSetEngagedTrackingId(0) == 0);
  std::memset(g.host(out), 0xEE, 16);
  XamState().NuiHudGetVersions(g, out, out + 8);
  CHECK(g.Load64(out) == 0 && g.Load64(out + 8) == 0);
  XamState().NuiHudGetVersions(g, 0, 0);
  const uint64_t session = XamState().NuiIdentitySessionId();
  CHECK(session != 0 && XamState().NuiIdentitySessionId() == session);
  Objects().Close(listener);
  XamState().Reset();
}

// Step 12's profile and user calls (xam.h), against Xenia's answers with one local profile.
void ProfileStep12Tests() {
  namespace fs = std::filesystem;
  Fresh();
  XamState().Reset();
  Files().Reset();
  const fs::path content = fs::temp_directory_path() / "any360-sys-profile";
  std::error_code ec;
  fs::remove_all(content, ec);
  XamState().title().content_root = content;
  XamState().title().gamertag = "Any360";
  const uint64_t xuid = XamState().title().xuid;
  const ConsoleSettings saved = Console();
  Console().language = 3, Console().country = 24;  // German, Germany
  const uint32_t out = kScratch + 0x400, account = kScratch + 0x800, ov = kScratch + 0x380;
  const uint32_t flags = 24u << 8 | 3u << 25;
  // User flags: the console's country and language, for the local profile only.
  CHECK(XamState().UserFlags(0) == flags && XamState().UserFlags(1) == 0);
  CHECK(XamState().UserFlagsFromXuid(xuid) == flags && XamState().UserFlagsFromXuid(xuid + 1) == 0);
  // The account: cleared then filled; another XUID is no such user (the account left cleared).
  std::memset(g.host(account), 0xEE, Xam::kAccountSize);
  g.Store32(out, 0);
  CHECK(XamState().ProfileFindAccount(g, xuid, account, out) == 0 && g.Load32(out) == 1);
  CHECK(g.Load16(account + 8) == 'A' && g.Load16(account + 18) == '0' && g.Load16(account + 20) == 0);
  CHECK(g.Load32(account) == 0 && g.Load64(account + 0x28) == 0 && g.Load32(account + 0x30) == flags);
  CHECK(g.Load8(account + Xam::kAccountSize - 1) == 0);
  std::memset(g.host(account), 0xEE, Xam::kAccountSize);
  CHECK(XamState().ProfileFindAccount(g, xuid + 1, account, out) == kErrorNoSuchUser && g.Load32(account + 0x30) == 0);
  CHECK(XamState().ProfileFindAccount(g, xuid, 0, out) == kErrorInvalidParameter);
  // The profile enumerator: one X_PROFILEENUMRESULT (XUID, account, device 1), then no more.
  CHECK(XamState().ProfileCreateEnumerator(g, 0) == kErrorInvalidParameter);
  CHECK(XamState().ProfileCreateEnumerator(g, out) == 0);
  const uint32_t handle = g.Load32(out);
  std::memset(g.host(account), 0xEE, Xam::kProfileEnumResultSize);
  CHECK(XamState().ProfileEnumerate(g, handle, account, 0) == 0);
  CHECK(g.Load64(account) == xuid && g.Load16(account + 16) == 'A' && g.Load32(account + 8 + 0x30) == flags);
  CHECK(g.Load32(account + Xam::kProfileEnumResultSize - 4) == 1);
  CHECK(XamState().ProfileEnumerate(g, handle, account, 0) == kErrorNoMoreFiles);
  Objects().Close(handle);
  // Opening the profile mounts its package under the content root and fills the content data.
  std::memcpy(g.host(out), "PROFILE", 8);
  std::memset(g.host(account), 0, 0x148);
  CHECK(XamState().ProfileOpen(g, xuid, out, account) == 0);
  CHECK(g.Load32(account) == 1 && g.Load32(account + 4) == 0x10000 && g.Load32(account + 0x140) == 0xFFFE07D1);
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llX", static_cast<unsigned long long>(xuid));
  CHECK(std::memcmp(g.host(account + 0x108), hex, 16) == 0 && g.Load8(account + 0x108 + 16) == 0);
  CHECK(fs::is_directory(content / "profile" / hex));
  CHECK(XamState().ProfileOpen(g, xuid + 1, out, account) == kErrorInvalidParameter);
  // Index from XUID, subscription, tenure, the Kinect sign-in index, logon.
  CHECK(XamState().UserGetIndexFromXuid(g, xuid, out) == 0 && g.Load32(out) == 0);
  CHECK(XamState().UserGetIndexFromXuid(g, xuid + 1, out) == 0x80070525u && XamState().UserGetIndexFromXuid(g, xuid, 0) == 0x80070057u);
  g.Store32(out, 7), g.Store32(out + 4, 7);
  CHECK(XamState().UserGetSubscriptionType(g, 0, out, out + 4) == 0 && g.Load32(out) == 0 && g.Load32(out + 4) == 0);
  CHECK(XamState().UserGetSubscriptionType(g, 1, out, out + 4) == 0x80070525u);
  CHECK(XamState().UserGetSubscriptionType(g, 4, out, out + 4) == 0x80070057u && XamState().UserGetSubscriptionType(g, 0, 0, out) == 0x80070057u);
  CHECK(XamState().UserGetUserTenure(0) == 0 && XamState().UserGetUserTenure(2) == 0x80070525u);
  g.Store32(out, 9);
  CHECK(XamState().UserNuiGetUserIndexForSignin(g, out) == 0 && g.Load32(out) == 0 && XamState().UserNuiGetUserIndexForSignin(g, 0) == 0x80070057u);
  CHECK(XamState().UserLogon(g, 0, 0) == kErrorInvalidParameter && XamState().UserLogon(g, out, 0) == 0);
  std::memset(g.host(ov), 0, 0x1C);
  CHECK(XamState().UserLogon(g, out, ov) == kErrorIoPending && g.Load32(ov) == 0);
  CHECK(XamState().UserGetSigninState(0) == 1);
  // Preferences: a read before any write is S_FALSE for a mask with a written bit; writes keep to their mask, set the
  // written bit and notify; bad masks and values fail with E_INVALIDARG as the extended error.
  const uint32_t listener = XamState().NotifyCreateListener(g, 1, 0);
  while (XamState().NotifyGetNext(g, listener, 0, out + 8, out + 12)) {}
  g.Store32(out, 0xEE);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x006, out, 0) == 0 && g.Load32(out) == 0);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x006, out, ov) == kErrorIoPending && g.Load32(ov) == 0 && g.Load32(ov + 0x18) == 1);
  CHECK(XamState().UserWriteUserPreference(g, 0, 0x006, 0x002, 0) == 0);
  CHECK(XamState().NotifyGetNext(g, listener, 0, out + 8, out + 12) == 1 && g.Load32(out + 8) == 0x0E && g.Load32(out + 12) == 1);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x006, out, ov) == kErrorIoPending && g.Load32(ov + 0x18) == 0 && g.Load32(out) == 0x002);
  CHECK(XamState().UserWriteUserPreference(g, 0, 0x020, 0x020, 0) == 0);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x020, out, 0) == 0 && g.Load32(out) == 0x020);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x006, out, 0) == 0 && g.Load32(out) == 0x002);  // untouched
  CHECK(XamState().UserWriteUserPreference(g, 0, 0x006, 0x004, ov) == kErrorIoPending && g.Load32(ov) == kErrorFunctionFailed &&
        g.Load32(ov + 0x18) == 0x80070057u);  // the written bit itself
  CHECK(XamState().UserWriteUserPreference(g, 0, 0x020, 0x040, 0) == kErrorFunctionFailed);  // outside the mask
  CHECK(XamState().UserWriteUserPreference(g, 0, 0x003, 0x001, 0) == kErrorFunctionFailed);  // no such preference
  CHECK(XamState().UserReadUserPreference(g, 0, 0x003, out, 0) == kErrorFunctionFailed);
  CHECK(XamState().UserReadUserPreference(g, 1, 0x020, out, ov) == kErrorIoPending && g.Load32(ov) == kErrorFunctionFailed &&
        g.Load32(ov + 0x18) == 0x80070525u);
  CHECK(XamState().UserReadUserPreference(g, 4, 0x020, out, 0) == kErrorInvalidParameter);
  CHECK(XamState().UserReadUserPreference(g, 0, 0x020, 0, 0) == kErrorInvalidParameter);
  CHECK(XamState().UserWriteUserPreference(g, 4, 0x020, 0x020, 0) == kErrorInvalidParameter);
  Objects().Close(listener);
  Console() = saved;
  XamState().Reset();
  Files().Reset();
  fs::remove_all(content, ec);
}

// Step 12's Win32 forms (win32.h): what is Win32's own over the kernel calls they use (the last error in the KTHREAD,
// millisecond timeouts, WaitForSingleObjectEx's documented results, SleepEx's, lstrlenW, the tick count).
// Step 12, eighth batch: the language and locale, XamAllocEx, the Xapi timeout and thread flags, IPTV's name (Xenia's
// xam_locale.cc and xam_info.cc). LDI has its own executable (sys_ldi_tests).
void MiscStep12EighthTests() {
  // XTLGetLanguageV2: the console's language up to Russian (12); past it the region's, English outside NTSC-J.
  const ConsoleSettings saved = Console();
  for (uint32_t language = 0; language <= 20; ++language) {
    Console().language = language;
    CHECK(XTLGetLanguageV2() == (language < 13 ? language : 1));
  }
  Console() = saved;

  // XapipGetLocale: "en-US" as ANSI, truncated to length - 1 and terminated; nothing for a length of 0.
  const uint32_t text = kScratch + 0x400;
  for (const uint32_t length : {0u, 1u, 3u, 5u, 6u, 7u, 64u}) {
    std::memset(g.host(text), 0xAA, 16);
    CHECK(XapipGetLocale(g, length, text) == 0);
    const std::string expected = std::string("en-US").substr(0, length ? length - 1 : 0);
    if (length) {
      CHECK(std::memcmp(g.host(text), expected.data(), expected.size()) == 0 && g.Load8(text + uint32_t(expected.size())) == 0);
      CHECK(g.Load8(text + uint32_t(expected.size()) + 1) == 0xAA);
    } else {
      CHECK(g.Load8(text) == 0xAA);
    }
  }
  // Another language and country (German, Austria: online country 5); an unknown country (17) is E_NOTFOUND with
  // nothing written.
  Console().language = 3;
  Console().country = 5;
  CHECK(XapipGetLocale(g, 16, text) == 0 && std::strcmp(reinterpret_cast<const char*>(g.host(text)), "de-AT") == 0);
  Console().country = 17;
  std::memset(g.host(text), 0xAA, 16);
  CHECK(XapipGetLocale(g, 16, text) == 0x80070490 && g.Load8(text) == 0xAA);
  Console() = saved;

  // XapiFormatTimeOut: the relative interval (big-endian) and its address; INFINITE gives 0 and writes nothing.
  const uint32_t interval = kScratch + 0x500;
  g.Store64(interval, 0x1122334455667788);
  CHECK(XapiFormatTimeOut(g, interval, 0xFFFFFFFF) == 0 && g.Load64(interval) == 0x1122334455667788);
  CHECK(XapiFormatTimeOut(g, interval, 1500) == interval && int64_t(g.Load64(interval)) == -15000000);
  CHECK(XapiFormatTimeOut(g, interval, 0) == interval && g.Load64(interval) == 0);
  CHECK(XapiFormatTimeOut(g, interval, 0xFFFFFFFE) == interval && int64_t(g.Load64(interval)) == -int64_t(10000) * 0xFFFFFFFE);
  // The creation flags: CREATE_SUSPENDED is the kernel's 1, a processor its affinity bit; none for -1 or past 7.
  CHECK(XapiThreadFlags(4, 0xFFFFFFFF) == 1 && XapiThreadFlags(0x10, 0xFFFFFFFF) == 0);
  CHECK(XapiThreadFlags(0, 2) == 1u << 26 && XapiThreadFlags(4, 5) == (1u | 1u << 29) && XapiThreadFlags(0, 7) == 1u << 31);
  CHECK(XapiThreadFlags(0, 8) == 0 && XapiThreadFlags(4, 31) == 1 && XapiThreadFlags(0, 0x80000000) == 0);
  // A thread created with a processor runs on it (suspended here: nothing runs).
  {
    Fresh();
    Objects().SetKernelHeap(0x10000, 0x70000);
    Objects().Reset();
    Threads().SetDispatcher(&Dispatch, nullptr);
    for (const uint32_t processor : {0u, 3u, 5u}) {
      uint32_t handle = 0;
      CHECK(Threads().CreateThreadHandle(g, &handle, 0, 0, 0, 0x1000, 0, XapiThreadFlags(4, processor)) == 0);
      const uint32_t kthread = Objects().Lookup(handle);
      CHECK(kthread && g.Load8(kthread + 0xBF) == processor && g.Load8(kthread + 0xBC) == 1);
      // Resumed, it runs (no code at its start: it returns at once) and ends.
      const int64_t second = -10000000;
      CHECK(Threads().NtResumeThread(g, handle, kScratch + 0x580) == 0);
      CHECK(Objects().WaitForHandle(g, handle, kMainThread, &second) == 0);
    }
    Threads().Shutdown();
  }

  // XamIptvGetServiceName: one name buffer (0x78 bytes, empty: IPTV is off), the same address every call, no host
  // allocation.
  Fresh();
  XamState().Reset();
  const uint32_t out = kScratch + 0x600;
  CHECK(XamState().IptvServiceName(g, out) == 0);
  const uint32_t name = g.Load32(out);
  CHECK(name != 0);
  bool zero = true;
  for (uint32_t i = 0; i < 0x78; ++i) zero = zero && g.Load8(name + i) == 0;
  CHECK(zero);
  g.Store32(out, 0);
  const AllocCounts before = ThreadAllocations();
  CHECK(XamState().IptvServiceName(g, out) == 0 && g.Load32(out) == name);
  CHECK((ThreadAllocations() - before).count == 0);

  // XamAllocEx: without bit 30 it is XamAlloc; with it, zeroed physical pages, the protection and alignment from the
  // physical flags; failure writes 0 and gives E_OUTOFMEMORY.
  std::vector<uint8_t> memory(0x400000, 0xAA);
  const Guest gm{memory.data()};
  Memory().Reset({{0x00000000, 0x40000, 0x1000, false}, {0x40000, 0x80000, 0x10000, false},
                  {0x100000, 0x100000, 0x1000, true}, {0x200000, 0x100000, 0x10000, true}});
  const uint32_t io = 0x3F000;
  CHECK(Memory().XamAllocEx(gm, 0, 0, 0x1800, io) == 0);
  const uint32_t plain = gm.Load32(io);
  CHECK(plain && plain < 0x40000 && memory[plain] == 0 && memory[plain + 0x1FFF] == 0);
  struct Case {
    uint32_t physical_flags, protect, alignment;
  };
  const Case cases[] = {
      {0x00000000, kPageReadOnly, 0x1000},
      {0x10000000, kPageReadWrite | kPageNoCache, 0x1000},
      {0x20000000, kPageReadWrite, 0x1000},
      {0x30000000, kPageReadWrite | kPageWriteCombine, 0x1000},
      {0x6E000000, kPageReadWrite, 0x4000},   // read-write, 16 KB alignment, zeroing asked
      {0x2F000000, kPageReadWrite, 0x8000},
      {0x0D000000, kPageReadOnly, 0x2000},
  };
  for (const Case& c : cases) {
    gm.Store32(io, 0xFFFFFFFF);
    CHECK(Memory().XamAllocEx(gm, c.physical_flags, 0x40000000, 0x2345, io) == 0);
    const uint32_t at = gm.Load32(io);
    CHECK(at >= 0x100000 && at < 0x200000 && at % c.alignment == 0);
    CHECK(Memory().QueryProtect(at) == c.protect);
    bool zeroed = true;
    for (uint32_t i = 0; i < 0x3000; ++i) zeroed = zeroed && memory[at + i] == 0;
    CHECK(zeroed);
    std::memset(memory.data() + at, 0x55, 0x3000);
    CHECK(Memory().XamFree(gm, at) == 0);
  }
  gm.Store32(io, 0xFFFFFFFF);
  CHECK(Memory().XamAllocEx(gm, 0x20000000, 0x40000000, 0x200000, io) == 0x8007000E && gm.Load32(io) == 0);
  Memory().Reset();
}

// Step 12, tenth batch: ObCreateObject and XexLoadImageFromMemory (Xenia's xboxkrnl_ob.cc and xboxkrnl_modules.cc).
struct ObAllocations {
  std::vector<std::array<uint32_t, 4>> calls;  // function, size, pool tag, pool
  uint32_t next = 0x20000;
  bool fail = false;
};
uint32_t ObAllocate(uint32_t function, uint32_t size, uint32_t tag, uint32_t pool, void* user) {
  auto& a = *static_cast<ObAllocations*>(user);
  a.calls.push_back({function, size, tag, pool});
  if (a.fail) return 0;
  const uint32_t at = a.next;
  std::memset(g.host(at), 0xCC, size + 8);
  a.next += (size + 0xFF) & ~0xFFu;
  return at;
}

void ObStep12TenthTests() {
  Fresh();
  const uint32_t type = kScratch, attributes = kScratch + 0x40, name = kScratch + 0x60, text = kScratch + 0x80,
                 out = kScratch + 0x100;
  std::memset(g.host(type), 0, 0x20);
  g.Store32(type, 0x82001000);        // allocate procedure
  g.Store32(type + 0x18, 0x54657374);  // pool tag "Test"
  ObAllocations a;
  // Unnamed, no attributes, a title thread: the title pool (1) and the title-object flag; the object after its header.
  g.Store32(out, 0xFFFFFFFF);
  CHECK(ObCreateObject(g, type, 0, 0x22, out, 1, ObAllocate, &a) == 0);
  CHECK(a.calls.size() == 1 && a.calls[0] == (std::array<uint32_t, 4>{0x82001000, 0x32, 0x54657374, 1}));
  CHECK(g.Load32(out) == 0x20010 && g.Load32(0x20000) == 1 && g.Load32(0x20004) == 0 && g.Load32(0x20008) == type);
  CHECK(g.Load16(0x2000C) == 0x10 && g.Load8(0x2000E) == 0xCC);  // the hash index is left as allocated
  // A system thread: the system pool (2), no flag.
  CHECK(ObCreateObject(g, type, 0, 8, out, 2, ObAllocate, &a) == 0 && a.calls.back()[3] == 2 && g.Load16(g.Load32(out) - 4) == 0);
  // The attributes choose: 0x1000 title, 0x2000 system (whatever the thread), neither the thread's.
  std::memset(g.host(attributes), 0, 12);
  for (const auto& [bits, process, pool, flag] : std::initializer_list<std::array<uint32_t, 4>>{
           {0x1000, 2, 1, 0x10}, {0x2000, 1, 2, 0}, {0x3000, 2, 1, 0x10}, {0, 1, 1, 0x10}, {0x40, 2, 2, 0}}) {
    g.Store32(attributes + 8, bits);
    CHECK(ObCreateObject(g, type, attributes, 8, out, process, ObAllocate, &a) == 0);
    CHECK(a.calls.back()[3] == pool && g.Load16(g.Load32(out) - 4) == flag);
  }
  // Named: the name info first, the header, the object (its size rounded to 4), then the last component's copy.
  g.Store32(attributes + 4, name);
  g.Store32(attributes + 8, 0);
  auto set_name = [&](const char* value) {
    g.Store16(name, uint16_t(std::strlen(value)));
    g.Store16(name + 2, uint16_t(std::strlen(value)));
    g.Store32(name + 4, text);
    std::memcpy(g.host(text), value, std::strlen(value));
  };
  set_name("\\BaseNamedObjects\\Title\\Event7");
  a.calls.clear();
  CHECK(ObCreateObject(g, type, attributes, 0x1A, out, 1, ObAllocate, &a) == 0);
  CHECK(a.calls.size() == 1 && a.calls[0][1] == 6 + 0x1C + 0x10 + 0x10 && a.calls[0][3] == 1);
  const uint32_t at = a.next - ((a.calls[0][1] + 0xFF) & ~0xFFu), object = g.Load32(out);
  CHECK(object == at + 0x20 && g.Load32(at) == 0 && g.Load32(at + 4) == 0 && g.Load16(at + 8) == 6 && g.Load16(at + 10) == 6);
  CHECK(g.Load32(at + 12) == object + 0x1C && std::memcmp(g.host(object + 0x1C), "Event7", 6) == 0);
  CHECK(g.Load32(at + 0x10) == 1 && g.Load32(at + 0x14) == 0 && g.Load32(at + 0x18) == type && g.Load16(at + 0x1C) == 0x11);
  set_name("Plain");
  CHECK(ObCreateObject(g, type, attributes, 4, out, 2, ObAllocate, &a) == 0 && a.calls.back()[1] == 5 + 4 + 0x20);
  CHECK(std::memcmp(g.host(g.Load32(out) + 4), "Plain", 5) == 0 && g.Load16(g.Load32(out) - 4) == 1);
  // Invalid names: empty, a lone backslash, a trailing one, a doubled one; nothing allocated, 0 written.
  a.calls.clear();
  for (const char* bad : {"", "\\", "A\\", "A\\\\B", "\\\\A"}) {
    set_name(bad);
    g.Store32(out, 0xFFFFFFFF);
    CHECK(ObCreateObject(g, type, attributes, 4, out, 1, ObAllocate, &a) == 0xC0000033 && g.Load32(out) == 0);
  }
  CHECK(a.calls.empty());
  // A failed allocation: STATUS_INSUFFICIENT_RESOURCES and 0, named or not.
  a.fail = true;
  g.Store32(out, 0xFFFFFFFF);
  CHECK(ObCreateObject(g, type, 0, 4, out, 1, ObAllocate, &a) == 0xC000009A && g.Load32(out) == 0);
  set_name("Named");
  CHECK(ObCreateObject(g, type, attributes, 4, out, 1, ObAllocate, &a) == 0xC000009A && g.Load32(out) == 0);
  // No host allocation once the log is sized.
  a.fail = false;
  a.calls.reserve(64);
  a.calls.clear();
  const AllocCounts before = ThreadAllocations();
  for (int i = 0; i < 20; ++i) {
    a.next = 0x20000;
    ObCreateObject(g, type, attributes, 4, out, 1, ObAllocate, &a);
  }
  CHECK((ThreadAllocations() - before).count == 0);

  // XexLoadImageFromMemory: a loaded module's name collides; any other image is not loaded; no handle pointer.
  XamState().Reset();
  XamState().title().module_name = "game.xex";
  XamState().title().module_handle = 0x40001000;
  const uint32_t module = kScratch + 0x200, handle = kScratch + 0x280;
  for (const char* loaded : {"game.xex", "GAME:\\game.xex", "xam.xex", "xboxkrnl.exe", "default.xex"}) {
    std::memcpy(g.host(module), loaded, std::strlen(loaded) + 1);
    g.Store32(handle, 0xFFFFFFFF);
    CHECK(XamState().LoadImageFromMemory(g, module, handle) == 0xC0000035 && g.Load32(handle) == 0);
  }
  std::memcpy(g.host(module), "dlc.xex", 8);
  g.Store32(handle, 0xFFFFFFFF);
  CHECK(XamState().LoadImageFromMemory(g, module, handle) == 0xC000000F && g.Load32(handle) == 0);
  CHECK(XamState().LoadImageFromMemory(g, 0, handle) == 0xC000000F && g.Load32(handle) == 0);
  CHECK(XamState().LoadImageFromMemory(g, module, 0) == 0x57);
  XamState().Reset();
}

// Step 12, ninth batch: the content "Internal" forms, the registered callbacks, XamReadString, the Ex startups,
// network storage's privilege, ObLookupAnyThreadByThreadId (Xenia's xam_content.cc, xam_info.cc, xam_net.cc,
// xam_user.cc, xboxkrnl_ob.cc).
void XamStep12NinthTests() {
  Fresh();
  namespace fs = std::filesystem;
  XamState().Reset();
  std::error_code ec;
  const fs::path content = fs::temp_directory_path() / "any360-sys-step12-ninth";
  fs::remove_all(content, ec);
  fs::create_directories(content / "00000001" / "slotB");
  fs::create_directories(content / "00000001" / "slotA");
  XamState().title().content_root = content;
  XamState().title().title_id = 0x4D5307E6;
  XamState().title().title_name = u"Bench Title";
  std::vector<uint8_t> memory(0x100000);
  const Guest gm{memory.data()};
  const uint32_t out = 0x1000, buf = 0x2000, data = 0x3000, meta = 0x10000, ov = 0x1800;

  // XamContentCreateEnumeratorInternal: XCONTENT_DATA items, the size 0x134 a call; the disc lists none; an unknown
  // device or no handle pointer clears the size and is E_INVALIDARG.
  for (const uint32_t device : {0u, 1u}) {
    gm.Store32(out + 8, 0xFFFFFFFF);
    CHECK(XamState().ContentCreateEnumeratorInternal(gm, device, 1, 2, out + 8, out) == 0 && gm.Load32(out + 8) == 0x268);
    const uint32_t handle = gm.Load32(out);
    CHECK(XamState().Enumerate(gm, handle, 0, buf, 0x268, out + 4, 0) == 0 && gm.Load32(out + 4) == 2);
    CHECK(gm.Load32(buf) == 1 && gm.Load32(buf + 4) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(gm.host(buf + 0x108))) == "slotA" &&
          std::string(reinterpret_cast<const char*>(gm.host(buf + 0x134 + 0x108))) == "slotB");
    CHECK(XamState().Enumerate(gm, handle, 0, buf, 0x268, out + 4, 0) != 0);
  }
  CHECK(XamState().ContentCreateEnumeratorInternal(gm, 2, 1, 1, out + 8, out) == 0 && gm.Load32(out + 8) == 0x134);
  CHECK(XamState().Enumerate(gm, gm.Load32(out), 0, buf, 0x134, out + 4, 0) != 0);
  for (const uint32_t device : {3u, 0x80u}) {
    gm.Store32(out + 8, 0x55);
    CHECK(XamState().ContentCreateEnumeratorInternal(gm, device, 1, 1, out + 8, out) == 0x80070057 && gm.Load32(out + 8) == 0);
  }
  gm.Store32(out + 8, 0x55);
  CHECK(XamState().ContentCreateEnumeratorInternal(gm, 1, 1, 1, out + 8, 0) == 0x80070057 && gm.Load32(out + 8) == 0);

  // XamContentGetMetaDataInternal: the package's metadata (0x93D6 bytes) from its header; the content data only names
  // the package. Without a header: all 0. A missing package fails, nothing written, an overlapped left alone.
  std::memset(gm.host(data), 0, 0x200);
  gm.Store32(data, 1);
  gm.Store32(data + 4, 1);
  const char16_t display[] = u"Slot A";
  for (uint32_t i = 0; i < 6; ++i) gm.Store16(data + 8 + 2 * i, uint16_t(display[i]));
  std::memcpy(gm.host(data + 0x108), "slotA", 6);
  gm.Store64(data + 0x138, 0xE000000000000001ull);
  gm.Store32(data + 0x140, 0x11111111);
  auto all = [&](uint32_t from, uint32_t to, uint8_t value) {
    bool same = true;
    for (uint32_t i = from; i < to; ++i) same = same && gm.Load8(meta + i) == value;
    return same;
  };
  std::memset(gm.host(meta), 0xAA, 0x9500);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, 0) == 0);
  CHECK(all(0, 0x93D6, 0) && gm.Load8(meta + 0x93D6) == 0xAA);
  // A header as XamContentCreate writes it (XCONTENT_AGGREGATE_DATA: the content data, the title id at 0x140).
  fs::create_directories(content / "Headers" / "00000001", ec);
  auto write_header = [&](size_t size) {
    std::vector<uint8_t> header(size, 0);
    header[7] = 1;  // type 1, big-endian
    const char16_t created[] = u"Created";
    for (uint32_t i = 0; i < 7; ++i) header[8 + 2 * i + 1] = uint8_t(created[i]);
    if (size >= 0x144) header[0x140] = 0x4D, header[0x141] = 0x53, header[0x142] = 0x07, header[0x143] = 0xE6;
    std::ofstream(content / "Headers" / "00000001" / "slotA.header", std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
  };
  write_header(0x148);
  std::memset(gm.host(meta), 0xAA, 0x9500);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, 0) == 0);
  CHECK(gm.Load32(meta) == 1 && gm.Load32(meta + 4) == 2 && all(8, 0x1C, 0));
  CHECK(gm.Load32(meta + 0x1C) == 0x4D5307E6 && all(0x20, 0x59, 0));  // the header's title id; no profile id
  CHECK(gm.Load32(meta + 0x59) == 1 && all(0x5D, 0xCD, 0));           // one data file, an STFS volume (0)
  CHECK(gm.Load16(meta + 0xCD) == 'C' && gm.Load16(meta + 0xCD + 12) == 'd' && all(0xCD + 14, 0xCD + 0x100, 0));
  CHECK(all(0x1CD, 0x93D6, 0) && gm.Load8(meta + 0x93D6) == 0xAA);
  // A header shorter than an XCONTENT_AGGREGATE_DATA is not read: all 0.
  write_header(0x134);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, 0) == 0 && all(0, 0x93D6, 0));
  write_header(0x148);
  // Through an overlapped: completed at once, ERROR_IO_PENDING returned.
  std::memset(gm.host(ov), 0xFF, 0x1C);
  gm.Store32(ov + 0x0C, 0);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, ov) == 997 && gm.Load32(ov) == 0);
  // A missing package: ERROR_FUNCTION_FAILED, nothing written, the overlapped untouched.
  std::memcpy(gm.host(data + 0x108), "slotZ", 6);
  std::memset(gm.host(meta), 0xAA, 16);
  std::memset(gm.host(ov), 0xFF, 0x1C);
  gm.Store32(ov + 0x0C, 0);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, ov) == 0x65B && gm.Load8(meta) == 0xAA &&
        gm.Load32(ov) == 0xFFFFFFFF);
  std::memset(gm.host(data + 0x108), 0, 42);
  CHECK(XamState().ContentGetMetaDataInternal(gm, data, meta, 0) == 0x65B);

  // XamContentSetThumbnailInternal is XamContentSetThumbnail: the thumbnail is kept beside the package.
  std::memcpy(gm.host(data + 0x108), "slotB", 6);
  std::memcpy(gm.host(buf), "\x89PNG", 4);
  CHECK(XamState().ContentSetThumbnail(gm, data, buf, 4, 0) == 0);
  gm.Store32(out, 64);
  CHECK(XamState().ContentGetThumbnail(gm, data, buf + 0x100, out, 0) == 0 && gm.Load32(out) == 4 &&
        std::memcmp(gm.host(buf + 0x100), "\x89PNG", 4) == 0);

  // The callbacks are kept (and cleared by a reset).
  XamState().ContentRegisterChangeCallback(0x82001234);
  XamState().LoaderRegisterLaunchRequestCallback(0x82005678);
  CHECK(XamState().content_change_callback() == 0x82001234 && XamState().launch_request_callback() == 0x82005678);

  // XamReadString: an empty string (no SPA string table), the terminator only when the size allows; the errors.
  gm.Store32(buf, 0xAAAAAAAA);
  gm.Store32(out, 16);
  CHECK(XamState().ReadString(gm, 5, buf, out, 0) == 0 && gm.Load16(buf) == 0 && gm.Load16(buf + 2) == 0xAAAA);
  gm.Store32(buf, 0xAAAAAAAA);
  gm.Store32(out, 0);
  CHECK(XamState().ReadString(gm, 5, buf, out, 0) == 0 && gm.Load32(buf) == 0xAAAAAAAA);
  CHECK(XamState().ReadString(gm, 0xFFFF, buf, out, 0) == 0x80070057 && XamState().ReadString(gm, 5, 0, out, 0) == 0x80070057);
  CHECK(XamState().ReadString(gm, 5, buf, 0, 0) == 0x8007007A);
  gm.Store32(out, 16);
  std::memset(gm.host(ov), 0xFF, 0x1C);
  gm.Store32(ov + 0x0C, 0);
  CHECK(XamState().ReadString(gm, 5, buf, out, ov) == 997 && gm.Load32(ov) == 0);

  // Network storage's privilege: only a profile signed in to Xbox Live holds it; the local profile is offline.
  for (const uint32_t user : {0u, 1u, 2u, 3u, 4u, 0xFEu, 0xFFu, 0x100u}) CHECK(XamState().NetworkStorageUserHasPrivilege(user) == 0);
  // The Ex startups are the plain ones (WSADATA's versions; the startup parameters kept).
  CHECK(XamState().WsaStartup(gm, 0x0202, buf) == 0 && gm.Load16(buf) == 0x0202 && gm.Load16(buf + 2) == 0x0202);

  XamState().Reset();
  CHECK(XamState().content_change_callback() == 0 && XamState().launch_request_callback() == 0);
  fs::remove_all(content, ec);
}

void Win32Step12Tests() {
  const uint32_t kthread = kScratch + 0x800, text = kScratch + 0x400;
  std::memset(g.host(kthread), 0, 0x200);
  SetWin32LastError(g, kthread, 0x57);
  CHECK(Win32LastError(g, kthread) == 0x57 && g.Load32(kthread + 0x160) == 0x57);
  CHECK(Win32LastError(g, 0) == 0);
  // RtlSetLastNTError: STATUS_INVALID_HANDLE is ERROR_INVALID_HANDLE (6).
  CHECK(SetWin32LastNtError(g, kthread, 0xC0000008) == 6 && Win32LastError(g, kthread) == 6);
  CHECK(!Win32Timeout(0xFFFFFFFF) && *Win32Timeout(0) == 0 && *Win32Timeout(1500) == -15000000);
  // The documented results pass through; an error is WAIT_FAILED with the last error set.
  SetWin32LastError(g, kthread, 0);
  CHECK(Win32WaitResult(g, kthread, 0) == 0 && Win32WaitResult(g, kthread, 3) == 3);
  CHECK(Win32WaitResult(g, kthread, 0x80) == 0x80 && Win32WaitResult(g, kthread, 0x102) == 0x102);
  CHECK(Win32WaitResult(g, kthread, 0xC0) == 0xC0 && Win32LastError(g, kthread) == 0);
  CHECK(Win32WaitResult(g, kthread, 0xC0000008) == 0xFFFFFFFF && Win32LastError(g, kthread) == 6);
  CHECK(Win32SleepResult(0) == 0 && Win32SleepResult(0xC0) == 0xC0);
  // lstrlenW on a big-endian UTF-16 string.
  const char16_t word[] = u"Anyé360";
  for (uint32_t i = 0; i <= 7; ++i) g.Store16(text + 2 * i, uint16_t(word[i]));
  CHECK(LstrlenW(g, text) == 7 && LstrlenW(g, 0) == 0);
  g.Store16(text, 0);
  CHECK(LstrlenW(g, text) == 0);
  // Units with a zero byte are not the end (U+0100 is 01 00 in guest order, U+0041 00 41), at any length and
  // alignment, an odd address too.
  for (const uint32_t start : {text, text + 2, text + 6, text + 14, text + 1}) {
    for (const uint32_t length : {1u, 7u, 8u, 9u, 31u, 300u}) {
      for (uint32_t i = 0; i < length; ++i) g.Store16(start + 2 * i, i % 2 ? 0x0100 : 0x0041);
      g.Store16(start + 2 * length, 0);
      CHECK(LstrlenW(g, start) == length);
    }
  }
  // The tick count is KeTimeStampBundle's: it moves on.
  const uint32_t before = TickCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(TickCount() - before >= 15);
}

// U32Map (the object manager's flat tables) against std::map on a random mix of insertions, look-ups and removals,
// key 0 and colliding aligned addresses included, through several growths.
void U32MapTests() {
  U32Map map(4);
  std::map<uint32_t, uint32_t> model;
  std::mt19937 random(1234);
  for (int step = 0; step < 200000; ++step) {
    const uint32_t key = (random() % 3000) * 16 + (step % 7 == 0 ? 0 : 0x10000);  // aligned; key 0 now and then
    switch (random() % 3) {
      case 0: {
        const uint32_t value = random();
        map[key] = value;
        model[key] = value;
        break;
      }
      case 1: CHECK(map.Erase(key) == (model.erase(key) == 1)); break;
      default: {
        const uint32_t* found = map.Find(key);
        const auto it = model.find(key);
        CHECK((found != nullptr) == (it != model.end()));
        if (found && it != model.end()) CHECK(*found == it->second);
      }
    }
    if (step % 50000 == 0) CHECK(map.size() == model.size());
  }
  CHECK(map.size() == model.size());
  for (const auto& [key, value] : model) CHECK(map.Find(key) && *map.Find(key) == value);
  // Key 0 (kept apart from the slots).
  map[0] = 5;
  CHECK(map.Find(0) && *map.Find(0) == 5 && map.Erase(0) && !map.Find(0) && !map.Erase(0));
  // Clear keeps the storage: refilling to the same size allocates nothing.
  const size_t capacity = map.capacity();
  map.Clear();
  CHECK(map.size() == 0 && !map.Find(0) && !map.Find(0x10000));
  const AllocCounts before = ThreadAllocations();
  for (uint32_t i = 0; i < capacity; ++i) map[0x10000 + i * 16] = i;
  for (uint32_t i = 0; i < capacity; ++i) CHECK(map.Erase(0x10000 + i * 16));
  CHECK((ThreadAllocations() - before).count == 0 && map.size() == 0);
}

// The object manager's bookkeeping: the handle table reuses its lowest closed slot, the heap its lowest fitting block,
// freed blocks coalesce back into the heap whatever the closing order, and a title's create / close cycles allocate
// nothing on the host once the tables have their working size.
void ObjectBookkeepingTests() {
  Fresh();
  const uint32_t out = kScratch + 0x200;
  auto event = [&]() {
    CHECK(Objects().CreateEvent(g, out, 0, 0, 0) == 0);
    return g.Load32(out);
  };
  // Lowest closed slot first, as the table always did.
  const uint32_t h1 = event(), h2 = event(), h3 = event(), h4 = event();
  CHECK(h2 == h1 + 4 && h3 == h2 + 4 && h4 == h3 + 4);
  CHECK(Objects().Close(h3) == 0 && Objects().Close(h2) == 0);
  const uint32_t h5 = event(), h6 = event(), h7 = event();
  CHECK(h5 == h2 && h6 == h3 && h7 == h4 + 4);
  for (uint32_t h : {h1, h4, h5, h6, h7}) CHECK(Objects().Close(h) == 0);
  CHECK(Objects().Lookup(event()) == kHeap);  // everything went back: the heap starts over
  Fresh();
  // A thousand objects (past the tables' first capacity) closed in a random order coalesce back into the heap.
  std::vector<uint32_t> handles;
  for (int i = 0; i < 1000; ++i) handles.push_back(event());
  CHECK(Objects().Lookup(handles.back()) == kHeap + 999 * 16);
  std::mt19937 random(99);
  std::shuffle(handles.begin(), handles.end(), random);
  for (uint32_t h : handles) CHECK(Objects().Close(h) == 0);
  // One block the size of the whole used region fits at the heap's start: no freed block was left apart.
  CHECK(Objects().AllocateObject(1000 * 16, true) == kHeap);
  Fresh();
  // A block freed below the heap's top is found again first fit and split; its rest is still found.
  const uint32_t big = Objects().AllocateObject(64, true), top = event();
  CHECK(big == kHeap && Objects().Lookup(top) == kHeap + 64);
  Objects().FreeObject(big);
  const uint32_t a = event(), b = event();
  CHECK(Objects().Lookup(a) == kHeap && Objects().Lookup(b) == kHeap + 16);
  CHECK(Objects().AllocateObject(32, true) == kHeap + 32);
  CHECK(Objects().AllocateObject(16, true) == kHeap + 80);  // the freed block is used up
  // Duplicates and Ob references count apart; the object goes when neither is left.
  Fresh();
  const uint32_t d = event(), d_at = Objects().Lookup(d);
  CHECK(Objects().Duplicate(g, d, out + 4, 1) == 0);  // DUPLICATE_CLOSE_SOURCE: one handle left
  const uint32_t d2 = g.Load32(out + 4);
  CHECK(Objects().Lookup(d) == 0 && Objects().Lookup(d2) == d_at);
  CHECK(Objects().ReferenceObjectByHandle(g, d2, kMainThread, out + 8) == 0);
  CHECK(Objects().ReferenceObjectByHandle(g, d2, kMainThread, out + 8) == 0);
  CHECK(Objects().Close(d2) == 0 && Objects().AllocateObject(16, true) != d_at);
  CHECK(Objects().DereferenceObject(g, d_at) == 0 && Objects().AllocateObject(16, true) != d_at);
  CHECK(Objects().DereferenceObject(g, d_at) == 0 && Objects().AllocateObject(16, true) == d_at);
  CHECK(Objects().DereferenceObject(g, d_at) == 0);  // one too many: nothing happens
  // Host memory: the cycles a title makes, after a warm-up, allocate nothing.
  Fresh();
  std::vector<uint32_t> held;
  for (int i = 0; i < 64; ++i) held.push_back(event());  // a title's usual table
  auto cycles = [&] {
    for (int i = 0; i < 1000; ++i) {
      const uint32_t x = event();
      CHECK(Objects().ReferenceObjectByHandle(g, x, kMainThread, out + 8) == 0);
      CHECK(Objects().DereferenceObject(g, g.Load32(out + 8)) == 0);
      const uint32_t y = event();
      CHECK(Objects().Close(x) == 0 && Objects().Close(y) == 0);  // the lower first: a freed block apart, then joined
    }
  };
  cycles();
  {
    // The meter counts in this executable (a zero below means something): one allocation seen.
    const AllocCounts start = ThreadAllocations();
    std::vector<uint8_t> control(100);
    volatile uint8_t* data = control.data();
    data[0] = 1;
    CHECK((ThreadAllocations() - start).count >= 1 && (ThreadAllocations() - start).bytes >= 100);
  }
  const AllocCounts before = ThreadAllocations();
  cycles();
  const AllocCounts spent = ThreadAllocations() - before;
  CHECK(spent.count == 0 && spent.bytes == 0);
  if (spent.count) std::printf("  object cycles: %llu allocations, %llu bytes\n", (unsigned long long)spent.count, (unsigned long long)spent.bytes);
  for (uint32_t h : held) CHECK(Objects().Close(h) == 0);
}

// Plain objects (AllocatePlainObject): their last close runs their owner's close hook alone, their memory goes back to
// the heap; with ANY360_VERIFY_OBJECT_HINTS every hook still runs (and one holding state for such an object stops the
// process). A timer set on a plain object clears its hint: the full close path cancels it.
uint32_t g_hinted_object = 0;
int g_owner_calls = 0, g_other_calls = 0;
bool g_owner_claims = true;  // whether the owner hook says it held state for g_hinted_object
void SetVerifyHints(bool on) {
#if defined(_WIN32)
  _putenv_s("ANY360_VERIFY_OBJECT_HINTS", on ? "1" : "0");
#else
  setenv("ANY360_VERIFY_OBJECT_HINTS", on ? "1" : "0", 1);
#endif
}
void PlainObjectTests() {
  const char* suite = std::getenv("ANY360_VERIFY_OBJECT_HINTS");
  const bool suite_verifies = suite && *suite && *suite != '0';
  const int owner = Objects().AddCloseHook([](uint32_t object) {
    if (object != g_hinted_object) return false;
    ++g_owner_calls;
    return g_owner_claims;
  });
  Objects().AddCloseHook([](uint32_t object) {
    if (object == g_hinted_object) ++g_other_calls;
    return false;
  });
  for (const bool verify : {false, true}) {
    SetVerifyHints(verify);
    Fresh();
    XamState().Reset();  // no enumerator or listener left by an earlier test at a reused address
    g_owner_calls = g_other_calls = 0;
    g_hinted_object = Objects().AllocatePlainObject(16, owner);
    const uint32_t h = Objects().OpenHandle(g_hinted_object), again = Objects().OpenHandle(g_hinted_object);
    CHECK(Objects().Close(h) == 0 && g_owner_calls == 0);  // a handle still open: no hook yet
    CHECK(Objects().Close(again) == 0 && g_owner_calls == 1 && g_other_calls == (verify ? 1 : 0));
    CHECK(Objects().AllocateObject(16, true) == g_hinted_object);  // its memory went back
    // An object with no hint still runs every hook.
    g_owner_calls = g_other_calls = 0;
    g_hinted_object = Objects().AllocateObject(16, true);
    CHECK(Objects().Close(Objects().OpenHandle(g_hinted_object)) == 0 && g_owner_calls == 1 && g_other_calls == 1);
    // A plain object no hook keeps: none runs (under verify all run, none holding state for it).
    g_owner_calls = g_other_calls = 0;
    g_owner_claims = false;
    g_hinted_object = Objects().AllocatePlainObject(16, ObjectManager::kNoCloseHook);
    CHECK(Objects().Close(Objects().OpenHandle(g_hinted_object)) == 0 && g_owner_calls == (verify ? 1 : 0));
    g_owner_claims = true;
    // A timer with an APC set on a plain object: cancelled with it (the hint cleared; under verify a release still
    // marked plain would stop here).
    const uint32_t out = kScratch + 0x200;
    g_hinted_object = 0;
    const uint32_t timer_at = Objects().AllocatePlainObject(kTimerSize, ObjectManager::kNoCloseHook);
    Objects().InitializeTimer(g, timer_at, 0, 0);
    const uint32_t timer = Objects().OpenHandle(timer_at);
    g.Store64(out + 8, uint64_t(int64_t(-10000)));  // 1 ms
    CHECK(Objects().NtSetTimerEx(g, timer, out + 8, 0x82000000, 0, kMainThread, 0, 0) == 0 && Objects().Close(timer) == 0);
    CHECK(Objects().AllocateObject(kTimerSize, true) == timer_at);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(Threads().PendingApcs(kMainThread) == 0);  // the cancelled timer queued nothing
  }
  g_hinted_object = 0;
  SetVerifyHints(suite_verifies);
  Fresh();
}

// A file stays usable while devices are mounted, remounted and unmounted around it: an open handle keeps the mount
// entry it was opened on (the list is reordered and its entries replaced under it).
void MountStabilityTests() {
  Fresh();
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "any360-sys-mounts";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "hdd");
  fs::create_directories(root / "other");
  Files().Reset();
  Files().Mount("\\Device\\Harddisk0\\Partition1", root / "hdd", false);
  Files().Link("hdd:", "\\Device\\Harddisk0\\Partition1");
  const uint32_t attrs = kScratch + 0x100, name = kScratch + 0x120, text = kScratch + 0x140, iosb = kScratch + 0x300,
                 out = kScratch + 0x320, buf = kScratch + 0x400;
  auto path = [&](const char* p) {
    g.Store32(attrs, 0);
    g.Store32(attrs + 4, name);
    g.Store16(name, uint16_t(std::strlen(p)));
    g.Store32(name + 4, text);
    std::memcpy(g.host(text), p, std::strlen(p) + 1);
    return attrs;
  };
  const uint32_t kRead = 0x80000000, kWrite = 0x40000000;
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\save.bin"), iosb, 0, kOverwriteIf,
                           kFileNonDirectoryFile | kFileSynchronousIo) == 0);
  const uint32_t f = g.Load32(out);
  std::memcpy(g.host(buf), "before", 6);
  CHECK(Files().WriteFile(g, f, 0, iosb, buf, 6, 0) == 0 && g.Load32(iosb + 4) == 6);
  // Other devices mounted (the list grows past its capacity and is sorted again), the file's own device remounted
  // read-only on the same folder, then another unmounted.
  for (int i = 0; i < 40; ++i) Files().Mount("\\Device\\Content\\" + std::to_string(i) + std::string(i % 5, 'x'), root / "other", false);
  Files().Mount("\\Device\\Harddisk0\\Partition1", root / "hdd", true);
  Files().Mount("\\Device\\Harddisk0\\Partition1", root / "hdd", true);
  Files().Unmount("\\Device\\Content\\7xx");
  Files().MountNull("\\Device\\Harddisk0\\Partition0");
  // The open handle writes, reads and closes as it was opened (writable); a new open sees the read-only device.
  std::memcpy(g.host(buf), "after", 5);
  CHECK(Files().WriteFile(g, f, 0, iosb, buf, 5, 0) == 0 && g.Load32(iosb + 4) == 5);
  g.Store64(out + 8, 0);
  std::memset(g.host(buf), 0, 16);
  CHECK(Files().ReadFile(g, f, 0, iosb, buf, 16, out + 8) == 0 && g.Load32(iosb + 4) == 11 &&
        std::memcmp(g.host(buf), "beforeafter", 11) == 0);
  CHECK(Files().QueryVolume(g, f, iosb, buf, 0x40, kFsSizeInformation) == 0);
  CHECK(Objects().Close(f) == 0);
  CHECK(Files().CreateFile(g, out, kRead | kWrite, path("hdd:\\save.bin"), iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo) ==
        kStatusAccessDenied);
  CHECK(Files().CreateFile(g, out, kRead, path("hdd:\\save.bin"), iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo) == 0);
  const uint32_t r = g.Load32(out);
  // Unmounted under an open handle: it still reads, then closes.
  Files().Unmount("\\Device\\Harddisk0\\Partition1");
  std::memset(g.host(buf), 0, 16);
  g.Store64(out + 8, 6);
  CHECK(Files().ReadFile(g, r, 0, iosb, buf, 5, out + 8) == 0 && std::memcmp(g.host(buf), "after", 5) == 0);
  CHECK(Objects().Close(r) == 0);
  Files().Reset();
  fs::remove_all(root, ec);
}

// Links and mounts changed on one thread (a title creating and closing content) while three others open files
// through a link and query a symbolic link object: every open of the stable link succeeds, nothing crashes.
void LinkConcurrencyTests() {
  Fresh();
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "any360-sys-links";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "game");
  fs::create_directories(root / "dlc");
  { std::ofstream(root / "game" / "data.bin", std::ios::binary) << "0123456789ABCDEF"; }
  Files().Reset();
  Files().Mount("\\Device\\Cdrom0", root / "game", true);
  Files().Link("game:", "\\Device\\Cdrom0");
  std::atomic<bool> stop{false};
  std::atomic<int> opened{0}, failed{0}, queried{0};
  std::thread changer([&] {
    for (int i = 0; !stop; ++i) {
      const std::string device = "\\Device\\Content\\" + std::to_string(i % 8), link = "dlc" + std::to_string(i % 8) + ":";
      Files().Mount(device, root / "dlc", false);
      Files().Link(link, device);
      Files().Unlink(link);
      Files().Unmount(device);
    }
  });
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&, t] {
      // Each thread its own guest scratch (the object attributes, the name, the handle and status block).
      const uint32_t base = kScratch + 0x800 + uint32_t(t) * 0x200, attrs = base, name = base + 0x20, text = base + 0x40,
                     iosb = base + 0x100, out = base + 0x120, target = base + 0x140, target_text = base + 0x160;
      auto path = [&](const char* p) {
        g.Store32(attrs, 0);
        g.Store32(attrs + 4, name);
        g.Store16(name, uint16_t(std::strlen(p)));
        g.Store32(name + 4, text);
        std::memcpy(g.host(text), p, std::strlen(p) + 1);
        return attrs;
      };
      for (int i = 0; i < 300; ++i) {
        if (Files().CreateFile(g, out, 0x80000000, path("game:\\data.bin"), iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo) == 0) {
          ++opened;
          Objects().Close(g.Load32(out));
        } else {
          ++failed;
        }
        if (Files().OpenSymbolicLink(g, out, path("\\??\\game:")) == 0) {
          const uint32_t link = g.Load32(out);
          g.Store16(target + 2, 64);
          g.Store32(target + 4, target_text);
          if (Files().QuerySymbolicLink(g, link, target) == 0 && g.Load16(target) == 14 &&
              std::memcmp(g.host(target_text), "\\Device\\Cdrom0", 14) == 0) {
            ++queried;
          }
          Objects().Close(link);
        }
      }
    });
  }
  for (auto& r : readers) r.join();
  stop = true;
  changer.join();
  CHECK(opened == 900 && failed == 0 && queried == 900);
  Files().Reset();
  fs::remove_all(root, ec);
}

// Step 12, seventh batch: SHA-512 (FIPS 180-4's vectors, streamed at random splits, the state's bytes), the BnQw
// helpers against Xenia's code as the model (and the arithmetic itself), the two signature formats, the console's
// identity and the key vault's sizes.
namespace crypt7 {

// Xenia's XeCryptBnQwNeModInv, as written there.
uint64_t ModInvModel(uint64_t input) {
  uint64_t intermediate = input;
  uint64_t result = (intermediate * 3) ^ 2;
  intermediate = 1 - (result * intermediate);
  for (uint32_t i = 5; i < 32; i <<= 1) {
    result = result * (intermediate + 1);
    intermediate = intermediate * intermediate;
  }
  return result * (intermediate + 1);
}

// Xenia's XeCryptBnQwNeModMul on host words.
std::vector<uint64_t> ModMulModel(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, uint64_t inverse,
                                  const std::vector<uint64_t>& modulus) {
  using U = unsigned __int128;
  const size_t n = a.size();
  std::vector<uint64_t> ab(n), mn(n), out(n);
  for (size_t i = 0; i < n; i++) {
    const uint64_t bi = b[i];
    const uint64_t m = (ab[0] - mn[0] + bi * a[0]) * inverse;
    uint64_t ab_carry = 0, mn_carry = 0;
    for (size_t j = 0; j < n; j++) {
      const U t = U(bi) * a[j] + ab[j] + ab_carry;
      const U u = U(m) * modulus[j] + mn[j] + mn_carry;
      if (j) ab[j - 1] = uint64_t(t), mn[j - 1] = uint64_t(u);
      ab_carry = uint64_t(t >> 64), mn_carry = uint64_t(u >> 64);
    }
    ab[n - 1] = ab_carry, mn[n - 1] = mn_carry;
  }
  bool greater = false;
  for (size_t i = n; i-- > 0;) {
    if (ab[i] != mn[i]) {
      greater = ab[i] > mn[i];
      break;
    }
  }
  uint64_t carry = 0, borrow = 0;
  for (size_t i = 0; i < n; i++) {
    const U sum = U(ab[i]) + (greater ? 0 : modulus[i]) + carry;
    carry = uint64_t(sum >> 64);
    const uint64_t lo = uint64_t(sum), diff = lo - mn[i];
    const uint64_t next_borrow = lo < mn[i] || diff < borrow;
    out[i] = diff - borrow;
    borrow = next_borrow;
  }
  return out;
}

// RC4 over host bytes (the test's own).
void Rc4(const uint8_t* key, size_t key_size, uint8_t* data, size_t size) {
  uint8_t st[256];
  for (int x = 0; x < 256; ++x) st[x] = uint8_t(x);
  for (int x = 0, j = 0; x < 256; ++x) {
    j = (j + st[x] + key[x % key_size]) & 255;
    std::swap(st[x], st[j]);
  }
  for (size_t n = 0, i = 0, j = 0; n < size; ++n) {
    i = (i + 1) & 255, j = (j + st[i]) & 255;
    std::swap(st[i], st[j]);
    data[n] ^= st[(st[i] + st[j]) & 255];
  }
}

#pragma pack(push, 1)
// Xenia's XE_CONSOLE_ID, its fields set as Xenia sets them: the bytes the console id must have.
struct ConsoleIdModel {
  union {
    struct {
      uint8_t RefurbBits : 4;
      uint8_t ManufactureMonth : 4;
      uint32_t ManufactureYear : 4;
      uint32_t MacIndex3 : 8;
      uint32_t MacIndex4 : 8;
      uint32_t MacIndex5 : 8;
      uint32_t Crc : 4;
    };
    uint8_t Data[5];
  };
};
#pragma pack(pop)

}  // namespace crypt7

void CryptStep12SeventhTests() {
  using namespace crypt7;
  const uint32_t in = kScratch, out = kScratch + 0x400, state = kScratch + 0x600, other = kScratch + 0x700;
  auto hex = [](uint32_t at, uint32_t size) {
    std::string text;
    for (uint32_t i = 0; i < size; ++i) text += "0123456789abcdef"[g.Load8(at + i) >> 4], text += "0123456789abcdef"[g.Load8(at + i) & 15];
    return text;
  };
  auto put = [](uint32_t at, const std::string& bytes) { std::memcpy(g.host(at), bytes.data(), bytes.size()); };
  std::mt19937_64 rng(7);

  // SHA-512 on the portable code and on the processor's (BMI2) path when it has one.
  const ShaPath restore = CurrentShaPath();
  for (const ShaPath path : {ShaPath::kPortable, DefaultShaPath()}) {
    SetShaPath(path);
    // SHA-512: FIPS 180-4's vectors (one block, empty, two padding blocks), one call and streamed.
    const std::pair<std::string, const char*> sha512[] = {
        {"abc",
         "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
        {"",
         "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"},
    };
    for (const auto& [message, digest] : sha512) {
      put(in, message);
      XeCryptSha512Init(g, state);
      XeCryptSha512Update(g, state, in, uint32_t(message.size()));
      std::memset(g.host(out), 0xAA, 0x48);
      XeCryptSha512Final(g, state, out, 64);
      CHECK(hex(out, 64) == digest);
      CHECK(g.Load8(out + 64) == 0xAA);
      const auto host = Sha512Digest(reinterpret_cast<const uint8_t*>(message.data()), message.size());
      CHECK(std::memcmp(host.data(), g.host(out), 64) == 0);
      // The state: the count kept, the digest in the state words, the last padded block in the buffer.
      CHECK(g.Load32(state) == message.size() && hex(state + 8, 64) == digest);
      const uint32_t last = message.size() < 112 ? 0 : 128;  // the second padding block starts zeroed
      CHECK(g.Load8(state + 0x48 + 127) == uint8_t(message.size() * 8) && g.Load8(state + 0x48 + 126) == uint8_t(message.size() * 8 >> 8));
      if (!last) CHECK(std::memcmp(g.host(state + 0x48), message.data(), message.size()) == 0 && g.Load8(state + 0x48 + uint32_t(message.size())) == 0x80);
      if (last) CHECK(g.Load8(state + 0x48) == 0 && g.Load8(state + 0x48 + 111) == 0);
    }
    // Init's bytes, and a short output.
    XeCryptSha512Init(g, state);
    CHECK(g.Load32(state) == 0 && g.Load32(state + 4) == 0 && g.Load64(state + 8) == 0x6a09e667f3bcc908 &&
          g.Load64(state + 0x40) == 0x5be0cd19137e2179 && g.Load8(state + 0x48) == 0 && g.Load8(state + 0xC7) == 0);
    put(in, "abc");
    XeCryptSha512Update(g, state, in, 3);
    std::memset(g.host(out), 0xAA, 64);
    XeCryptSha512Final(g, state, out, 10);
    CHECK(hex(out, 10) == "ddaf35a193617abacc41" && g.Load8(out + 10) == 0xAA);
    // Lengths 0 to 400 split into three updates at random places: the digest as the one-call form, and the state between
    // updates as a byte-by-byte stream leaves it (the count, the words, the buffered bytes).
    for (uint32_t length = 0; length <= 400; ++length) {
      for (uint32_t i = 0; i < length; ++i) g.Store8(in + i, uint8_t(rng()));
      const uint32_t a = uint32_t(rng() % (length + 1)), b = a + uint32_t(rng() % (length - a + 1));
      XeCryptSha512Init(g, state);
      XeCryptSha512Update(g, state, in, a);
      XeCryptSha512Update(g, state, in + a, b - a);
      XeCryptSha512Update(g, state, in + b, length - b);
      XeCryptSha512Init(g, other);
      for (uint32_t i = 0; i < length; ++i) XeCryptSha512Update(g, other, in + i, 1);
      CHECK(g.Load32(state) == length && std::memcmp(g.host(state), g.host(other), 0x48 + length % 128) == 0);
      XeCryptSha512Final(g, state, out, 64);
      const auto host = Sha512Digest(g.host(in), length);
      CHECK(std::memcmp(host.data(), g.host(out), 64) == 0);
    }
  }
  SetShaPath(restore);

  // XeCryptBnQw_Copy: words copied whole, overlapping both ways.
  for (uint32_t i = 0; i < 64; ++i) g.Store8(in + i, uint8_t(i));
  XeCryptBnQw_Copy(g, in, in + 8, 4);
  CHECK(g.Load8(in + 8) == 0 && g.Load8(in + 39) == 31 && g.Load8(in + 40) == 40);
  XeCryptBnQw_Copy(g, in + 8, in, 4);
  CHECK(g.Load8(in) == 0 && g.Load8(in + 31) == 31 && g.Load8(in + 32) == 24);
  XeCryptBnQw_Copy(g, in, out, 0);

  // XeCryptBnQwNeModInv: Xenia's result for any word, and the inverse for an odd one.
  for (int i = 0; i < 10000; ++i) {
    const uint64_t v = rng();
    CHECK(XeCryptBnQwNeModInv(v) == ModInvModel(v));
    CHECK(((v | 1) * XeCryptBnQwNeModInv(v | 1)) == 1);
  }

  // XeCryptBnQwNeModMul: the model's words for sizes 1 to 70 (past the stack's 64), with the output over each input;
  // and the arithmetic: out * 2^64 = a * b (mod m) for one word.
  for (uint32_t n = 1; n <= 70; ++n) {
    std::vector<uint64_t> a(n), b(n), m(n);
    for (uint32_t i = 0; i < n; ++i) a[i] = rng(), b[i] = rng(), m[i] = rng();
    m[0] |= 1;
    m[n - 1] |= uint64_t(1) << 63;
    if (n % 3 == 0) a[n - 1] >>= 1, b[n - 1] >>= 1;  // below the modulus
    const uint64_t inverse = XeCryptBnQwNeModInv(m[0]);
    const auto expected = ModMulModel(a, b, inverse, m);
    const uint32_t ga = kScratch + 0x1000, gb = ga + 0x400, gm = gb + 0x400, go = gm + 0x400;
    for (int where = 0; where < 3; ++where) {
      for (uint32_t i = 0; i < n; ++i) g.Store64(ga + 8 * i, a[i]), g.Store64(gb + 8 * i, b[i]), g.Store64(gm + 8 * i, m[i]);
      const uint32_t target = where == 0 ? go : where == 1 ? ga : gb;
      XeCryptBnQwNeModMul(g, ga, gb, target, inverse, gm, n);
      bool same = true;
      for (uint32_t i = 0; i < n; ++i) same = same && g.Load64(target + 8 * i) == expected[i];
      CHECK(same);
    }
  }
  for (int i = 0; i < 1000; ++i) {
    using U = unsigned __int128;
    const uint64_t m = rng() | 1 | uint64_t(1) << 63, a = rng() % m, b = rng() % m;
    g.Store64(in, a), g.Store64(in + 8, b), g.Store64(in + 16, m);
    XeCryptBnQwNeModMul(g, in, in + 8, out, XeCryptBnQwNeModInv(m), in + 16, 1);
    const uint64_t r = g.Load64(out);
    // The remainders bit by bit (no 128-bit division in the MSVC runtime).
    auto mod = [m](U x) {
      U r = 0;
      for (int bit = 127; bit >= 0; --bit) {
        r = r << 1 | ((x >> bit) & 1);
        if (r >= m) r -= m;
      }
      return uint64_t(r);
    };
    CHECK(r < m && mod(U(r) << 64) == mod(U(a) * b));
  }

  // XeCryptBnQwBeSigFormat: undone with the RC4 key it carries, the block is 0 ... 0, 1, the salt, SHA-1(8 zero bytes,
  // hash, salt), 0xBC (the top bit cleared), stored as 32 words, least significant first.
  for (int t = 0; t < 20; ++t) {
    const uint32_t hash = kScratch + 0x100, salt = kScratch + 0x140, sig = kScratch + 0x200;
    for (uint32_t i = 0; i < 20; ++i) g.Store8(hash + i, uint8_t(rng()));
    if (t == 0) {
      put(salt, "XBOX360SVC");
    } else {
      for (uint32_t i = 0; i < 10; ++i) g.Store8(salt + i, uint8_t(rng()));
    }
    std::memset(g.host(sig + 256), 0xAA, 8);
    XeCryptBnQwBeSigFormat(g, sig, hash, salt);
    CHECK(g.Load8(sig + 256) == 0xAA);
    uint8_t block[256];
    for (int i = 0; i < 32; ++i) std::memcpy(block + 8 * i, g.host(sig + 256 - 8 * (i + 1)), 8);
    uint8_t zeros[8] = {};
    std::memcpy(g.host(kScratch + 0x180), zeros, 8);
    XeCryptSha(g, kScratch + 0x180, 8, hash, 20, salt, 10, kScratch + 0x1A0, 20);
    CHECK(std::memcmp(block + 0xEB, g.host(kScratch + 0x1A0), 20) == 0 && block[0xFF] == 0xBC);
    const uint8_t top = block[0];
    Rc4(block + 0xEB, 20, block, 0xEB);
    bool zero = true;
    for (int i = 1; i < 0xE0; ++i) zero = zero && block[i] == 0;
    // The first byte: the stream's top bit was cleared, so only its low 7 bits are known.
    uint8_t stream = 0;
    Rc4(block + 0xEB, 20, &stream, 1);
    CHECK(zero && block[0xE0] == 1 && std::memcmp(block + 0xE1, g.host(salt), 10) == 0 && (top & 0x80) == 0 &&
          top == (stream & 0x7F));
  }

  // XeCryptBnDwLePkcs1Format: the hash reversed, the DigestInfo (reversed, with its separator), 0xFF, then 1 and 0 at
  // the top; sizes outside 39-512 all 0xFF.
  {
    const uint32_t hash = kScratch + 0x100, sig = kScratch + 0x200;
    for (uint32_t i = 0; i < 20; ++i) g.Store8(hash + i, uint8_t(0x10 + i));
    const std::string info0("\x14\x04\x00\x05\x1A\x02\x03\x0E\x2B\x05\x06\x09\x30\x21\x30\x00", 16);
    const std::string info1("\x14\x04\x1A\x02\x03\x0E\x2B\x05\x06\x07\x30\x1F\x30\x00", 14);
    const std::string info2 = std::string(1, '\0');
    const std::string* infos[] = {&info0, &info1, &info2};
    for (uint32_t format = 0; format < 4; ++format) {
      for (const uint32_t size : {39u, 128u, 256u, 512u}) {
        std::memset(g.host(sig), 0xAA, 520);
        XeCryptBnDwLePkcs1Format(g, hash, format, sig, size);
        bool ok = g.Load8(sig + size - 1) == 0 && g.Load8(sig + size - 2) == 1 && g.Load8(sig + size) == 0xAA;
        for (uint32_t i = 0; i < 20; ++i) ok = ok && g.Load8(sig + i) == 0x10 + 19 - i;
        const uint32_t prefix = format < 3 ? uint32_t(infos[format]->size()) : 0;
        if (format < 3) ok = ok && std::memcmp(g.host(sig + 20), infos[format]->data(), prefix) == 0;
        for (uint32_t i = 20 + prefix; i < size - 2; ++i) ok = ok && g.Load8(sig + i) == 0xFF;
        CHECK(ok);
      }
    }
    for (const uint32_t size : {0u, 1u, 38u, 513u}) {
      std::memset(g.host(sig), 0xAA, 520);
      XeCryptBnDwLePkcs1Format(g, hash, 0, sig, size);
      bool ok = g.Load8(sig + size) == 0xAA;
      for (uint32_t i = 0; i < size; ++i) ok = ok && g.Load8(sig + i) == 0xFF;
      CHECK(ok);
    }
  }

  // The console's identity.
  g.Store32(out, 0xAAAAAAAA);
  CHECK(XeKeysGetConsoleType(g, out) == 0 && g.Load32(out) == 2);
  CHECK(XeKeysGetConsoleType(g, 0) == 0);
  ConsoleIdModel model{};
  model.RefurbBits = 0b0011, model.ManufactureMonth = 0b1001, model.ManufactureYear = 0b0001;
  model.MacIndex3 = 0b01000000, model.MacIndex4 = 0b01100110, model.MacIndex5 = 0b01111110, model.Crc = 0;
  std::memset(g.host(out), 0xAA, 32);
  CHECK(XeKeysGetConsoleID(g, out, out + 16) == 0);
  CHECK(std::memcmp(g.host(out), model.Data, 5) == 0 && g.Load8(out + 5) == 0xAA);
  CHECK(std::memcmp(g.host(out + 16), "245149100000", 12) == 0 && g.Load8(out + 28) == 0xAA);
  CHECK(XeKeysGetConsoleID(g, 0, 0) == 0);
  // The key vault's sizes (Xenia's table), 0 past it.
  const std::pair<uint32_t, uint32_t> sizes[] = {{0x00, 1},     {0x03, 1},     {0x04, 2},     {0x06, 4},    {0x0A, 8},
                                                 {0x0E, 0x10},  {0x13, 0x10},  {0x14, 0xC},   {0x16, 2},    {0x1B, 0x18},
                                                 {0x1C, 0x10},  {0x32, 0x10},  {0x33, 0x1D0}, {0x34, 0x390}, {0x36, 0x1A8},
                                                 {0x37, 0x1288}, {0x38, 0x2108}, {0x39, 0},    {0x43, 0},     {0x44, 0x100},
                                                 {0x45, 0},     {0x2000, 0},   {0xFFFFFFFF, 0}};
  for (const auto& [key, size] : sizes) CHECK(XeKeysGetKeyProperties(key) == size);

  // Host memory: none for any of them (ModMul up to the stack's 64 words).
  const AllocCounts before = ThreadAllocations();
  XeCryptSha512Init(g, state);
  XeCryptSha512Update(g, state, in, 300);
  XeCryptSha512Final(g, state, out, 64);
  XeCryptBnQwNeModMul(g, kScratch + 0x1000, kScratch + 0x1400, kScratch + 0x1C00, 1, kScratch + 0x1800, 64);
  XeCryptBnQwBeSigFormat(g, kScratch + 0x200, kScratch + 0x100, kScratch + 0x140);
  XeCryptBnDwLePkcs1Format(g, kScratch + 0x100, 0, kScratch + 0x200, 256);
  XeKeysGetConsoleID(g, out, out + 16);
  CHECK((ThreadAllocations() - before).count == 0);
}

int main() {
  LinkConcurrencyTests();
  U32MapTests();
  ObjectBookkeepingTests();
  PlainObjectTests();
  MountStabilityTests();
  CryptPathTests();
  XamInfoStep12Tests();
  KernelStep12Tests();
  GuideNuiStep12Tests();
  ProfileStep12Tests();
  CryptStep12SeventhTests();
  MiscStep12EighthTests();
  XamStep12NinthTests();
  ObStep12TenthTests();
  Win32Step12Tests();
  InputQueueTests();
  InputScriptTests();
  ProfileSettingsKept();
  ConcurrentDispatcher();
  SimpsonsTests();
  ContentAggregateTests();
  TduTests();
  GraphicsAudioTests();
  SonicTests();
  XamTests();
  FileTests();
  KernelOddsTests();
  CompletionPortTests();
  LocaleTests();
  VideoTests();
  Win32FileTests();
  VariableTests();
  UserProfileTests();
  MemoryTests();
  SchedulerTests();
  StackTests();
  SpinPacerTests();
  ReclaimTests();
  RwLockTests();
  Timers();
  WaitsThatSleep();
  Lifetimes();
  MultipleWaits();
  Events();
  Semaphores();
  Mutants();
  References();
  std::printf("sys_object_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
