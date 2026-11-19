// xam.xex's Win32 forms of kernel calls (step 12, docs/SYS_STUBS.md: implemented by Xenia Canary, stubbed by the
// reference): Sleep / SleepEx, WaitForSingleObject(Ex), CreateThread, ResumeThread, ExitThread, GetCurrentThreadId,
// GetLastError, GetModuleHandleA, GetTickCount, QueryPerformanceFrequency, GetSystemTimeAsFileTime, lstrlenW. Each is
// a thin form over a kernel call any360::sys already answers (KeDelayExecutionThread, NtWaitForSingleObjectEx,
// ExCreateThread, NtResumeThread, ExTerminateThread, XexGetModuleHandle, KeQueryPerformanceFrequency,
// KeQuerySystemTime, KeTimeStampBundle's tick count, RtlNtStatusToDosError); what is Win32's own is here: the
// thread's last error (the KTHREAD's LastWin32ErrorCode at +0x160, its thread id at +0x14C, as Xenia lays the KTHREAD
// out), the millisecond timeouts, and the wait results as Microsoft documents WaitForSingleObjectEx. Xenia's
// WaitForSingleObjectEx always returns WAIT_FAILED (marked as a known shortcut in its source); the documented results
// are returned here instead.
#pragma once

#include <cstdint>
#include <optional>

#include "guest.h"

namespace any360::sys {

constexpr uint32_t kKThreadId = 0x14C, kKThreadLastError = 0x160;
constexpr uint32_t kInfinite = 0xFFFFFFFF, kWaitFailed = 0xFFFFFFFF, kWaitIoCompletion = 0xC0;

// The calling thread's last Win32 error (its KTHREAD), and setting it.
uint32_t Win32LastError(Guest g, uint32_t kthread);
void SetWin32LastError(Guest g, uint32_t kthread, uint32_t error);
// RtlSetLastNTError: the NTSTATUS turned into a Win32 error, stored as the last error and returned.
uint32_t SetWin32LastNtError(Guest g, uint32_t kthread, uint32_t status);

// A Win32 timeout in milliseconds as a kernel interval (100 ns, negative: relative); none for INFINITE.
std::optional<int64_t> Win32Timeout(uint32_t milliseconds);

// WaitForSingleObjectEx's result from the kernel wait's status: WAIT_OBJECT_0 + n, WAIT_ABANDONED_0 + n,
// WAIT_TIMEOUT, WAIT_IO_COMPLETION as the status gives them; an error status sets the last error and gives
// WAIT_FAILED.
uint32_t Win32WaitResult(Guest g, uint32_t kthread, uint32_t status);

// SleepEx's result from the kernel delay's status: 0, or WAIT_IO_COMPLETION when an APC ended it.
uint32_t Win32SleepResult(uint32_t status);

// lstrlenW: the length of a big-endian UTF-16 guest string in characters; 0 for a null pointer.
uint32_t LstrlenW(Guest g, uint32_t string);

// GetTickCount: milliseconds since the layer started, the count KeTimeStampBundle carries.
uint32_t TickCount();
// XGetOverlappedExtendedError: an XOVERLAPPED's extended error (+0x18); ERROR_IO_INCOMPLETE while its result is
// still ERROR_IO_PENDING; with no XOVERLAPPED, the thread's last error.
uint32_t XGetOverlappedExtendedError(Guest g, uint32_t kthread, uint32_t overlapped);

// Step 12, eighth batch (Xenia's xam_info.cc).
// XapiFormatTimeOut (interval out, milliseconds): the timeout as a relative kernel interval (big-endian, 100 ns,
// negative) written at `interval` and that address returned, the pointer a kernel wait takes; INFINITE writes nothing
// and returns 0 (no timeout). Xenia returns the host address of its buffer: the guest's is returned here.
uint32_t XapiFormatTimeOut(Guest g, uint32_t interval, uint32_t milliseconds);
// XapipCreateThread's creation flags for ExCreateThread: CREATE_SUSPENDED (4) is the kernel's suspended flag (1), a
// processor (0-5 on the console) its affinity bit at 24 + processor; -1 (CreateThread) or any processor past the
// affinity byte (8 or more, which Xenia's shift overflows) asks for none.
uint32_t XapiThreadFlags(uint32_t creation_flags, uint32_t processor);

}  // namespace any360::sys
