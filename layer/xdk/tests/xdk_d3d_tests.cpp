// Unit tests of the D3D runtime's GPU wait made to sleep (any360/xdk/d3d.h). The wait is a loop of the caller: look
// at the word, run the step, look again. What is checked: a step sleeps only when the GPU has written nothing since
// the thread's previous step, a write made at any moment around the caller's look ends the sleep at once, and no
// wait ever lasts until its time-out while its word is written (the time-outs here are seconds: one lost wake and a
// test fails on its count of time-outs).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>

#include "any360/xdk/d3d.h"
#include "xdk_test.h"

namespace {

using any360::xdk::D3dGpuWait;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

// The GPU side as the binding gives it: a count that moves after each write the guest can see, and a wait for it.
struct Gpu {
  std::mutex mutex;
  std::condition_variable moved;
  uint64_t count = 0;
  uint64_t slept = 0, at_once = 0;  // waits that found the count as given, and that found it moved already

  uint64_t Progress() {
    std::lock_guard<std::mutex> lock(mutex);
    return count;
  }
  void WaitProgress(uint64_t seen, microseconds longest) {
    std::unique_lock<std::mutex> lock(mutex);
    if (count != seen) {
      ++at_once;
      return;
    }
    ++slept;
    moved.wait_for(lock, longest, [&] { return count != seen; });
  }
  void Notify() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++count;
    }
    moved.notify_all();
  }
};

// One thread, every order of the caller's look, the GPU's write and the step.
void Orders() {
  Gpu gpu;
  D3dGpuWait wait;
  // The first step of a thread: nothing known of the GPU, no sleep.
  CHECK(wait.Sleep(gpu, seconds(10)) && gpu.at_once == 1 && gpu.slept == 0 && wait.seen == 0);
  // Nothing written since: the step sleeps its time out, and the count it keeps is still right.
  auto start = steady_clock::now();
  CHECK(!wait.Sleep(gpu, milliseconds(20)) && gpu.slept == 1 && wait.seen == 0);
  CHECK(steady_clock::now() - start >= milliseconds(19));
  // The caller looks (its word is not there), the GPU writes, the step runs: no sleep, though the count the step
  // would read now is the new one.
  gpu.Notify();
  CHECK(wait.Sleep(gpu, seconds(10)) && gpu.at_once == 2 && gpu.slept == 1 && wait.seen == 1);
  // A wait that ended without a step (the caller found its word), GPU writes meanwhile, then another wait: its
  // first step does not sleep.
  gpu.Notify();
  gpu.Notify();
  CHECK(wait.Sleep(gpu, seconds(10)) && gpu.at_once == 3 && wait.seen == 3);
  // The GPU writes while the step sleeps: it wakes long before its time-out.
  std::thread writer([&] {
    std::this_thread::sleep_for(milliseconds(30));
    gpu.Notify();
  });
  start = steady_clock::now();
  CHECK(wait.Sleep(gpu, seconds(10)) && gpu.slept == 2 && wait.seen == 4);
  CHECK(steady_clock::now() - start < seconds(5));
  writer.join();
  // Each thread keeps its own count: another thread's first step does not sleep either.
  D3dGpuWait other;
  CHECK(other.Sleep(gpu, seconds(10)) && other.seen == 4 && gpu.at_once == 4);
}

// A waiter and the GPU's thread, `waits` waits for a word the GPU writes at a random moment around the waiter's
// looks, with or without other progress (vertical blanks, another thread's fences) moving the count meanwhile.
void Raced(uint32_t waits, bool noise) {
  Gpu gpu;
  std::atomic<uint32_t> word{0}, asked{0};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::mt19937 rng(7);
    for (uint32_t i = 1; i <= waits; ++i) {
      while (asked.load(std::memory_order_acquire) < i) std::this_thread::yield();
      for (uint32_t spin = rng() % 400; spin; --spin) (void)asked.load(std::memory_order_relaxed);
      word.store(i, std::memory_order_release);
      gpu.Notify();  // after the write, as the GPU side does
    }
  });
  std::thread other([&] {
    while (noise && !done.load(std::memory_order_acquire)) {
      gpu.Notify();
      std::this_thread::sleep_for(microseconds(50));
    }
  });
  D3dGpuWait wait;
  uint64_t timed_out = 0, steps = 0;
  const auto start = steady_clock::now();
  for (uint32_t i = 1; i <= waits; ++i) {
    asked.store(i, std::memory_order_release);
    while (word.load(std::memory_order_acquire) != i) {  // the caller's look
      ++steps;                                           // the step's own code: the wait goes on
      if (!wait.Sleep(gpu, seconds(5))) ++timed_out;
    }
  }
  done = true;
  writer.join();
  other.join();
  CHECK(timed_out == 0);
  CHECK(steady_clock::now() - start < seconds(5));
  // It slept: the waits were not spun through.
  CHECK(gpu.slept > 0 && steps < uint64_t(waits) * 50);
}

// Two threads waiting for their own words written by one GPU thread: each has its own count.
void TwoWaiters() {
  Gpu gpu;
  constexpr uint32_t kWaits = 5000;
  std::atomic<uint32_t> words[2] = {0, 0}, asked[2] = {0, 0};
  std::atomic<uint64_t> timed_out{0};
  std::thread writer([&] {
    uint32_t written[2] = {0, 0};
    while (written[0] < kWaits || written[1] < kWaits) {
      for (int t = 0; t < 2; ++t) {
        if (written[t] < kWaits && asked[t].load(std::memory_order_acquire) > written[t]) {
          words[t].store(++written[t], std::memory_order_release);
          gpu.Notify();
        }
      }
      std::this_thread::yield();
    }
  });
  auto waiter = [&](int t) {
    D3dGpuWait wait;
    for (uint32_t i = 1; i <= kWaits; ++i) {
      asked[t].store(i, std::memory_order_release);
      while (words[t].load(std::memory_order_acquire) != i) {
        if (!wait.Sleep(gpu, seconds(5))) ++timed_out;
      }
    }
  };
  std::thread a(waiter, 0), b(waiter, 1);
  a.join();
  b.join();
  writer.join();
  CHECK(timed_out == 0);
}

}  // namespace

int main() {
  Orders();
  Raced(20000, false);
  Raced(20000, true);
  TwoWaiters();
  return xdk_test::Finish("xdk_d3d_tests");
}
