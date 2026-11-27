#include "any360/host/usage.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#else
#include <dirent.h>
#include <sched.h>
#include <time.h>

#include <cstdlib>
#endif

namespace any360::host {
namespace {

std::atomic<uint64_t> g_kept_mask{0};

uint64_t WallNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

UsageShares ComputeUsage(uint64_t wall_ns, uint64_t cpu_ns, uint32_t cores, int64_t gpu_ns, int64_t gpu_own_ns) {
  UsageShares s;
  s.cores = cores;
  if (!wall_ns) return s;
  const double wall = double(wall_ns);
  s.cpu_core_pct = 100.0 * double(cpu_ns) / wall;
  if (cores) s.cpu_allowed_pct = s.cpu_core_pct / double(cores);
  if (gpu_ns >= 0) s.gpu_pct = 100.0 * double(gpu_ns) / wall;
  if (gpu_own_ns >= 0) s.gpu_own_pct = 100.0 * double(gpu_own_ns) / wall;
  return s;
}

std::string FormatUsage(const UsageShares& s) {
  std::string out;
  char part[96];
  if (s.cpu_core_pct >= 0) {
    std::snprintf(part, sizeof part, "CPU %.0f%% of a core", s.cpu_core_pct);
    out += part;
    if (s.cpu_allowed_pct >= 0) {
      std::snprintf(part, sizeof part, ", %.0f%% of the %u allowed", s.cpu_allowed_pct, s.cores);
      out += part;
    }
    if (s.mask) {
      std::snprintf(part, sizeof part, " (mask 0x%llX", (unsigned long long)s.mask);
      out += part;
      if (s.mask_resets) {
        std::snprintf(part, sizeof part, ", set again %u time%s", s.mask_resets, s.mask_resets == 1 ? "" : "s");
        out += part;
      }
      out += ')';
    }
  }
  if (s.gpu_pct >= 0 || s.gpu_own_pct >= 0) {
    out += out.empty() ? "GPU " : "; GPU ";
    if (s.gpu_pct >= 0) {
      std::snprintf(part, sizeof part, "%.0f%% (system)", s.gpu_pct);
      out += part;
    }
    if (s.gpu_own_pct >= 0) {
      std::snprintf(part, sizeof part, "%s%.0f%% (batches)", s.gpu_pct >= 0 ? ", " : "", s.gpu_own_pct);
      out += part;
    }
  }
  return out;
}

uint64_t UsageMeter::ProcessCpuNs() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) * 100;
#else
  timespec t{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
  return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}

uint32_t UsageMeter::AllowedProcessors() {
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) return 0;
  uint32_t n = 0;
  for (; process; process &= process - 1) ++n;
  return n;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set)) return 0;
  return uint32_t(CPU_COUNT(&set));
#endif
}

uint64_t ProcessAffinity() {
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  return GetProcessAffinityMask(GetCurrentProcess(), &process, &system) ? uint64_t(process) : 0;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set)) return 0;
  uint64_t mask = 0;
  for (int i = 0; i < 64; ++i) {
    if (CPU_ISSET(i, &set)) mask |= uint64_t(1) << i;
  }
  return mask;
#endif
}

bool SetProcessAffinity(uint64_t mask) {
  if (!mask) return false;
#if defined(_WIN32)
  return SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask)) != 0;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < 64; ++i) {
    if (mask >> i & 1) CPU_SET(i, &set);
  }
  // Every thread: Linux keeps an affinity per thread.
  bool ok = true;
  if (DIR* tasks = opendir("/proc/self/task")) {
    while (dirent* e = readdir(tasks)) {
      if (e->d_name[0] != '.') ok &= sched_setaffinity(pid_t(std::atoi(e->d_name)), sizeof set, &set) == 0;
    }
    closedir(tasks);
  } else {
    ok = sched_setaffinity(0, sizeof set, &set) == 0;
  }
  return ok;
#endif
}

bool KeepProcessAffinity(uint64_t mask) {
  g_kept_mask = mask;
  return mask ? SetProcessAffinity(mask) : true;
}

uint64_t KeptProcessAffinity() { return g_kept_mask; }

