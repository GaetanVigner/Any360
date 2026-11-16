// Thread stacks for diagnostics (any360/host/thread_stacks.h), after the title hang watchdog's capture.
#include "any360/host/thread_stacks.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#endif

namespace any360::host::diagnostics {

#if defined(_WIN32)
namespace {

bool Readable(DWORD64 address) {
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY;
  return info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & PAGE_GUARD) &&
         address + 64 <= reinterpret_cast<DWORD64>(info.BaseAddress) + info.RegionSize;
}

// A suspended thread's context into return addresses. Allocation-free: the thread may hold the heap lock.
size_t Unwind(CONTEXT context, uint64_t* frames) {
  size_t count = 0;
  while (count < kMaxStackFrames && context.Rip != 0) {
    frames[count++] = context.Rip;
    if (!Readable(context.Rsp)) break;
    const DWORD64 rsp = context.Rsp;
    DWORD64 image_base = 0;
    if (PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)) {
      void* handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data, &frame, nullptr);
    } else {
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);  // a leaf: the return address on top
      context.Rsp += 8;
    }
    if (context.Rsp <= rsp) break;
  }
  return count;
}

std::mutex g_symbols;

}  // namespace

std::vector<uint32_t> ProcessThreads() {
  std::vector<uint32_t> ids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return ids;
  const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) ids.push_back(entry.th32ThreadID);
  }
  CloseHandle(snapshot);
  return ids;
}

ThreadHandle::ThreadHandle(uint32_t id) : id_(id) {
  handle_ = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
}

ThreadHandle::~ThreadHandle() {
  if (handle_) CloseHandle(handle_);
}

bool ThreadHandle::Capture(ThreadStack& out) const {
  out.id = id_, out.count = 0;
  if (!handle_ || SuspendThread(handle_) == DWORD(-1)) return false;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_FULL;
  const bool read = GetThreadContext(handle_, &context) != FALSE;
  if (read) out.count = Unwind(context, out.frames);
  ResumeThread(handle_);
  return read;
}

uint64_t ThreadHandle::Cycles() const {
  ULONG64 cycles = 0;
  if (handle_) QueryThreadCycleTime(handle_, &cycles);
  return cycles;
}

std::string ThreadHandle::Name() const {
  std::string name;
  PWSTR description = nullptr;
  if (handle_ && SUCCEEDED(GetThreadDescription(handle_, &description)) && description) {
    for (const wchar_t* c = description; *c; ++c) name += *c < 128 ? char(*c) : '?';
    LocalFree(description);
  }
  return name;
}

bool InExecutable(uint64_t address) {
  static const auto range = [] {
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info));
    const uint64_t begin = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
    return std::pair<uint64_t, uint64_t>(begin, begin + info.SizeOfImage);
  }();
  return address >= range.first && address < range.second;
}

uint64_t FunctionStart(uint64_t address) {
  DWORD64 image_base = 0;
  if (PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(address, &image_base, nullptr)) return image_base + entry->BeginAddress;
  return address;
}

std::string Describe(uint64_t address) {
  std::lock_guard lock(g_symbols);
  HANDLE process = GetCurrentProcess();
  static const bool symbols = [&] {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool ok = SymInitializeW(process, nullptr, TRUE) != FALSE;
    if (ok) SymRefreshModuleList(process);
    return ok;
  }();
  char hex[32];
  std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(address));
  std::string text = hex;
  if (!symbols) return text;
  IMAGEHLP_MODULE64 module{};
  module.SizeOfStruct = sizeof(module);
  if (SymGetModuleInfo64(process, address, &module)) text += std::string(" ") + module.ModuleName;
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol)) {
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llX", static_cast<unsigned long long>(displacement));
    text += "!" + std::string(symbol->Name, std::min<size_t>(symbol->NameLen, 511)) + offset;
  }
  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD column = 0;
  if (SymGetLineFromAddr64(process, address, &column, &line)) {
    text += " (" + std::filesystem::path(line.FileName).filename().string() + ":" + std::to_string(line.LineNumber) + ")";
  }
  return text;
}

#else

std::vector<uint32_t> ProcessThreads() { return {}; }
ThreadHandle::ThreadHandle(uint32_t id) : id_(id) {}
ThreadHandle::~ThreadHandle() = default;
bool ThreadHandle::Capture(ThreadStack& out) const {
  out.count = 0;
  return false;
}
uint64_t ThreadHandle::Cycles() const { return 0; }
std::string ThreadHandle::Name() const { return {}; }
bool InExecutable(uint64_t) { return false; }
uint64_t FunctionStart(uint64_t address) { return address; }
std::string Describe(uint64_t address) { return std::to_string(address); }

#endif

}  // namespace any360::host::diagnostics
