#include "any360/sys/win32.h"

#include "any360/sys/config.h"
#include "any360/sys/variables.h"

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
  uint32_t length = 0;
  while (g.Load16(string + 2 * length) != 0) ++length;
  return length;
}

uint32_t TickCount() { return UptimeMilliseconds(); }

}  // namespace any360::sys
