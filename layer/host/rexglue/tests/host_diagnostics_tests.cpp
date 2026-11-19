// The hitch tracer's sampling (any360/host/hitch_tracer.h) on a thread that spins in a known function: the report must
// put that thread first (it used the cycles), name the function, and the sampling must not hang the sampled thread
// (nothing allocated while it is suspended). Then the function profiler's (any360/host/profiler.h) on a thread that
// spins under a function named as a title's (sub_8200ABCD) beside an idle one: the title function first, its own time
// most of the stretch, the spin among the host frames. Windows only; elsewhere nothing is captured and the test passes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "any360/host/hitch_tracer.h"
#include "any360/host/profiler.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace {

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

std::atomic<uint64_t> g_spins{0};

// The function the tracer must find: busy (and allocating, as a title's code may, to catch a sampler that would
// deadlock on the heap lock) for `ms` milliseconds.
__attribute__((noinline)) void SpinInKnownFunction(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    std::string s(64, char('a' + g_spins.load() % 26));
    g_spins += s.size();
  }
}

}  // namespace

// A title function as the recompiled code names them, spinning through the known function.
// (A frame of its own: work after the call keeps it from being a tail jump.)
extern "C" __attribute__((noinline)) void sub_8200ABCD(int ms) {
  SpinInKnownFunction(ms);
  g_spins.fetch_add(1);
}

int main() {
#if defined(_WIN32)
  namespace diag = any360::host::diagnostics;
  std::atomic<bool> go{false};
  std::thread spinner([&] {
    SetThreadDescription(GetCurrentThread(), L"spinner");
    while (!go) std::this_thread::yield();
    SpinInKnownFunction(120);
  });
  go = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  diag::HitchSampler sampler;
  const auto start = std::chrono::steady_clock::now();
  sampler.Refresh();
  sampler.Begin();
  for (int i = 0; i < 30; ++i) {
    sampler.Sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const std::string report = sampler.End(60.0);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  spinner.join();
  // The busiest thread is the spinner, and its samples are in the known function.
  const size_t first = report.find("\nThread "), second = report.find("\nThread ", first + 1);
  const std::string busiest = report.substr(first, second == std::string::npos ? std::string::npos : second - first);
  CHECK(first != std::string::npos);
  CHECK(busiest.find("\"spinner\"") != std::string::npos);
  CHECK(busiest.find("SpinInKnownFunction") != std::string::npos);
  CHECK(g_spins.load() > 0 && ms < 2000);  // the spinner ran on; the sampling did not stall
  std::printf("host_diagnostics_tests: sampled 30 rounds in %.0f ms; the busiest thread %s\n", ms,
              busiest.find("SpinInKnownFunction") != std::string::npos ? "found in SpinInKnownFunction" : "NOT found");
  if (g_failures) std::printf("%s\n", report.c_str());

  // The profiler: 100 rounds 2 ms apart over a thread spinning under sub_8200ABCD and one asleep.
  std::atomic<bool> go2{false};
  timeBeginPeriod(1);  // sleeps of 2 ms, not the default 15.6
  std::thread title([&] {
    while (!go2) std::this_thread::yield();
    sub_8200ABCD(400);
  });
  std::thread sleeper([] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); });
  go2 = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const double cycles_per_ms = diag::MeasureCyclesPerMs();
  diag::ProfileSampler profile;
  const auto pstart = std::chrono::steady_clock::now();
  profile.Refresh();
  auto round = pstart;
  for (int i = 0; i < 100; ++i) {
    const auto now = std::chrono::steady_clock::now();
    profile.Sample(uint64_t(cycles_per_ms / 10), std::chrono::duration<double, std::milli>(now - round).count());
    round = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const double sampled_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - pstart).count();
  title.join();
  sleeper.join();
  timeEndPeriod(1);
  std::string text, csv;
  profile.Report(sampled_s, cycles_per_ms, text, csv);
  // The title function first, its own time most of what was sampled; the spin among the host frames.
  const size_t row = csv.find("title,8200ABCD,");
  double own_ms = 0;
  if (row != std::string::npos) {
    const size_t a = csv.find(',', csv.find(',', csv.find(',', row) + 1) + 1);
    own_ms = std::atof(csv.c_str() + a + 1);
  }
  CHECK(cycles_per_ms > 1e5);
  CHECK(profile.samples() > 30);
  CHECK(row != std::string::npos && row == csv.find("title,"));
  CHECK(text.find("SpinInKnownFunction") != std::string::npos);
  CHECK(text.find("under 8200ABCD") != std::string::npos);  // the host function it calls, under it
  CHECK(own_ms > 0.6 * sampled_s * 1000 && own_ms < 1.2 * sampled_s * 1000);  // it ran the whole stretch
  std::printf("host_diagnostics_tests: profiled %llu samples in %.0f ms (%.0f cycles a ms); sub_8200ABCD's own time "
              "%.0f ms\n",
              static_cast<unsigned long long>(profile.samples()), sampled_s * 1000, cycles_per_ms, own_ms);
  if (g_failures) std::printf("%s\n%s\n", text.c_str(), csv.c_str());
#endif
  std::printf("host_diagnostics_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
