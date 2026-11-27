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
#include "any360/host/usage.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
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


#if defined(_WIN32)
// GPU work the system counts against this process: a large target cleared again and again on a D3D11 device (the
// hardware one), timed by the device's own timestamps; returns those ns, < 0 without a device. `meter` is read once the
// work is done, while the device still exists (the system's statistics of a process end with its last device there).
int64_t GpuWork(int clears, any360::host::UsageMeter& meter, any360::host::UsageShares& shares) {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                               nullptr, &context))) {
    return -1;
  }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = 4096;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* texture = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
  int64_t ns = -1;
  if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
      SUCCEEDED(device->CreateRenderTargetView(texture, nullptr, &view)) && SUCCEEDED(device->CreateQuery(&qd, &disjoint)) &&
      SUCCEEDED(device->CreateQuery(&qt, &begin)) && SUCCEEDED(device->CreateQuery(&qt, &end))) {
    context->Begin(disjoint);
    context->End(begin);
    for (int i = 0; i < clears; ++i) {
      const float colour[4] = {float(i & 1), 0.5f, 0.25f, 1.0f};
      context->ClearRenderTargetView(view, colour);
    }
    context->End(end);
    context->End(disjoint);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    while (context->GetData(disjoint, &d, sizeof d, 0) == S_FALSE) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    UINT64 t0 = 0, t1 = 0;
    context->GetData(begin, &t0, sizeof t0, 0);
    context->GetData(end, &t1, sizeof t1, 0);
    if (!d.Disjoint && d.Frequency && t1 > t0) ns = int64_t(double(t1 - t0) * 1e9 / double(d.Frequency));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    shares = meter.Take();
  }
  for (IUnknown* u : {static_cast<IUnknown*>(end), static_cast<IUnknown*>(begin), static_cast<IUnknown*>(disjoint),
                      static_cast<IUnknown*>(view), static_cast<IUnknown*>(texture), static_cast<IUnknown*>(context),
                      static_cast<IUnknown*>(device)}) {
    if (u) u->Release();
  }
  return ns;
}
#endif

