// The bench and verify modes' two sides (src/xdk_sides.h), on a stand-in function: an original that leaves r3 at 0
// and a replacement slower than ten times the original, so that the bench runs it once more. Every run of the
// replacement must get the call's entry registers (a rerun on the registers a side left read guest address 0 in
// Crackdown's scissor), and the bytes of the side that stood must be what memory keeps.
// Run: host_xdk_sides_tests
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/xdk_sides.h"

namespace {

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("FAILED line %d: %s\n", __LINE__, #condition);  \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace any360::host::rexglue::xdk;

constexpr uint32_t kAt = 0x40000100u, kEntryR3 = kAt;
int g_native_runs = 0;
std::vector<uint64_t> g_native_r3;

void Original(PPCContext& ctx, uint8_t*) {
  *g_memory.Host(kAt) = 0x11;
  ctx.r3.u64 = 0;
}

}  // namespace

int main() {
  std::vector<uint8_t> memory(0x1000);
  g_memory.base = memory.data() - 0x40000000u;
  g_memory.physical_offset = 0;

  Sides s;
  s.f = uint32_t(Function::kMemset);
  s.writes = {{kAt, 4}};
  s.work = 4;
  s.native = [](PPCContext& c) {
    ++g_native_runs;
    g_native_r3.push_back(c.r3.u64);
    *g_memory.Host(kAt) = g_native_runs == 1 ? 0x11 : 0xBB;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  };
  s.differs = [](const PPCContext& a, const PPCContext& b) -> const char* {
    return a.r3.u64 != b.r3.u64 ? "r3" : nullptr;
  };
  s.call = "stand-in";

  int reruns = 0;
  for (int call = 0; call < 8; ++call) {
    for (Mode mode : {Mode::kBench, Mode::kVerify}) {
      memory[0x100] = 0;
      g_native_runs = 0;
      g_native_r3.clear();
      PPCContext ctx{};
      ctx.r3.u64 = kEntryR3;
      ctx.r1.u64 = 0x40000800u;
      RunSides(mode, s, ctx, nullptr, &Original);
      for (uint64_t r3 : g_native_r3) CHECK(r3 == kEntryR3);
      CHECK(memory[0x100] == 0x11);
      if (mode == Mode::kBench && g_native_runs > 1) ++reruns;
      if (mode == Mode::kVerify) CHECK(ctx.r3.u64 == 0);
    }
  }
  CHECK(reruns > 0);
  std::printf("host_xdk_sides_tests: %d bench calls ran the replacement again, %d failing checks\n", reruns, g_failures);
  return g_failures ? 1 : 0;
}
