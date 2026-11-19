// The function profiler (any360/host/profiler.h).
#include "any360/host/profiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

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

namespace any360::host::diagnostics {

namespace {

constexpr int kSampleEveryMs = 2;

// The function's name in Describe's "module!function+0xoffset (file:line)", or the whole text.
std::string FunctionName(const std::string& described) {
  const size_t bang = described.find('!');
  const size_t from = bang == std::string::npos ? 0 : bang + 1;
  size_t to = described.find("+0x", from);
  if (to == std::string::npos) to = described.find(" (", from);
  return described.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

// The title function's guest address in a symbol (sub_XXXXXXXX anywhere in it), or 0.
uint32_t GuestAddress(const std::string& name) {
  for (size_t at = name.find("sub_"); at != std::string::npos; at = name.find("sub_", at + 1)) {
    if (at + 12 > name.size()) break;
    uint32_t value = 0;
    bool hex = true;
    for (size_t i = at + 4; i < at + 12; ++i) {
      const char c = name[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0) {
        hex = false;
        break;
      }
      value = value << 4 | uint32_t(digit);
    }
    if (hex && (at + 12 == name.size() || !std::isxdigit(static_cast<unsigned char>(name[at + 12])))) return value;
  }
  return 0;
}

// A system call's stub (ntdll's Zw* / Nt*): the thread is in the kernel, nearly always waiting. The cycles it used
// since the last look were spent before it blocked, elsewhere: not put on the stack it waits in.
bool InKernel(const std::string& name) {
  return name.size() > 2 && ((name[0] == 'Z' && name[1] == 'w') || (name[0] == 'N' && name[1] == 't')) &&
         std::isupper(static_cast<unsigned char>(name[2]));
}

}  // namespace

double MeasureCyclesPerMs() {
#if defined(_WIN32)
  ThreadHandle self(GetCurrentThreadId());
#else
  ThreadHandle self(0);
#endif
  if (!self.valid()) return 0;
  const auto start = std::chrono::steady_clock::now();
  const uint64_t c0 = self.Cycles();
  volatile uint64_t spin = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(30)) spin = spin + 1;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return ms > 0 ? double(self.Cycles() - c0) / ms : 0;
}

void ProfileSampler::Refresh() {
  std::unordered_map<uint32_t, uint64_t> seen;
  for (const Thread& t : threads_) seen[t.handle.id()] = t.last_cycles;
  threads_.clear();
  const std::vector<uint32_t> ids = ProcessThreads();
  threads_.reserve(ids.size());
  for (uint32_t id : ids) {
    threads_.emplace_back(id);
    Thread& t = threads_.back();
    const auto it = seen.find(id);
    t.last_cycles = it != seen.end() ? it->second : t.handle.Cycles();
  }
}

void ProfileSampler::Sample(uint64_t min_cycles, double ms) {
  for (Thread& t : threads_) {
    if (!t.handle.valid()) continue;
    const uint64_t now = t.handle.Cycles(), used = now - t.last_cycles;
    if (used < min_cycles) continue;
    t.last_cycles = now;
    if (!t.handle.Capture(stack_) || !stack_.count) continue;
    // The thread runs again: the tally may allocate.
    Key key;
    key.frames[0] = FunctionStart(stack_.frames[0]);
    for (size_t i = 0, d = 1; i < stack_.count && d <= kDepth; ++i) {
      if (InExecutable(stack_.frames[i])) key.frames[d++] = FunctionStart(stack_.frames[i]);
    }
    tally_[key] += ms;
    ++samples_;
  }
}

void ProfileSampler::Report(double seconds, double cycles_per_ms, std::string& text, std::string& csv) const {
  // Symbols, once a function.
  std::unordered_map<uint64_t, std::string> names;
  const auto name_of = [&](uint64_t start) -> const std::string& {
    auto it = names.find(start);
    if (it == names.end()) it = names.emplace(start, FunctionName(Describe(start))).first;
    return it->second;
  };
  std::unordered_map<uint32_t, double> own, total, self;
  std::map<std::string, double> called;                   // host functions title functions call
  std::map<std::string, std::map<uint32_t, double>> called_by;
  std::unordered_map<uint32_t, std::string> guest_symbol;
  std::map<std::string, double> host;
  double all = 0, waited = 0;
  for (const auto& [key, cycles] : tally_) {
    const std::string& leaf = name_of(key.frames[0]);
    if (InKernel(leaf)) {
      waited += cycles;
      continue;
    }
    all += cycles;
    host[leaf] += cycles;
    std::set<uint32_t> on_stack;
    bool innermost = true;
    for (size_t i = 1; i <= kDepth && key.frames[i]; ++i) {
      const std::string& name = name_of(key.frames[i]);
      const uint32_t guest = GuestAddress(name);
      if (!guest) continue;
      if (!guest_symbol.count(guest)) guest_symbol[guest] = name;
      if (innermost) {
        own[guest] += cycles, innermost = false;
        if (i == 1) {
          self[guest] += cycles;  // its own code (or the system's, called straight)
        } else {
          const std::string& host_name = name_of(key.frames[i - 1]);
          called[host_name] += cycles;
          called_by[host_name][guest] += cycles;
        }
      }
      on_stack.insert(guest);
    }
    for (uint32_t g : on_stack) total[g] += cycles;
  }
  const auto ms = [](double v) { return v; };
  std::vector<std::pair<uint32_t, double>> by_own(own.begin(), own.end());
  std::sort(by_own.begin(), by_own.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<std::pair<std::string, double>> by_host(host.begin(), host.end());
  std::sort(by_host.begin(), by_host.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  double in_title = 0;
  for (const auto& [g, c] : own) in_title += c;
  char line[512];
  std::snprintf(line, sizeof(line),
                "Profile: %.1f s sampled every %d ms, %llu samples, %.0f ms of processor time caught running (%.2f cores), "
                "%.0f ms of it under title functions; threads caught waiting in the kernel for %.0f ms more (not counted); "
                "%.0f cycles a ms\n",
                seconds, kSampleEveryMs, static_cast<unsigned long long>(samples_), ms(all), seconds > 0 ? ms(all) / (seconds * 1000) : 0,
                ms(in_title), ms(waited), cycles_per_ms);
  text = line;
  text += "\nTitle functions by their own time (with the host code they call), then with what they call:\n";
  text += "  rank  address    own ms  own %  self ms   total ms  symbol\n";
  for (size_t i = 0; i < by_own.size() && i < 150; ++i) {
    const auto& [g, c] = by_own[i];
    std::snprintf(line, sizeof(line), "  %4zu  %08X %8.1f  %5.2f %8.1f  %9.1f  %s\n", i + 1, g, ms(c), all > 0 ? 100.0 * c / all : 0.0,
                  ms(self.count(g) ? self.at(g) : 0.0), ms(total.at(g)), guest_symbol[g].c_str());
    text += line;
  }
  std::vector<std::pair<std::string, double>> by_called(called.begin(), called.end());
  std::sort(by_called.begin(), by_called.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  text += "\nHost code title functions call (bindings' handlers, natives, kernel exports), and its top caller:\n";
  for (size_t i = 0; i < by_called.size() && i < 60; ++i) {
    const auto& callers = called_by.at(by_called[i].first);
    const auto top = std::max_element(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::snprintf(line, sizeof(line), "  %4zu  %8.1f ms  under %08X  ", i + 1, ms(by_called[i].second), top->first);
    text += line + by_called[i].first + "\n";
  }
  text += "\nHost functions by the time they were the innermost frame (the kernel's left out):\n";
  for (size_t i = 0; i < by_host.size() && i < 60; ++i) {
    std::snprintf(line, sizeof(line), "  %4zu  %8.1f ms  %5.2f %%  ", i + 1, ms(by_host[i].second),
                  all > 0 ? 100.0 * by_host[i].second / all : 0.0);
    text += line + by_host[i].first + "\n";
  }
  csv = "kind,address,symbol,own_ms,total_ms,self_ms\n";
  for (const auto& [g, c] : by_own) {
    std::snprintf(line, sizeof(line), "title,%08X,%s,%.3f,%.3f,%.3f\n", g, guest_symbol[g].c_str(), ms(c), ms(total.at(g)),
                  ms(self.count(g) ? self.at(g) : 0.0));
    csv += line;
  }
  const auto clean = [](std::string s) {
    std::replace(s.begin(), s.end(), ',', ';');
    std::replace(s.begin(), s.end(), '"', '\'');
    return s;
  };
  for (const auto& [name, c] : by_called) {
    for (const auto& [g, gc] : called_by.at(name)) {
      std::snprintf(line, sizeof(line), "called,%08X,", g);
      csv += line + clean(name);
      std::snprintf(line, sizeof(line), ",%.3f,,\n", ms(gc));
      csv += line;
    }
  }
  // (Host symbols can be long templates: written whole, their commas turned to semicolons.)
  for (const auto& [name, c] : by_host) {
    std::snprintf(line, sizeof(line), ",%.3f,,\n", ms(c));
    csv += "host,," + clean(name) + line;
  }
}

Profiler::Profiler(double after_seconds, double seconds, std::string folder, std::function<void(const std::string&)> log)
    : after_seconds_(after_seconds), seconds_(seconds), folder_(std::move(folder)), log_(std::move(log)) {
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
  worker_ = std::thread([this] { Run(); });
}

Profiler::~Profiler() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void Profiler::Run() {
  const auto begin = std::chrono::steady_clock::now();
  while (!stop_ && std::chrono::steady_clock::now() - begin < std::chrono::duration<double>(after_seconds_)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (stop_) return;
  const double cycles_per_ms = MeasureCyclesPerMs();
  // A thread is looked at when it ran a fiftieth of a sample's spacing since the last look (else it is waiting).
  const uint64_t min_cycles = uint64_t(cycles_per_ms * kSampleEveryMs / 50);
  ProfileSampler sampler;
  sampler.Refresh();
  const auto start = std::chrono::steady_clock::now();
  auto refreshed = start, round = start;
  if (log_) log_("Any360: profiling the threads for " + std::to_string(int(seconds_)) + " s");
  while (!stop_ && std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds_)) {
    const auto now = std::chrono::steady_clock::now();
    sampler.Sample(min_cycles, std::chrono::duration<double, std::milli>(now - round).count());
    round = now;
    if (std::chrono::steady_clock::now() - refreshed > std::chrono::seconds(2)) {
      sampler.Refresh();
      refreshed = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSampleEveryMs));
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::string text, csv;
  sampler.Report(seconds, cycles_per_ms, text, csv);
  std::error_code ec;
  std::filesystem::create_directories(folder_, ec);
  const std::filesystem::path path = std::filesystem::path(folder_) / "profile.txt";
  std::ofstream(path) << text;
  std::ofstream(std::filesystem::path(folder_) / "profile.csv") << csv;
  if (log_) log_("Any360: profile of " + std::to_string(int(seconds + 0.5)) + " s written to " + path.string());
}

}  // namespace any360::host::diagnostics
