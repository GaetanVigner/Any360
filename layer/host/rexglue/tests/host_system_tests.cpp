// The system binding inside a running reference runtime (ReXGlue, tool mode): this test is the "title". It takes
// every override (system_exports.inc), then on a guest thread checks that a group left off reaches the runtime's own
// export and a group switched on reaches any360::sys's, through calls whose answers differ on the two sides
// (XeCryptRandom: 0xFD bytes in the runtime, random in Any360; XamUserIsOnlineEnabled: 1 in the runtime, 0 offline in
// Any360). It also runs a guest function through the dispatcher any360::sys's scheduler gets.
// Run: host_system_tests
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "any360/host/system.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/host/system_exports.inc"

namespace {

using namespace any360::host::rexglue;

constexpr uint32_t kEntry = 0x82000000, kSum = 0x82000100, kInThread = 0x82000300;

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

std::atomic<bool> g_done{false};
rex::Runtime* g_runtime = nullptr;

// Guest code: the sum of its eight arguments, checked against the stack and PCR it was given.
void Sum(PPCContext& ctx, uint8_t*) {
  CHECK(ctx.r1.u32 == 0x12340000 && ctx.r13.u32 == 0x56780000);
  ctx.r3.u64 = ctx.r3.u64 + ctx.r4.u64 + ctx.r5.u64 + ctx.r6.u64 + ctx.r7.u64 + ctx.r8.u64 + ctx.r9.u64 + ctx.r10.u64;
}

// Guest code on a thread any360::sys made: whether the runtime sees a thread of its own there (its calls need one).
std::atomic<int> g_in_runtime_thread{-1};
void InThread(PPCContext&, uint8_t*) { g_in_runtime_thread = rex::system::XThread::IsInThread() ? 1 : 0; }

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

bool AllFd(const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] != 0xFD) return false;
  }
  return true;
}

