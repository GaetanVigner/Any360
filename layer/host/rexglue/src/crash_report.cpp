// An unhandled exception's report in the title's log (code, address, module and offset of each frame), for titles that
// have no crash reporter of their own: installed with the system binding, chained to the filter it replaces.
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

LONG WINAPI Report(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  REXLOG_ERROR("Any360 crash report: exception {:08X} at {}", record->ExceptionCode, Where(record->ExceptionAddress));
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    REXLOG_ERROR("Any360 crash report: {} of {:016X}", record->ExceptionInformation[0] ? "write" : "read",
                 record->ExceptionInformation[1]);
  }
  void* frames[48];
  const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) REXLOG_ERROR("Any360 crash report:   #{:02} {}", i, Where(frames[i]));
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
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
