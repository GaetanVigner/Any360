// PreciseSleep (spin_pacer.h), apart from the scheduler so that windows.h's macros stay out of it.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include "any360/sys/spin_pacer.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace any360::sys {

bool PreciseSleep(std::chrono::nanoseconds d) {
#if defined(_WIN32)
  // A high-resolution waitable timer, one a thread (Windows 10 1803 and later; a plain Sleep rounds to the timer
  // period, up to 15.6 ms).
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle) CloseHandle(handle);
    }
  };
  static thread_local Timer timer;
  if (!timer.handle) return false;
  LARGE_INTEGER due;
  due.QuadPart = -std::max<int64_t>(1, d.count() / 100);  // relative, 100 ns units
  if (!SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) return false;
  return WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0;
#else
  std::this_thread::sleep_for(d);  // nanosleep: precise to tens of microseconds
  return true;
#endif
}

}  // namespace any360::sys
