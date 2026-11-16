// The hitch tracer (any360/host/hitch_tracer.h).
#include "any360/host/hitch_tracer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

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

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t kArmFrames = 300;   // fast frames in a row before a hitch is traced (play, not loading screens)
constexpr int kSampleEveryMs = 2;
constexpr int kLongestTraceMs = 2000;  // a frame longer than that is a hang: the hang watchdogs report those

}  // namespace

void HitchSampler::Refresh() {
  threads_.clear();
  const std::vector<uint32_t> ids = ProcessThreads();
  threads_.reserve(ids.size());
  for (uint32_t id : ids) threads_.emplace_back(id);
}

void HitchSampler::Begin() {
  for (Thread& t : threads_) {
    t.cycles = t.handle.Cycles(), t.samples = t.lost = 0;
    for (Path& p : t.paths) p = Path{};
  }
  rounds_ = 0;
}

void HitchSampler::Sample() {
  ++rounds_;
  for (Thread& t : threads_) {
    if (!t.handle.valid() || !t.handle.Capture(stack_) || !stack_.count) {
      ++t.lost;
      continue;
    }
    ++t.samples;
    // The innermost function (a wait in the system's DLLs shows there), then the executable's first three functions
    // calling it (the thread runs again: looking its functions up cannot block it).
    Path key;
    key.frames[0] = FunctionStart(stack_.frames[0]);
    for (size_t i = 1, d = 1; i < stack_.count && d < kDepth; ++i) {
      if (InExecutable(stack_.frames[i])) key.frames[d++] = FunctionStart(stack_.frames[i]);
    }
    Path* slot = nullptr;
    for (Path& p : t.paths) {
      if (!p.count || std::equal(p.frames, p.frames + kDepth, key.frames)) {
        slot = &p;
        break;
      }
    }
    if (!slot) continue;  // more distinct places than the table holds: the rest uncounted
    if (!slot->count) std::copy(key.frames, key.frames + kDepth, slot->frames);
    ++slot->count;
  }
}

std::string HitchSampler::End(double hitch_ms) {
  for (Thread& t : threads_) t.cycles = t.handle.Cycles() - t.cycles;
  std::vector<Thread*> order;
  for (Thread& t : threads_) order.push_back(&t);
  std::sort(order.begin(), order.end(), [](const Thread* a, const Thread* b) { return a->cycles > b->cycles; });
  char line[256];
  std::snprintf(line, sizeof(line), "Hitch: a frame of %.1f ms; %u samples of %zu threads, every %d ms\n", hitch_ms, rounds_,
                threads_.size(), kSampleEveryMs);
  std::string text = line;
  uint64_t total = 0;
  for (const Thread* t : order) total += t->cycles;
  size_t shown = 0;
  for (const Thread* t : order) {
    if (shown++ == 10 || t->cycles == 0) break;
    const std::string name = t->handle.Name();
    std::snprintf(line, sizeof(line), "\nThread %u%s%s%s: %.1f Mcycles (%.0f%% of the threads'), %u samples\n", t->handle.id(),
                  name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"", double(t->cycles) / 1e6,
                  total ? 100.0 * double(t->cycles) / double(total) : 0.0, t->samples);
    text += line;
    std::vector<const Path*> paths;
    for (const Path& p : t->paths) {
      if (p.count) paths.push_back(&p);
    }
    std::sort(paths.begin(), paths.end(), [](const Path* a, const Path* b) { return a->count > b->count; });
    for (size_t i = 0; i < paths.size() && i < 4; ++i) {
      std::snprintf(line, sizeof(line), "  %u of %u samples:\n", paths[i]->count, t->samples);
      text += line;
      for (uint64_t frame : paths[i]->frames) {
        if (frame) text += "    " + Describe(frame) + "\n";
      }
    }
  }
  return text;
}

HitchTracer::HitchTracer(double threshold_ms, std::string folder, int max_reports, std::function<void(const std::string&)> log)
    : threshold_ms_(threshold_ms), folder_(std::move(folder)), max_reports_(max_reports), log_(std::move(log)) {
#if defined(_WIN32)
  timeBeginPeriod(1);  // sleeps of 1-2 ms, not the default 15.6 (the samples' spacing)
#endif
  worker_ = std::thread([this] { Run(); });
}

HitchTracer::~HitchTracer() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void HitchTracer::Frame() {
  const int64_t now = NowNs(), last = last_frame_ns_.exchange(now, std::memory_order_acq_rel);
  frames_.fetch_add(1, std::memory_order_release);
  const bool fast = last && double(now - last) / 1e6 < threshold_ms_ / 2;
  if (fast) {
    fast_streak_.fetch_add(1, std::memory_order_relaxed);
  } else {
    fast_streak_.store(0, std::memory_order_relaxed);
  }
}

void HitchTracer::Run() {
  HitchSampler sampler;
  int reports = 0;
  auto refreshed = std::chrono::steady_clock::time_point{};
  while (!stop_ && reports < max_reports_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // The thread list kept fresh between hitches (threads come and go), never while a frame runs late.
    if (std::chrono::steady_clock::now() - refreshed > std::chrono::seconds(2) &&
        double(NowNs() - last_frame_ns_.load(std::memory_order_acquire)) / 1e6 < threshold_ms_ / 2) {
      sampler.Refresh();
      refreshed = std::chrono::steady_clock::now();
    }
    const int64_t last = last_frame_ns_.load(std::memory_order_acquire);
    if (!last || fast_streak_.load(std::memory_order_relaxed) < kArmFrames) continue;
    if (double(NowNs() - last) / 1e6 < threshold_ms_) continue;
    // A frame running past the threshold: sample until the next one (or until it is a hang, not a hitch).
    const uint64_t frames = frames_.load(std::memory_order_acquire);
    sampler.Begin();
    while (!stop_ && frames_.load(std::memory_order_acquire) == frames && double(NowNs() - last) / 1e6 < kLongestTraceMs) {
      sampler.Sample();
      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleEveryMs));
    }
    const double hitch_ms = double(last_frame_ns_.load(std::memory_order_acquire) - last) / 1e6;
    const std::string text = sampler.End(hitch_ms > 0 ? hitch_ms : double(NowNs() - last) / 1e6);
    std::error_code ec;
    std::filesystem::create_directories(folder_, ec);
    std::filesystem::path path;
    for (int n = 1; n < 1000; ++n) {
      path = std::filesystem::path(folder_) / ("hitch_" + std::to_string(n) + ".txt");
      if (!std::filesystem::exists(path, ec)) break;
    }
    std::ofstream(path) << text;
    ++reports;
    if (log_) log_("Any360: hitch of " + std::to_string(int(hitch_ms + 0.5)) + " ms traced to " + path.string());
    fast_streak_.store(0, std::memory_order_relaxed);  // armed again after another run of fast frames
  }
}

}  // namespace any360::host::diagnostics