void Entry(PPCContext& ctx, uint8_t* base) {
  std::string error;
  int traced = 0;
  const std::filesystem::path files = std::filesystem::temp_directory_path() / "any360-host-system-files";
  std::filesystem::create_directories(files);
  { std::ofstream(files / "data.bin", std::ios::binary) << "1234"; }
  SystemOptions options;
  options.kernel_heap_size = 16 << 20;
  options.log = [&](const std::string&) { ++traced; };
  options.trace_calls = true;
  options.mounts.push_back({"\\Device\\Harddisk0\\Partition1", files.string(), true, {"game:"}});
  CHECK(StartSystem(options, error));
  SysGroup group;
  CHECK(ParseSysGroup("threads", group) && group == SysGroup::kThreads && !ParseSysGroup("gpu", group));
  CHECK(VariableGroup("KeTimeStampBundle", group) && group == SysGroup::kThreads && !VariableGroup("ExEventObjectType", group));
  CHECK(BindVariables() == 0);  // no executable module in this runtime
  const uint32_t buffer = g_runtime->memory()->SystemHeapAlloc(64);
  PPCContext call = ctx;
  // Left off: the runtime's exports.
  call.r3.u64 = buffer;
  call.r4.u64 = 32;
  __imp__XeCryptRandom(call, base);
  CHECK(AllFd(base + buffer, 32));
  __imp__XamUserIsOnlineEnabled(call, base);
  CHECK(call.r3.u32 == 1);
  // Switched on: any360::sys's.
  EnableSystemGroup(SysGroup::kStateless, true);
  EnableSystemGroup(SysGroup::kXam, true);
  CHECK(SystemGroupEnabled(SysGroup::kStateless) && !SystemGroupEnabled(SysGroup::kFiles));
  call.r3.u64 = buffer;
  call.r4.u64 = 32;
  __imp__XeCryptRandom(call, base);
  CHECK(!AllFd(base + buffer, 32));
  __imp__XamUserIsOnlineEnabled(call, base);
  CHECK(call.r3.u32 == 0);
  __imp__XamUserIsOnlineEnabled(call, base);
  CHECK(traced == 2);  // XeCryptRandom and XamUserIsOnlineEnabled, once each
  EnableSystemGroup(SysGroup::kStateless, false);
  EnableSystemGroup(SysGroup::kXam, false);
  // Handles go back to the side that made them: an event made by any360::sys (its range starts at 0xF8100000) is closed
  // by any360::sys through the runtime's NtClose override, the runtime's own events by the runtime.
  const uint32_t out = g_runtime->memory()->SystemHeapAlloc(16);
  CHECK(any360::sys::Objects().CreateEvent(any360::sys::Guest{base}, out, 0, 0, 0) == 0);
  const uint32_t ours = Load32(base, out);
  CHECK(ours >= 0xF8100000 && any360::sys::Objects().Lookup(ours) != 0);
  call.r3.u64 = ours;
  __imp__NtClose(call, base);
  CHECK(call.r3.u32 == 0 && any360::sys::Objects().Lookup(ours) == 0);
  call.r3.u64 = out;  // (handle out, attributes, type, initial state)
  call.r4.u64 = 0;
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  __imp__NtCreateEvent(call, base);  // objects off: the runtime's
  const uint32_t theirs = Load32(base, out);
  CHECK(call.r3.u32 == 0 && theirs < 0xF8100000 && !any360::sys::Objects().OwnsHandle(theirs));
  call.r3.u64 = theirs;
  __imp__NtClose(call, base);
  CHECK(call.r3.u32 == 0);
  // Paths: a device any360::sys mounted is its own with the files on; another one stays the runtime's.
  EnableSystemGroup(SysGroup::kFiles, true);
  const uint32_t path = g_runtime->memory()->SystemHeapAlloc(64);
  std::memcpy(base + path, "game:\\data.bin", 15);
  call.r3.u64 = path;
  call.r4.u64 = 0x80000000;
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  call.r7.u64 = 3;  // OPEN_EXISTING
  call.r8.u64 = 0;
  call.r9.u64 = 0;
  __imp__CreateFileA(call, base);
  const uint32_t file = call.r3.u32;
  CHECK(any360::sys::Objects().OwnsHandle(file) && any360::sys::Objects().Lookup(file) != 0);
  // A read on that file with an event the runtime made (the objects are the runtime's): the runtime's event is set.
  call.r3.u64 = out;
  call.r4.u64 = 0;
  call.r5.u64 = 0;  // notification event, not signaled
  call.r6.u64 = 0;
  __imp__NtCreateEvent(call, base);
  const uint32_t event = Load32(base, out);
  const uint32_t io = g_runtime->memory()->SystemHeapAlloc(16);
  call.r3.u64 = file;  // (file, event, APC routine, APC context, io status, buffer, length, offset)
  call.r4.u64 = event;
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  call.r7.u64 = io;
  call.r8.u64 = buffer;
  call.r9.u64 = 4;
  call.r10.u64 = 0;
  __imp__NtReadFile(call, base);
  CHECK(call.r3.u32 == 0 && Load32(base, io + 4) == 4 && std::memcmp(base + buffer, "1234", 4) == 0);
  auto runtime_event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event);
  uint64_t no_wait = 0;
  CHECK(runtime_event && runtime_event->Wait(0, 0, 0, &no_wait) == 0);  // signaled (not STATUS_TIMEOUT)
  call.r3.u64 = event;
  __imp__NtClose(call, base);
  // A read into physical memory (0xE0000000 up, mapped 0x1000 further on Windows): the bytes land where the runtime
  // and the title's code look.
  const uint32_t physical = g_runtime->memory()->SystemHeapAlloc(0x1000, 0x1000, rex::memory::kSystemHeapPhysical);
  CHECK(physical >= 0xE0000000);
  std::memset(g_runtime->memory()->TranslateVirtual<uint8_t*>(physical), 0, 8);
  const uint32_t at = g_runtime->memory()->SystemHeapAlloc(8);
  std::memset(base + at, 0, 8);  // offset 0
  call.r3.u64 = file;
  call.r4.u64 = 0;
  call.r7.u64 = io;
  call.r8.u64 = physical;
  call.r9.u64 = 4;
  call.r10.u64 = at;
  __imp__NtReadFile(call, base);
  CHECK(call.r3.u32 == 0 && std::memcmp(g_runtime->memory()->TranslateVirtual<uint8_t*>(physical), "1234", 4) == 0);
  call.r3.u64 = file;
  __imp__CloseHandle(call, base);
  CHECK(call.r3.u32 == 1 && any360::sys::Objects().Lookup(file) == 0);
  std::memcpy(base + path, "nowhere:\\data.bin", 18);
  call.r3.u64 = path;
  call.r4.u64 = 0x80000000;
  call.r7.u64 = 3;
  __imp__CreateFileA(call, base);
  CHECK(!any360::sys::Objects().OwnsHandle(call.r3.u32));  // the runtime's answer (no such device there either)
  EnableSystemGroup(SysGroup::kFiles, false);
  // A thread any360::sys creates runs inside one of the runtime's.
  {
    const uint32_t handle_out = g_runtime->memory()->SystemHeapAlloc(8);
    CHECK(any360::sys::Threads().CreateThread(any360::sys::Guest{base}, handle_out, 0, 0, 0, kInThread, 0, 0) == 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_in_runtime_thread < 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(g_in_runtime_thread == 1);
    // One with no handle frees itself as it ends, from the runtime's thread it ran on (it joined itself before). The
    // object calls after it would wait for ever on the lock it held.
    g_in_runtime_thread = -1;
    CHECK(any360::sys::Threads().CreateThread(any360::sys::Guest{base}, 0, 0, 0, 0, kInThread, 0, 0) == 0);
    while (g_in_runtime_thread < 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(g_in_runtime_thread == 1 && any360::sys::Objects().Lookup(Load32(base, handle_out)) != 0);
  }
  // One address space, two allocators: what any360::sys hands out is the runtime's too (allocated there), what the
  // runtime had is never handed out, and a release here releases there.
  {
    auto* memory = g_runtime->memory();
    const any360::sys::Guest g{base};
    const uint32_t base_ptr = memory->SystemHeapAlloc(8), size_ptr = base_ptr + 4;
    g.Store32(base_ptr, 0);
    g.Store32(size_ptr, 0x20000);
    CHECK(any360::sys::Memory().AllocateVirtual(g, base_ptr, size_ptr, 0x3000, 4, 0) == 0);  // reserve | commit, read-write
    const uint32_t ours = g.Load32(base_ptr);
    rex::memory::HeapAllocationInfo info{};
    CHECK(ours && memory->LookupHeap(ours)->QueryRegionInfo(ours, &info) && info.state != 0);
    // The runtime's own allocation lands elsewhere, and any360::sys's next one avoids it.
    const uint32_t theirs = memory->SystemHeapAlloc(0x10000);
    CHECK(theirs && (theirs + 0x10000 <= ours || theirs >= ours + 0x20000));
    g.Store32(size_ptr, 0);
    CHECK(any360::sys::Memory().FreeVirtual(g, base_ptr, size_ptr, 0x8000, 0) == 0);  // release
    CHECK(memory->LookupHeap(ours)->QueryRegionInfo(ours, &info) && info.state == 0);
    // Physical memory the same way.
    const uint32_t physical = any360::sys::Memory().AllocatePhysical(g, 0, 0x2000, 4, 0, 0xFFFFFFFF, 0x1000);
    CHECK(physical >= 0xA0000000 && memory->LookupHeap(physical)->QueryRegionInfo(physical, &info) && info.state != 0);
    any360::sys::Memory().FreePhysical(0, physical);
  }
  // Guest code through the dispatcher, eight arguments.
  CHECK(DispatchGuest(kSum, 0x12340000, 0x56780000, std::initializer_list<uint64_t>{1, 2, 3, 4, 5, 6, 7, 8}.begin(), 8, nullptr) == 36);
  CHECK(any360::sys::Threads().Dispatch(kSum, {0, 0x56780000, 0x12340000}, {10, 20}) == 30);
  CHECK(DispatchGuest(0x82000200, 0, 0, nullptr, 0, nullptr) == 0);  // no function there
  g_done = true;
}

}  // namespace

