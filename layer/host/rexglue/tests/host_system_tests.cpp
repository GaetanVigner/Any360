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
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "any360/host/system.h"
#include "any360/sys/scheduler.h"
#include "any360/host/system_exports.inc"

namespace {

using namespace any360::host::rexglue;

constexpr uint32_t kEntry = 0x82000000, kSum = 0x82000100;

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

bool AllFd(const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] != 0xFD) return false;
  }
  return true;
}

void Entry(PPCContext& ctx, uint8_t* base) {
  std::string error;
  int traced = 0;
  CHECK(StartSystem({16 << 20, [&](const std::string&) { ++traced; }, true}, error));
  SysGroup group;
  CHECK(ParseSysGroup("threads", group) && group == SysGroup::kThreads && !ParseSysGroup("gpu", group));
  CHECK(VariableGroup("ExEventObjectType", group) && group == SysGroup::kObjects && !VariableGroup("NoSuchVariable", group));
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
      !dispatcher->SetFunction(kSum, &Sum)) {
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