uint64_t ParseAffinityMask(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (text.empty()) return 0;
  const auto number = [](std::string_view t, int base, uint64_t& out) {
    if (t.empty()) return false;
    out = 0;
    for (char c : t) {
      int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
              : base == 16 && std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(c) - 'a' + 10
                                                                            : -1;
      if (d < 0 || d >= base || out > (~uint64_t(0) - uint64_t(d)) / uint64_t(base)) return false;
      out = out * uint64_t(base) + uint64_t(d);
    }
    return true;
  };
  uint64_t value = 0;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    return number(text.substr(2), 16, value) ? value : 0;
  }
  if (text.find_first_of(",-") == std::string_view::npos) return number(text, 10, value) ? value : 0;
  uint64_t mask = 0;
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    const size_t dash = item.find('-');
    uint64_t first = 0, last = 0;
    if (!number(item.substr(0, dash), 10, first)) return 0;
    last = first;
    if (dash != std::string_view::npos && !number(item.substr(dash + 1), 10, last)) return 0;
    if (first > last || last > 63) return 0;
    for (uint64_t i = first; i <= last; ++i) mask |= uint64_t(1) << i;
  }
  return mask;
}

UsageMeter::UsageMeter() {
#if defined(_WIN32)
  D3DKMT_ENUMADAPTERS2 list{};
  if (D3DKMTEnumAdapters2(&list) == 0 && list.NumAdapters) {
    std::vector<D3DKMT_ADAPTERINFO> adapters(list.NumAdapters);
    list.pAdapters = adapters.data();
    if (D3DKMTEnumAdapters2(&list) == 0) {
      adapters.resize(list.NumAdapters);
      for (const D3DKMT_ADAPTERINFO& a : adapters) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        q.AdapterLuid = a.AdapterLuid;
        if (D3DKMTQueryStatistics(&q) == 0) {
          const uint64_t luid = (uint64_t(uint32_t(a.AdapterLuid.HighPart)) << 32) | a.AdapterLuid.LowPart;
          for (ULONG node = 0; node < q.QueryResult.AdapterInformation.NodeCount; ++node) engines_.push_back({luid, node});
        }
        D3DKMT_CLOSEADAPTER close{a.hAdapter};
        D3DKMTCloseAdapter(&close);
      }
    }
  }
#endif
  wall_ns_ = WallNs();
  cpu_ns_ = ProcessCpuNs();
  gpu_ns_ = GpuEngineNs();
}

std::vector<uint64_t> UsageMeter::GpuEngineNs() const {
  std::vector<uint64_t> ns;
#if defined(_WIN32)
  ns.reserve(engines_.size());
  for (const Engine& e : engines_) {
    D3DKMT_QUERYSTATISTICS q{};
    q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
    q.AdapterLuid.LowPart = DWORD(e.luid);
    q.AdapterLuid.HighPart = LONG(e.luid >> 32);
    q.hProcess = GetCurrentProcess();
    q.QueryProcessNode.NodeId = e.node;
    const size_t i = ns.size();
    ns.push_back(D3DKMTQueryStatistics(&q) == 0 ? uint64_t(q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart) * 100
                 : i < gpu_ns_.size()             ? gpu_ns_[i]
                                                  : 0);
  }
#endif
  return ns;
}

UsageShares UsageMeter::Take(int64_t gpu_own_ns) {
  const uint64_t kept = g_kept_mask;
  uint32_t resets = 0;
  if (kept && ProcessAffinity() != kept && SetProcessAffinity(kept)) ++resets;
  const uint64_t wall = WallNs(), cpu = ProcessCpuNs();
  std::vector<uint64_t> gpu = GpuEngineNs();
  int64_t busiest = -1;
  if (!gpu.empty() && gpu.size() == gpu_ns_.size()) {
    busiest = 0;
    for (size_t i = 0; i < gpu.size(); ++i) busiest = std::max(busiest, int64_t(gpu[i] - std::min(gpu[i], gpu_ns_[i])));
  }
  UsageShares s = ComputeUsage(wall - wall_ns_, cpu - std::min(cpu, cpu_ns_), AllowedProcessors(), busiest, gpu_own_ns);
  s.mask = ProcessAffinity();
  s.mask_resets = resets;
  wall_ns_ = wall, cpu_ns_ = cpu, gpu_ns_ = std::move(gpu);
  return s;
}

}  // namespace any360::host