int main() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "any360-host-system-tests";
  std::filesystem::create_directories(root);
  rex::Runtime runtime(root);
  rex::RuntimeConfig config;
  config.tool_mode = true;
  if (runtime.Setup(std::move(config)) != 0) {
    std::printf("host_system_tests: the reference runtime did not set up\n");
    return 2;
  }
  g_runtime = &runtime;
  auto* dispatcher = runtime.function_dispatcher();
  if (!dispatcher->InitializeFunctionTable(kEntry, 0x10000, kEntry, 0x20000, true) || !dispatcher->SetFunction(kEntry, &Entry) ||
      !dispatcher->SetFunction(kSum, &Sum) || !dispatcher->SetFunction(kInThread, &InThread)) {
    std::printf("host_system_tests: cannot register the guest functions\n");
    return 2;
  }
  auto thread = rex::system::make_object<rex::system::XThread>(runtime.kernel_state(), 256 * 1024, 0, kEntry, 0, 0, true, true);
  if (thread->Create() != 0) {
    std::printf("host_system_tests: cannot create the guest thread\n");
    return 2;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (!g_done && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!g_done) ++g_failures;
  std::printf("host_system_tests: %d failing checks\n", g_failures);
  std::fflush(stdout);
  std::_Exit(g_failures ? 1 : 0);
}
