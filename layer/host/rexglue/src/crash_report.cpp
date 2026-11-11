// An unhandled exception's report in the title's log (code, address, module and offset of each frame), for titles that
// have no crash reporter of their own: installed with the system binding, chained to the filter it replaces. After
// the thread that crashed, where every other thread of the process is: a title's crash is often one thread reading
// what another has not made yet, or has just taken away, and the other one is then the half of it that the crashing
// thread's frames do not show. tools/crash_names.ps1 gives the names of a report's addresses.
#include <rex/logging.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace any360::host::rexglue {

#if defined(_WIN32)
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

std::string Where(const void* address) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &module) && module) {
    GetModuleFileNameA(module, name, sizeof name);
  }
  const char* base = std::strrchr(name, '\\');
  char text[MAX_PATH + 48];
  std::snprintf(text, sizeof text, "%016llX %s+0x%llX", static_cast<unsigned long long>(uintptr_t(address)),
                base ? base + 1 : name,
                static_cast<unsigned long long>(uintptr_t(address) - uintptr_t(module)));
  return text;
}

// "module+0xoffset", the short form of Where.
std::string Short(const void* address) {
  const std::string where = Where(address);
  const size_t space = where.find(' ');
  return space == std::string::npos ? where : where.substr(space + 1);
}

#if defined(_M_X64)
// Where each other thread is, one line a thread. A thread is stopped only while its frames are read into an array
// on this stack: nothing is allocated or logged meanwhile, since the stopped thread may hold the heap's lock or
// the log's.
void OtherThreads() {
  const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof entry;
  uint32_t threads = 0;
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    constexpr int kFrames = 20;
    DWORD64 frames[kFrames];
    int count = 0;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(thread, &context)) {
        while (count < kFrames && context.Rip) {
          frames[count++] = context.Rip;
          DWORD64 image = 0;
          const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
          if (function) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &establisher, nullptr);
          } else {
            // A leaf function, or code with no unwind data: the return address is at the stack pointer, if that
            // is memory at all.
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(context.Rsp), &memory, sizeof memory) || memory.State != MEM_COMMIT ||
                (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
              break;
            }
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
          }
        }
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
    std::string line;
    for (int i = 0; i < count; ++i) line += (i ? " < " : "") + Short(reinterpret_cast<void*>(frames[i]));
    REXLOG_ERROR("Any360 crash report: thread {}: {}", entry.th32ThreadID, line);
    ++threads;
  }
  CloseHandle(snapshot);
  REXLOG_ERROR("Any360 crash report: {} other threads", threads);
}
#else
void OtherThreads() {}
#endif

LONG WINAPI Report(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  REXLOG_ERROR("Any360 crash report: exception {:08X} at {} on thread {}", record->ExceptionCode, Where(record->ExceptionAddress),
               GetCurrentThreadId());
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    REXLOG_ERROR("Any360 crash report: {} of {:016X}", record->ExceptionInformation[0] ? "write" : "read",
                 record->ExceptionInformation[1]);
  }
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) REXLOG_ERROR("Any360 crash report:   #{:02} {}", i, Where(frames[i]));
  // The crashing thread's report is on disk before the other threads are touched.
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
  static bool others_done = false;  // once: a second crash while the first is reported adds nothing
  if (!others_done) {
    others_done = true;
    OtherThreads();
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashReport() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  g_previous = SetUnhandledExceptionFilter(&Report);
}
#else
void InstallCrashReport() {}
#endif

}  // namespace any360::host::rexglue
