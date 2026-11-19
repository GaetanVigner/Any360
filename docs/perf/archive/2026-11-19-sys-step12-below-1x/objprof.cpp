#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "any360/sys/files.h"
#include "any360/sys/alloc_meter.h"
#include <filesystem>
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"
using namespace any360::sys;
static std::vector<uint8_t> mem(1u << 24);
template <class F> double T(int n, F f) {
  double best = 1e9;
  for (int r = 0; r < 9; ++r) {
    auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) f();
    auto b = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count() / n);
  }
  return best;
}
int main() {
  Guest g{mem.data()};
  Objects().SetKernelHeap(0x80000, 0x8000);
  Objects().SetGuest(g);
  Objects().Reset();
  XamState();
  Files();
  Threads();
  const uint32_t out = 0x10000;
  std::printf("alloc+free (no handle)  %.1f\n", T(200000, [&] { uint32_t o = Objects().AllocateObject(16, true); Objects().FreeObject(o); }));
  std::printf("alloc+open+close        %.1f\n", T(200000, [&] { uint32_t o = Objects().AllocateObject(16, true); Objects().Close(Objects().OpenHandle(o)); }));
  std::printf("CreateEnumerator+close  %.1f\n", T(200000, [&] { XamState().ProfileCreateEnumerator(g, out); Objects().Close(__builtin_bswap32(*(uint32_t*)(mem.data() + out))); }));
  std::printf("TitlesPlayed+close      %.1f\n", T(200000, [&] { XamState().UserCreateTitlesPlayedEnumerator(g, 0, 5, out + 4, out); Objects().Close(__builtin_bswap32(*(uint32_t*)(mem.data() + out))); }));
  std::recursive_mutex rm; std::mutex m;
  std::printf("recursive_mutex lock    %.1f\n", T(2000000, [&] { rm.lock(); rm.unlock(); }));
  std::printf("mutex lock              %.1f\n", T(2000000, [&] { m.lock(); m.unlock(); }));
  std::printf("Files().OnClose         %.1f\n", T(2000000, [&] { Files().OnClose(0x12345670); }));

  XamState().title().content_root = std::filesystem::temp_directory_path() / "any360-objprof";
  const uint64_t xuid = XamState().title().xuid;
  std::memcpy(mem.data() + 0x20000, "BENCHPROF", 10);
  auto meter = [&](const char* name, auto f) {
    f();
    const auto a = ThreadAllocations();
    f();
    const auto b = ThreadAllocations();
    std::printf("%-24s %.1f ns, %llu allocs, %llu B\n", name, T(20000, f), (unsigned long long)(b.count - a.count), (unsigned long long)(b.bytes - a.bytes));
  };
  meter("open+close", [&] { XamState().ProfileOpen(g, xuid, 0x20000, 0); XamState().ProfileClose(g, 0x20000); });
  XamState().ProfileOpen(g, xuid, 0x20000, 0);
  meter("open again (mounted)", [&] { XamState().ProfileOpen(g, xuid, 0x20000, 0); });
  meter("Mount+Unmount", [&] { Files().Mount("\Device\Content\BENCHPROF", XamState().title().content_root, false); Files().Unmount("\Device\Content\BENCHPROF"); });
  meter("Link+Unlink", [&] { Files().Link("BENCHPROF:", "\Device\Content\BENCHPROF"); Files().Unlink("BENCHPROF:"); });
}
