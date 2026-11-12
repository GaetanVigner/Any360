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
#include <vector>

#include "any360/xdk/d3d.h"
#include "any360/xdk/d3d_reference.h"
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

// The upload of the dirty shader constants against its model: masks of every shape (one bit, runs, all 64, the
// first and last bits), the buffer's pointer at every alignment, sources aligned or not, both runtimes' offsets.
// Every accepted call leaves memory, r4 and r5 as the model does; a call without room for a run is declined and
// leaves everything as it was.
struct ConstantsCase {
  uint32_t device, cursor, end, source;
  uint64_t mask, index;
};

bool Constants(xdk_test::Pair& p, uint32_t offset, const ConstantsCase& c) {
  namespace xdk = any360::xdk;
  const auto put32 = [&](uint32_t a, uint32_t v) {
    for (int b = 0; b < 4; ++b) p.native[a + b] = p.model[a + b] = uint8_t(v >> (8 * (3 - b)));
  };
  put32(c.device + offset, c.cursor);
  put32(c.device + offset + 4, c.end);
  uint64_t index = c.index, cursor = 0;
  any360::xdk::Range write;
  const bool planned = xdk::D3dDirtyConstantsAccepts(p.a, c.device, offset, c.mask, c.index, c.source, write);
  const bool accepted = xdk::D3dDirtyConstants(p.a, c.device, offset, c.mask, index, c.source, cursor);
  CHECK(planned == accepted);
  if (!accepted) {
    CHECK(p.SameEverywhere());
    return false;
  }
  uint64_t r4 = c.mask, r5 = c.index;
  xdk::reference::D3dDirtyConstants(p.b, offset, c.device, r4, r5, c.source);
  CHECK(p.SameEverywhere());
  CHECK(r4 == cursor && r5 == index);
  CHECK(write.at == c.cursor + 4 && write.at + write.size == uint32_t(cursor) + 4);
  return true;
}

void DirtyConstants() {
  xdk_test::Pair p(13);
  std::mt19937_64 rng(17);
  std::vector<uint64_t> masks = {1, 3, 0x8000000000000000ull, 0xC000000000000001ull, ~0ull, 0xFFull << 20,
                                 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00000000FFFFFFFFull,
                                 0xFFFFFFFF00000000ull, 0x8000000000000001ull, 0x0F0F0F0F0F0F0F0Full};
  for (int i = 0; i < 400; ++i) masks.push_back(rng() & rng());
  uint64_t accepted = 0;
  for (uint32_t offset : {48u, 40u}) {
    for (uint64_t mask : masks) {
      for (uint32_t align : {0u, 4u, 8u, 12u}) {
        for (uint32_t source : {0x40000u, 0x40004u, 0x4000Cu}) {
          const uint32_t cursor = 0x10000 + align + 4 * uint32_t(rng() % 64) * 4;
          const uint64_t index = (rng() % 4 == 0) ? (rng() << 32) | (rng() & 0x3FF) : rng() % 1024;
          accepted += Constants(p, offset, {0x80000, cursor, 0x30000, source, mask, index});
        }
      }
    }
  }
  CHECK(accepted == 2 * masks.size() * 4 * 3);
  // No room for a run: declined (the runtime's own function makes room), the whole call.
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x10000 + 4 * (16 + 3), 0x40000, 3, 0}));
  CHECK(Constants(p, 48, {0x80000, 0x10000, 0x10000 + 4 * (32 + 3) + 1, 0x40000, 3, 0}));
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x10000 + 4 * (32 + 3), 0x40000, 3, 0}));
  // The first run fits (to 0x1004C), the second does not: the whole call is declined.
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x10080, 0x40000, 0x8000000000000001ull, 0}));
  CHECK(Constants(p, 48, {0x80000, 0x10000, 0x10099, 0x40000, 0x8000000000000001ull, 0}));
  // A mask of 0 (the function would loop for ever), a pointer not on 4 bytes, the source over the buffer, the
  // device's words in what is written, memory that is not plain: declined.
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x30000, 0x40000, 0, 0}));
  CHECK(!Constants(p, 48, {0x80000, 0x10002, 0x30000, 0x40000, 1, 0}));
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x30000, 0x10040, 0x8000000000000000ull, 0}));
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x30000, 0x10040 - 63 * 64, 1, 0}));
  CHECK(!Constants(p, 48, {0x10010, 0x10000, 0x30000, 0x40000, 1, 0}));
  CHECK(!Constants(p, 48, {0x80000, 0x10000, 0x30000, 0x7F000000u, 1, 0}));
}

// The upload of 4-byte constants against its model: the same masks and alignments, the room test's edge.
bool Words(xdk_test::Pair& p, uint32_t offset, const ConstantsCase& c) {
  namespace xdk = any360::xdk;
  const auto put32 = [&](uint32_t a, uint32_t v) {
    for (int b = 0; b < 4; ++b) p.native[a + b] = p.model[a + b] = uint8_t(v >> (8 * (3 - b)));
  };
  put32(c.device + offset, c.cursor);
  put32(c.device + offset + 4, c.end);
  uint64_t index = c.index, cursor = 0;
  any360::xdk::Range write;
  const bool planned = xdk::D3dDirtyWordsAccepts(p.a, c.device, offset, c.mask, c.index, c.source, write);
  const bool accepted = xdk::D3dDirtyWords(p.a, c.device, offset, c.mask, index, c.source, cursor);
  CHECK(planned == accepted);
  if (!accepted) {
    CHECK(p.SameEverywhere());
    return false;
  }
  uint64_t r4 = c.mask, r5 = c.index;
  xdk::reference::D3dDirtyWords(p.b, offset, c.device, r4, r5, c.source);
  CHECK(p.SameEverywhere());
  CHECK(r4 == cursor && r5 == index);
  CHECK(write.at == c.cursor + 4 && write.at + write.size == uint32_t(cursor) + 4);
  return true;
}