// The usage meter (any360/host/usage.h): the shares from a period's figures and their text; then the process's own
// figures: one thread spinning reads as about one core, the allowed processors as the affinity says, and the GPU's
// time the system counts against the process as about what the device's timestamps measured.
void TestUsage() {
  using any360::host::ComputeUsage;
  using any360::host::FormatUsage;
  using any360::host::UsageMeter;
  const auto s = ComputeUsage(1000000000, 2450000000, 8, 620000000, 600000000);
  CHECK(s.cpu_core_pct > 244.9 && s.cpu_core_pct < 245.1);
  CHECK(s.cpu_allowed_pct > 30.6 && s.cpu_allowed_pct < 30.7);
  CHECK(s.gpu_pct > 61.9 && s.gpu_pct < 62.1);
  CHECK(s.gpu_own_pct > 59.9 && s.gpu_own_pct < 60.1);
  CHECK(FormatUsage(s) == "CPU 245% of a core, 31% of the 8 allowed; GPU 62% (system), 60% (batches)");
  const auto unknown = ComputeUsage(1000000000, 500000000, 0, -1, -1);
  CHECK(unknown.cpu_allowed_pct < 0 && unknown.gpu_pct < 0 && unknown.gpu_own_pct < 0);
  CHECK(FormatUsage(unknown) == "CPU 50% of a core");
  CHECK(FormatUsage(ComputeUsage(1000000000, 0, 2, -1, 250000000)) == "CPU 0% of a core, 0% of the 2 allowed; GPU 25% (batches)");
  CHECK(ComputeUsage(0, 5, 1, 5, 5).cpu_core_pct < 0);  // no time passed: nothing known
  auto masked = ComputeUsage(1000000000, 1000000000, 2, -1, -1);
  masked.mask = 0x3;
  CHECK(FormatUsage(masked) == "CPU 100% of a core, 50% of the 2 allowed (mask 0x3)");
  masked.mask_resets = 1;
  CHECK(FormatUsage(masked) == "CPU 100% of a core, 50% of the 2 allowed (mask 0x3, set again 1 time)");
  masked.mask_resets = 2;
  CHECK(FormatUsage(masked) == "CPU 100% of a core, 50% of the 2 allowed (mask 0x3, set again 2 times)");

  using any360::host::ParseAffinityMask;
  CHECK(ParseAffinityMask("0xFF") == 0xFF && ParseAffinityMask("0x3") == 0x3 && ParseAffinityMask(" 0Xff00 ") == 0xFF00);
  CHECK(ParseAffinityMask("255") == 0xFF && ParseAffinityMask("3") == 0x3);
  CHECK(ParseAffinityMask("0-7") == 0xFF && ParseAffinityMask("0,1") == 0x3 && ParseAffinityMask("8-15") == 0xFF00);
  CHECK(ParseAffinityMask("0-1,4,6-7") == 0xD3 && ParseAffinityMask("63") == 63 && ParseAffinityMask("63-63") == (1ull << 63));
  CHECK(ParseAffinityMask("") == 0 && ParseAffinityMask("0x") == 0 && ParseAffinityMask("zz") == 0);
  CHECK(ParseAffinityMask("7-0") == 0 && ParseAffinityMask("0-64") == 0 && ParseAffinityMask("1,,2") == 0);
  CHECK(ParseAffinityMask("0x1FFFFFFFFFFFFFFFF") == 0 && ParseAffinityMask("0") == 0);

  UsageMeter meter;
  std::atomic<bool> stop{false};
  std::thread spinner([&] {
    while (!stop.load(std::memory_order_relaxed)) g_spins.fetch_add(1, std::memory_order_relaxed);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  stop = true;
  spinner.join();
  const auto spun = meter.Take();
  CHECK(spun.cpu_core_pct > 70 && spun.cpu_core_pct < 160);
  CHECK(spun.cores >= 1 && spun.cores == UsageMeter::AllowedProcessors());
  std::printf("host_diagnostics_tests: one thread spinning 400 ms reads as %s\n", FormatUsage(spun).c_str());
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
  uint32_t allowed = 0;
  for (DWORD_PTR m = process; m; m &= m - 1) ++allowed;
  CHECK(UsageMeter::AllowedProcessors() == allowed);
  CHECK(!meter.GpuEngineNs().empty());
  // A kept mask holds: set, changed behind its back (as a runtime's own processor setup does), set again by Take.
  {
    using any360::host::KeepProcessAffinity;
    using any360::host::KeptProcessAffinity;
    using any360::host::ProcessAffinity;
    using any360::host::SetProcessAffinity;
    const uint64_t original = ProcessAffinity();
    const uint64_t first = original & (~original + 1);  // its lowest processor
    CHECK(original != 0 && uint64_t(process) == original);
    CHECK(KeepProcessAffinity(first) && KeptProcessAffinity() == first && ProcessAffinity() == first);
    CHECK(UsageMeter::AllowedProcessors() == 1);
    CHECK(SetProcessAffinity(original) && ProcessAffinity() == original);
    const auto held = meter.Take();
    CHECK(held.mask == first && held.mask_resets == 1 && ProcessAffinity() == first);
    const auto still = meter.Take();
    CHECK(still.mask == first && still.mask_resets == 0);
    CHECK(KeepProcessAffinity(0) && KeptProcessAffinity() == 0 && ProcessAffinity() == first);  // none kept: left as is
    CHECK(SetProcessAffinity(original) && ProcessAffinity() == original);
    CHECK(meter.Take().mask_resets == 0 && ProcessAffinity() == original);
    CHECK(!SetProcessAffinity(0));
    std::printf("host_diagnostics_tests: mask 0x%llX kept against a reset to 0x%llX: %s\n", (unsigned long long)first,
                (unsigned long long)original, FormatUsage(held).c_str());
  }
  meter.Take();
  const auto wall0 = std::chrono::steady_clock::now();
  any360::host::UsageShares worked;
  const int64_t timed = GpuWork(3000, meter, worked);
  const double wall_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - wall0).count();
  const double counted_ns = worked.gpu_pct / 100.0 * wall_ns;
  CHECK(timed > 20000000);  // the work took the GPU 20 ms or more
  CHECK(worked.gpu_pct >= 0 && worked.gpu_pct <= 101);
  CHECK(counted_ns > 0.5 * double(timed) && counted_ns < 2.0 * double(timed) + 5e6);
  std::printf("host_diagnostics_tests: GPU work the device timed at %.1f ms, the system counted %.1f ms (%s)\n",
              double(timed) / 1e6, counted_ns / 1e6, FormatUsage(worked).c_str());
#endif
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
  TestUsage();
  std::printf("host_diagnostics_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
