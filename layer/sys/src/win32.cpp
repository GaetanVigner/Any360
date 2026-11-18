#include "any360/sys/win32.h"

#include <cstring>
#include <cwchar>
#include <string>

#include "any360/sys/config.h"
#include "any360/sys/variables.h"
#include "any360/sys/xam.h"

namespace any360::sys {

uint32_t Win32LastError(Guest g, uint32_t kthread) { return kthread ? g.Load32(kthread + kKThreadLastError) : 0; }

void SetWin32LastError(Guest g, uint32_t kthread, uint32_t error) {
  if (kthread) g.Store32(kthread + kKThreadLastError, error);
}

uint32_t SetWin32LastNtError(Guest g, uint32_t kthread, uint32_t status) {
  const uint32_t error = RtlNtStatusToDosError(status);
  SetWin32LastError(g, kthread, error);
  return error;
}

std::optional<int64_t> Win32Timeout(uint32_t milliseconds) {
  if (milliseconds == kInfinite) return std::nullopt;
  return -int64_t(10000) * int64_t(milliseconds);
}

uint32_t Win32WaitResult(Guest g, uint32_t kthread, uint32_t status) {
  // Success and information statuses are the documented results themselves (STATUS_WAIT_0 + n = WAIT_OBJECT_0 + n,
  // STATUS_ABANDONED_WAIT_0 + n = WAIT_ABANDONED_0 + n, STATUS_USER_APC = WAIT_IO_COMPLETION, STATUS_TIMEOUT =
  // WAIT_TIMEOUT); warnings and errors fail the call.
  if (status < 0x80000000u) return status;
  SetWin32LastNtError(g, kthread, status);
  return kWaitFailed;
}

uint32_t Win32SleepResult(uint32_t status) { return status == 0 ? 0 : kWaitIoCompletion; }

uint32_t LstrlenW(Guest g, uint32_t string) {
  if (!string) return 0;
  // A zero UTF-16 unit is zero in either byte order, so the units are scanned as they lie. Short strings (most a title
  // measures) end within the first units; longer ones go to the host's own scan of 16-bit units: Windows' wcslen
  // (wchar_t is 16-bit there), the C++ library's elsewhere. Those step over whole aligned units, so an odd address
  // (never a compiler's, possible by hand) stays on the unit-by-unit loop.
  const uint8_t* units = g.host(string);
  constexpr uint32_t kInline = 8;
  for (uint32_t i = 0; i < kInline; ++i) {
    uint16_t unit;
    std::memcpy(&unit, units + 2 * i, 2);
    if (unit == 0) return i;
  }
  if (string & 1) {
    uint32_t length = kInline;
    while (g.Load16(string + 2 * length) != 0) ++length;
    return length;
  }
#if defined(_WIN32)
  static_assert(sizeof(wchar_t) == 2);
  return kInline + uint32_t(std::wcslen(reinterpret_cast<const wchar_t*>(units) + kInline));
#else
  return kInline + uint32_t(std::char_traits<char16_t>::length(reinterpret_cast<const char16_t*>(units) + kInline));
#endif
}

uint32_t TickCount() { return UptimeMilliseconds(); }

uint32_t XGetOverlappedExtendedError(Guest g, uint32_t kthread, uint32_t overlapped) {
  if (!overlapped) return Win32LastError(g, kthread);
  constexpr uint32_t kErrorIoIncomplete = 0x3E4;
  if (g.Load32(overlapped) == kErrorIoPending) return kErrorIoIncomplete;
  return g.Load32(overlapped + 0x18);
}

}  // namespace any360::sys