void DirtyWords() {
  xdk_test::Pair p(19);
  std::mt19937_64 rng(23);
  std::vector<uint64_t> masks = {1, 3, 0x8000000000000000ull, 0xC000000000000001ull, ~0ull, 0xFFull << 20,
                                 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00000000FFFFFFFFull,
                                 0xFFFFFFFF00000000ull, 0x8000000000000001ull, 0x0F0F0F0F0F0F0F0Full};
  for (int i = 0; i < 400; ++i) masks.push_back(rng() & rng());
  uint64_t accepted = 0;
  for (uint32_t offset : {48u, 40u}) {
    for (uint64_t mask : masks) {
      for (uint32_t align : {0u, 4u, 8u, 12u}) {
        for (uint32_t source : {0x40000u, 0x40001u, 0x40006u}) {
          const uint32_t cursor = 0x10000 + align + 16 * uint32_t(rng() % 64);
          const uint64_t index = (rng() % 4 == 0) ? (rng() << 32) | (rng() & 0x3FF) : rng() % 1024;
          accepted += Words(p, offset, {0x80000, cursor, 0x30000, source, mask, index});
        }
      }
    }
  }
  CHECK(accepted == 2 * masks.size() * 4 * 3);
  // The room test: the run's bytes after the pointer, the header not counted, must stay below the end.
  CHECK(!Words(p, 48, {0x80000, 0x10000, 0x10000 + 8, 0x40000, 0xC000000000000000ull, 0}));
  CHECK(Words(p, 48, {0x80000, 0x10000, 0x10000 + 9, 0x40000, 0xC000000000000000ull, 0}));
  // The first run fits, the second does not: the whole call is declined.
  CHECK(!Words(p, 48, {0x80000, 0x10000, 0x10000 + 12, 0x40000, 0x8000000000000001ull, 0}));
  // A mask of 0, a pointer not on 4 bytes, the source over the buffer, the device's words in what is written,
  // memory that is not plain: declined.
  CHECK(!Words(p, 48, {0x80000, 0x10000, 0x30000, 0x40000, 0, 0}));
  CHECK(!Words(p, 48, {0x80000, 0x10002, 0x30000, 0x40000, 1, 0}));
  CHECK(!Words(p, 48, {0x80000, 0x10000, 0x30000, 0x10004, 0x8000000000000000ull, 0}));
  CHECK(!Words(p, 48, {0x10000 - 48 + 4, 0x10000, 0x30000, 0x40000, 0x8000000000000000ull, 0}));
  CHECK(!Words(p, 48, {0x80000, 0x10000, 0x30000, 0x7F000000u, 1, 0}));
}

// The copy of a title's shader constants into the device against its model: every count up to 70 and long ones,
// sources at every alignment, the device on and off 16 bytes, 64-bit registers.
bool SetConstants(xdk_test::Pair& p, uint64_t device, uint64_t first, uint32_t source, uint32_t count, uint64_t dirty) {
  namespace xdk = any360::xdk;
  constexpr uint32_t kStackPointer = xdk_test::kStack - 512;
  any360::xdk::Range write;
  const bool planned = xdk::D3dSetShaderConstantsAccepts(device, first, source, count, kStackPointer, write);
  const bool accepted = xdk::D3dSetShaderConstants(p.a, device, first, source, count, dirty, kStackPointer);
  CHECK(planned == accepted);
  if (!accepted) {
    CHECK(p.SameEverywhere());
    return false;
  }
  xdk::reference::D3dSetShaderConstants(p.b, device, first, source, count, dirty, kStackPointer);
  CHECK(p.SameEverywhere());
  return true;
}

void ShaderConstants() {
  xdk_test::Pair p(29);
  std::mt19937_64 rng(31);
  uint64_t accepted = 0, calls = 0;
  for (uint32_t count = 0; count <= 70; ++count) {
    for (uint32_t align = 0; align < 16; ++align) {
      for (uint32_t device_align : {0u, 4u, 8u}) {
        const uint64_t high = (rng() % 4 == 0) ? (rng() << 32) : 0;
        const uint64_t device = high | (0x20000 + device_align);
        const uint64_t first = rng() % 400;
        ++calls;
        accepted += SetConstants(p, device, first, 0x60000 + align + 16 * uint32_t(rng() % 8), count, rng());
      }
    }
  }
  for (uint32_t count : {255u, 256u, 300u, 512u}) {
    ++calls;
    accepted += SetConstants(p, 0x20000, 0, 0x60003, count, rng());
  }
  CHECK(accepted == calls);
  // Declined: the source over the destination, the device's bits in what is written, memory that is not plain.
  CHECK(!SetConstants(p, 0x20000, 0, 0x20000 + 112 * 16 + 8, 4, 1));
  CHECK(!SetConstants(p, 0x20000, 0x10000000 - 112, 0x60000, 4, 1));  // (first + 112) * 16 wraps to the device
  CHECK(!SetConstants(p, 0x20000, 0, 0x7F000000u, 4, 1));
  CHECK(!SetConstants(p, 0x7FC80000u, 0, 0x60000, 4, 1));
}

}  // namespace

int main() {
  Orders();
  Raced(20000, false);
  Raced(20000, true);
  TwoWaiters();
  DirtyConstants();
  DirtyWords();
  ShaderConstants();
  return xdk_test::Finish("xdk_d3d_tests");
}
