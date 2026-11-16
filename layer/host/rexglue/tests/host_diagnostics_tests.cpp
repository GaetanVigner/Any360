// The hitch tracer's sampling (any360/host/hitch_tracer.h) on a thread that spins in a known function: the report must
// put that thread first (it used the cycles), name the function, and the sampling must not hang the sampled thread
// (nothing allocated while it is suspended). Windows only; elsewhere nothing is captured and the test passes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "any360/host/hitch_tracer.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
#endif
  std::printf("host_diagnostics_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
