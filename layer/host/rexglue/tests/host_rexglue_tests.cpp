// The graphics binding inside a running reference runtime (ReXGlue, tool mode: memory and kernel, no GPU): this test
// is the "title". It overrides the Vd* exports as a title does (ANY360_REXGLUE_GRAPHICS_EXPORTS), checks that before
// the binding they reach the runtime's own exports, registers the GPU MMIO range the runtime's graphics system
// would have (a placeholder that must never be reached afterwards), then binds and drives a ring the way a title
// does: Vd* calls for the ring, the read-pointer write-back and the interrupt callback, packets in guest memory, and
// the write pointer stored through the MMIO handler. The command processor must run the ring, write the read pointer
// back, and the title's callback (a guest function) must run on the runtime's thread for the ring's INTERRUPT
// (source 1) and for vertical blanks (source 0).
// Run: host_rexglue_tests [--gpu NAME]
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
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

#include "any360/host/rexglue.h"

ANY360_REXGLUE_GRAPHICS_EXPORTS()
ANY360_REXGLUE_BIND_GRAPHICS(VdSwap)

namespace {

constexpr uint32_t kEntry = 0x82000000, kCallback = 0x82000100;
constexpr uint32_t kCpRbWptr = 0x7FC80000 + 0x01C5 * 4;

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

std::string g_gpu;
std::atomic<bool> g_done{false};
std::atomic<int> g_interrupts[2] = {0, 0};
std::atomic<uint32_t> g_user_data{0}, g_placeholder_writes{0};
std::atomic<bool> g_on_runtime_thread{true};

uint32_t PlaceholderRead(void*, void*, uint32_t) { return 0; }
void PlaceholderWrite(void*, void*, uint32_t, uint32_t) { ++g_placeholder_writes; }

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

// The title's graphics interrupt callback: r3 the source, r4 its user data.
void Callback(PPCContext& ctx, uint8_t*) {
  if (!rex::system::XThread::GetCurrentThread()) g_on_runtime_thread = false;
  g_user_data = ctx.r4.u32;
  if (ctx.r3.u32 < 2) ++g_interrupts[ctx.r3.u32];
}

void Entry(PPCContext& ctx, uint8_t* base) {
  auto* runtime = rex::Runtime::instance();
  rex::memory::Memory* memory = runtime->memory();
  using namespace any360::host::rexglue;

  // Before the binding: the overrides reach the runtime's exports, found in its library by name.
  CHECK(!GraphicsBound());
  CHECK(ReferenceExport("__imp__VdSwap") != nullptr && ReferenceExport("__imp__NoSuchExport") == nullptr);
  const uint32_t scratch = memory->SystemHeapAlloc(0x200, 0x100);
  ctx.r3.u64 = scratch;
  ctx.r4.u64 = scratch + 0x100;
  __imp__VdGetSystemCommandBuffer(ctx, base);
  CHECK(Load32(base, scratch) == 0xBEEF0000 && Load32(base, scratch + 0x100) == 0xBEEF0001);

  // The binding.
  GraphicsOptions options;
  options.gpu = g_gpu;
  options.width = 320, options.height = 180;
  options.vblank_hz = 200.0;
  std::string error;
  if (!StartGraphics(options, error)) {
    std::printf("host_rexglue_tests: StartGraphics: %s\n", error.c_str());
    ++g_failures;
    g_done = true;
    return;
  }
  CHECK(GraphicsBound());

  // The title's side: a 64 KB ring and the write-back word in physical memory, the callback, then an INTERRUPT
  // packet for CPU 2 and padding, kicked through the GPU MMIO.
  const uint32_t ring = memory->SystemHeapAlloc(0x10000, 0x10000, rex::memory::kSystemHeapPhysical);
  const uint32_t write_back = memory->SystemHeapAlloc(0x100, 0x100, rex::memory::kSystemHeapPhysical);
  std::printf("host_rexglue_tests: ring %08X, write-back %08X\n", ring, write_back);
  ctx.r3.u64 = ring;
  ctx.r4.u64 = 16;
  __imp__VdInitializeRingBuffer(ctx, base);
  ctx.r3.u64 = write_back;
  ctx.r4.u64 = 2;
  __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);
  ctx.r3.u64 = kCallback;
  ctx.r4.u64 = 0x1234;
  __imp__VdSetGraphicsInterruptCallback(ctx, base);
  uint32_t at = 0;
  Store32(base, ring + 4 * at++, 3u << 30 | 0u << 16 | 0x54u << 8);  // INTERRUPT, one dword: the CPU mask
  Store32(base, ring + 4 * at++, 1u << 2);
  while (at < 8) Store32(base, ring + 4 * at++, 2u << 30);  // type-2 padding
  rex::runtime::MMIOHandler::global_handler()->CheckStore(kCpRbWptr, at);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((g_interrupts[1] < 1 || g_interrupts[0] < 5 || Load32(base, write_back) == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  uint32_t read_back = 0;
  std::memcpy(&read_back, base + write_back, 4);
  std::printf("host_rexglue_tests: %d ring interrupts, %d vertical blanks, user data %04X, read pointer written back "
              "%08X (raw), placeholder MMIO writes %u\n",
              g_interrupts[1].load(), g_interrupts[0].load(), g_user_data.load(), read_back, g_placeholder_writes.load());
  CHECK(g_interrupts[1] == 1 && g_interrupts[0] >= 5 && g_user_data == 0x1234 && g_on_runtime_thread);
  CHECK(read_back == at || __builtin_bswap32(read_back) == at);
  CHECK(g_placeholder_writes == 0);
  StopGraphics();
  g_done = true;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--gpu")) g_gpu = argv[++i];
  }
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "any360-host-rexglue-tests";
  std::filesystem::create_directories(root);
  rex::Runtime runtime(root);
  rex::RuntimeConfig config;
  config.tool_mode = true;
  if (runtime.Setup(std::move(config)) != 0) {
    std::printf("host_rexglue_tests: the reference runtime did not set up\n");
    return 2;
  }
  // The GPU MMIO range the runtime's graphics system registers (none in tool mode): a placeholder the binding takes.
  runtime.memory()->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0xFFFF, nullptr,
                                          reinterpret_cast<rex::runtime::MMIOReadCallback>(&PlaceholderRead),
                                          reinterpret_cast<rex::runtime::MMIOWriteCallback>(&PlaceholderWrite));
  auto* dispatcher = runtime.function_dispatcher();
  if (!dispatcher->InitializeFunctionTable(kEntry, 0x10000, kEntry, 0x20000, true) || !dispatcher->SetFunction(kEntry, &Entry) ||
      !dispatcher->SetFunction(kCallback, &Callback)) {
    std::printf("host_rexglue_tests: cannot register the guest functions\n");
    return 2;
  }
  auto thread = rex::system::make_object<rex::system::XThread>(runtime.kernel_state(), 256 * 1024, 0, kEntry, 0, 0, true, true);
  if (thread->Create() != 0) {
    std::printf("host_rexglue_tests: cannot create the guest thread\n");
    return 2;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  while (!g_done && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!g_done) ++g_failures;
  std::printf("host_rexglue_tests: %d failing checks\n", g_failures);
  std::fflush(stdout);
  std::_Exit(g_failures ? 1 : 0);
}
