// Guest memory as a renderer watches it, for the tests: pages made read-only and a fault handler that opens a page
// when code writes to it, counting the faults. Kept apart from the tests' own file so the platform headers (windows.h
// and its CreateFile and CreateEvent macros) stay out of it.
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#endif

namespace any360::sys::test {

namespace {

constexpr size_t kPage = 0x1000;
uint8_t* g_region = nullptr;
size_t g_size = 0;
int g_faults = 0;

#ifdef _WIN32
void* g_handler = nullptr;

LONG CALLBACK OnFault(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  uint8_t* at = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
  if (!g_region || at < g_region || at >= g_region + g_size) return EXCEPTION_CONTINUE_SEARCH;
  DWORD before = 0;
  VirtualProtect(g_region + (size_t(at - g_region) & ~(kPage - 1)), kPage, PAGE_READWRITE, &before);
  ++g_faults;
  return EXCEPTION_CONTINUE_EXECUTION;
}
#else
struct sigaction g_before;

void OnFault(int, siginfo_t* info, void*) {
  uint8_t* at = static_cast<uint8_t*>(info->si_addr);
  if (!g_region || at < g_region || at >= g_region + g_size) {
    sigaction(SIGSEGV, &g_before, nullptr);  // not ours: the default takes it at the instruction's second try
    return;
  }
  mprotect(g_region + (size_t(at - g_region) & ~(kPage - 1)), kPage, PROT_READ | PROT_WRITE);
  ++g_faults;
}
#endif

}  // namespace

// `size` bytes of zeroed memory on page boundaries, writes to its watched pages handled from here on.
uint8_t* WatchedMemoryCreate(size_t size) {
  g_size = size;
  g_faults = 0;
#ifdef _WIN32
  g_region = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  g_handler = AddVectoredExceptionHandler(1, &OnFault);
#else
  g_region = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  struct sigaction action {};
  action.sa_sigaction = &OnFault;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, &g_before);
#endif
  return g_region;
}

// The page at `offset` becomes read-only until code writes to it; the count of faults starts again.
void WatchedMemoryWatch(size_t offset) {
#ifdef _WIN32
  DWORD before = 0;
  VirtualProtect(g_region + offset, kPage, PAGE_READONLY, &before);
#else
  mprotect(g_region + offset, kPage, PROT_READ);
#endif
  g_faults = 0;
}

int WatchedMemoryFaults() { return g_faults; }

void WatchedMemoryDestroy() {
#ifdef _WIN32
  RemoveVectoredExceptionHandler(g_handler);
  VirtualFree(g_region, 0, MEM_RELEASE);
#else
  sigaction(SIGSEGV, &g_before, nullptr);
  munmap(g_region, g_size);
#endif
  g_region = nullptr;
}

}  // namespace any360::sys::test
