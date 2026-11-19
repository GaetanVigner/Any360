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
#include "any360/sys/alloc_meter.h"
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

// The uploads as register records against the PM4 they replace: both buffers decoded into (register, value) in the
// order written (type-0 values big-endian, pads skipped; records' values in the host's order) give the same list, the
// same index after; the records stop before the buffer's end, or are declined.
std::vector<std::pair<uint32_t, uint32_t>> Decode(const std::vector<uint8_t>& memory, uint32_t from, uint32_t to) {
  namespace record = any360::xdk::record;
  std::vector<std::pair<uint32_t, uint32_t>> out;
  const auto big = [&](uint32_t a) {
    return uint32_t(memory[a]) << 24 | uint32_t(memory[a + 1]) << 16 | uint32_t(memory[a + 2]) << 8 | memory[a + 3];
  };
  const auto host = [&](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, &memory[a], 4);
    return v;
  };
  uint32_t at = from;
  while (at <= to) {
    const uint32_t h = big(at);
    at += 4;
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    if ((h >> 30) == 2) continue;
    if ((h >> 30) == 0) {
      for (uint32_t i = 0; i < count; ++i, at += 4) out.push_back({(h & 0x7FFF) + i, big(at)});
      continue;
    }
    const uint32_t kind = host(at);
    CHECK(((h >> 8) & 0x7F) == record::kOpcode && (kind & 0xFF) == record::kRegisters);
    for (uint32_t i = 1; i < count; ++i) out.push_back({(kind >> 8) + i - 1, std::byteswap(big(at + 4 * i))});
    at += 4 * count;
  }
  return out;
}

void UploadRecords() {
  namespace xdk = any360::xdk;
  std::mt19937_64 rng(23);
  uint64_t compared = 0, declined = 0;
  for (int round = 0; round < 3000; ++round) {
    const bool words = round & 1;
    xdk_test::Pair p(100 + round);
    const uint32_t device = 0x80000, offset = round % 3 ? 48 : 40;
    const uint32_t cursor = 0x10000 + 4 * uint32_t(rng() % 64);
    const uint32_t end = rng() % 8 ? 0x30000 : cursor + 4 * uint32_t(rng() % 800);
    const auto put32 = [&](uint32_t a, uint32_t v) {
      for (int b = 0; b < 4; ++b) p.native[a + b] = p.model[a + b] = uint8_t(v >> (8 * (3 - b)));
    };
    put32(device + offset, cursor), put32(device + offset + 4, end);
    uint64_t mask = rng() % 4 ? rng() & rng() : rng();
    if (!mask) mask = 1;
    const uint64_t index0 = rng() % 1024;
    const uint32_t source = 0x40000 + 4 * uint32_t(rng() % 64);
    uint64_t index_pm4 = index0, index_records = index0, cursor_pm4 = 0, cursor_records = 0;
    const bool pm4 = words ? xdk::D3dDirtyWords(p.a, device, offset, mask, index_pm4, source, cursor_pm4)
                           : xdk::D3dDirtyConstants(p.a, device, offset, mask, index_pm4, source, cursor_pm4);
    const std::vector<uint8_t> before = p.model;
    const bool records = words ? xdk::D3dDirtyWordsRecords(p.b, device, offset, mask, index_records, source, cursor_records)
                               : xdk::D3dDirtyConstantsRecords(p.b, device, offset, mask, index_records, source, cursor_records);
    if (!records) {
      ++declined;
      CHECK(p.model == before);  // nothing written
      continue;
    }
    CHECK(pm4);  // the records fit only where the PM4 does (their room is checked against the same end)
    if (!pm4) continue;
    ++compared;
    CHECK(index_pm4 == index_records && cursor_records < end);
    CHECK(Decode(p.native, cursor + 4, uint32_t(cursor_pm4)) == Decode(p.model, cursor + 4, uint32_t(cursor_records)));
  }
  CHECK(compared > 2000 && declined > 50);
}

// D3DResource_AddRef / D3DResource_Release (Crackdown sub_826ACC90, sub_826ACD08) against their code read line by
// line: chains of 1 to 4 resources (type 4 or not, bit 30 or not), counts at 0, 1, 2, 3 and 0xFFFFFFFF; the memory, r3
// and the destroy calls (resource, depth, order) the same. Then what is declined,
// the count under 8 threads at once (exact, nothing destroyed), and the time a call.
void ResourceRefCounts() {
  namespace xdk = any360::xdk;
  namespace ref = any360::xdk::reference::refcount;
  xdk_test::Pair p(41);
  std::mt19937_64 rng(43);
  const auto put = [&](uint32_t address, uint32_t value) {
    xdk::detail::Set32(p.a.Host(address), value);
    xdk::detail::Set32(p.b.Host(address), value);
  };
  const uint32_t kCounts[] = {0u, 1u, 1u, 2u, 3u, 0xFFFFFFFFu};
  uint64_t compared = 0, destroyed = 0;
  for (int round = 0; round < 20000; ++round) {
    const uint32_t first = 0x10000 + 32 * uint32_t(rng() % 1024), length = 1 + uint32_t(rng() % 4);
    std::vector<uint32_t> chain;
    for (uint32_t i = 0; i < length; ++i) chain.push_back(i ? 0x40000 + 32 * (256 * i + uint32_t(rng() % 256)) : first);  // apart: no cycle
    for (uint32_t i = 0; i < length; ++i) {
      const bool last = i + 1 == length;
      const bool holds = !last && rng() % 4;  // a type-4 resource with bit 30 leads on to the next one
      const uint32_t type = holds ? 4 : (rng() % 3 ? 4 : uint32_t(rng() % 16));
      const uint32_t word = (uint32_t(rng()) & ~0x4000000Fu) | type | (holds || (type != 4 && rng() % 2) ? 0x40000000u : 0);
      put(chain[i], word);
      put(chain[i] + 4, kCounts[rng() % 6]);
      put(chain[i] + 24, holds ? chain[i + 1] : uint32_t(rng()));
    }
    const bool add = rng() % 2;
    std::vector<std::pair<uint32_t, uint32_t>> native_calls, model_calls;
    auto native_destroy = [&](uint32_t r, uint32_t depth) { native_calls.push_back({r, depth}); };
    auto model_destroy = [&](uint32_t r, uint32_t depth) { model_calls.push_back({r, depth}); };
    uint64_t native_r3 = 0;
    const bool accepted = add ? xdk::D3dResourceAddRef(p.a, first, native_r3)
                              : xdk::D3dResourceRelease(p.a, first, native_destroy, native_r3);
    CHECK(accepted);
    const uint64_t model_r3 = add ? ref::AddRef(p.b, first) : ref::Release(p.b, first, model_destroy);
    CHECK(native_r3 == model_r3);
    CHECK(native_calls == model_calls);
    for (uint32_t r : chain) CHECK(p.Same(r, r + 28, 0));
    ++compared, destroyed += model_calls.size();
  }
  CHECK(compared == 20000 && destroyed > 1000);
  // Declined, nothing changed: not aligned, not plain, a chain of type-4 parents that never ends (a cycle).
  uint64_t r3 = 0;
  auto none = [](uint32_t, uint32_t) {};
  put(0x20000, 0x40000004u), put(0x20004, 1), put(0x20018, 0x20000);
  CHECK(!xdk::D3dResourceAddRef(p.a, 0x20002, r3) && !xdk::D3dResourceRelease(p.a, 0x20002, none, r3));
  CHECK(!xdk::D3dResourceAddRef(p.a, 0x7FFFFFF0u, r3) && !xdk::D3dResourceRelease(p.a, 0x7FFFFFF0u, none, r3));
  CHECK(!xdk::D3dResourceAddRef(p.a, 0x20000, r3) && !xdk::D3dResourceRelease(p.a, 0x20000, none, r3));
  CHECK(xdk::detail::At32(p.a.Host(0x20004)) == 1);
  // 8 threads raising and dropping one resource's count at once: back to its count, nothing destroyed.
  put(0x30000, 0x40000004u), put(0x30004, 1), put(0x30018, 0x30100);
  put(0x30100, 0x00000003u), put(0x30104, 5);
  std::atomic<uint32_t> destroys{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      uint64_t c = 0;
      auto d = [&](uint32_t, uint32_t) { destroys.fetch_add(1); };
      for (int i = 0; i < 20000; ++i) {
        xdk::D3dResourceAddRef(p.a, 0x30000, c);
        xdk::D3dResourceRelease(p.a, 0x30000, d, c);
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK(xdk::detail::At32(p.a.Host(0x30004)) == 1 && xdk::detail::At32(p.a.Host(0x30104)) == 5 && destroys == 0);
  // Time a call (no allocation: the destroy callback is not reached), the replacement against the reference.
  put(0x30004, 2);
  const auto time = [&](auto&& call) {
    const auto start = steady_clock::now();
    for (int i = 0; i < 1000000; ++i) call();
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - start).count()) / 1e6;
  };
  uint64_t c = 0;
  const double native = time([&] {
    xdk::D3dResourceAddRef(p.a, 0x30000, c);
    xdk::D3dResourceRelease(p.a, 0x30000, none, c);
  });
  const double model = time([&] {
    ref::AddRef(p.b, 0x30000);
    ref::Release(p.b, 0x30000, none);
  });
  std::printf("xdk_d3d_tests: resource AddRef + Release: %.1f ns a pair (atomic), reference %.1f ns; 0 bytes allocated\n",
              native, model);
}

// D3DResource_GetType (Crackdown sub_826ACDB0) and the GPU format's block (sub_826ACFE8) against their code read line
// by line. GetType: random resources, mostly textures and surfaces, every dimension, the array bit, a surface's parent
// bit and its parent's dimension; r3 the same, every type it can give reached; and what it declines (read nothing).
// The block: every format up to 300 and random 32-bit numbers, the width and height words apart, the same word or
// overlapping; the bytes around them the same. Then the time a call and the bytes and allocations a call.
void ResourceTypesAndBlocks() {
  namespace xdk = any360::xdk;
  namespace ref = any360::xdk::reference::resource;
  xdk_test::Pair p(47);
  std::mt19937_64 rng(53);
  const auto put = [&](uint32_t address, uint32_t value) {
    xdk::detail::Set32(p.a.Host(address), value);
    xdk::detail::Set32(p.b.Host(address), value);
  };
  uint64_t by_type[32] = {};
  for (int i = 0; i < 200000; ++i) {
    const uint32_t resource = 0x10000 + 64 * uint32_t(rng() % 1024), parent = 0x30000 + 64 * uint32_t(rng() % 1024);
    const uint32_t pick = uint32_t(rng() % 8);
    const uint32_t type = pick < 3 ? 3 : pick < 6 ? 4 : uint32_t(rng() % 16);
    put(resource, (uint32_t(rng()) & ~0xFu) | type);
    put(resource + 24, parent);
    put(resource + 32, uint32_t(rng()));
    put(resource + 48, uint32_t(rng()));
    put(parent + 48, uint32_t(rng()));
    uint32_t got = 0xDEAD;
    CHECK(xdk::D3dResourceType(p.a, resource, got));
    const uint64_t expect = ref::GetType(p.b, resource);
    CHECK(got == expect);
    ++by_type[expect & 31];
  }
  for (uint32_t t : {3u, 4u, 16u, 17u, 18u, 19u, 20u, 0u, 15u}) CHECK(by_type[t] > 500);
  uint32_t type = 0;
  CHECK(!xdk::D3dResourceType(p.a, 0x7F000000u - 2, type));  // its first word not plain
  put(0x20000, 0x40000004u), put(0x20018, 0x7F000000u - 48);  // a surface whose parent's word is not plain
  CHECK(!xdk::D3dResourceType(p.a, 0x20000, type));
  CHECK(p.SameEverywhere());
  uint64_t blocks = 0;
  for (int i = 0; i < 20000; ++i) {
    const uint64_t format = i < 300 ? uint64_t(i) : rng();
    const uint32_t width_at = 0x50000 + 4 * uint32_t(rng() % 256);
    const uint32_t height_at = rng() % 8 == 0 ? width_at : rng() % 8 == 0 ? width_at + 2 : 0x51000 + 4 * uint32_t(rng() % 256);
    uint32_t w, h;
    xdk::D3dFormatBlock(uint32_t(format), w, h);
    xdk::detail::Set32(p.a.Host(width_at), w), xdk::detail::Set32(p.a.Host(height_at), h);
    ref::FormatBlock(p.b, format, width_at, height_at);
    CHECK(p.Same(width_at, width_at + 4) && p.Same(height_at, height_at + 4));
    blocks += w * h > 1;
  }
  CHECK(p.SameEverywhere() && blocks >= 16);  // the 16 formats with a block past 1x1
  // Time a call, and the bytes and allocations a call (none), the native against the reference.
  put(0x20000, 0x40000004u), put(0x20018, 0x30000), put(0x30030, 0x400);
  const auto time = [&](auto&& call) {
    const auto start = steady_clock::now();
    for (int i = 0; i < 10000000; ++i) call(i);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - start).count()) / 1e7;
  };
  volatile uint32_t sink = 0;
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const double type_native = time([&](int i) {
    uint32_t t = 0;
    xdk::D3dResourceType(p.a, i & 1 ? 0x20000 : 0x10000, t);
    sink = t;
  });
  const double block_native = time([&](int i) {
    uint32_t w, h;
    xdk::D3dFormatBlock(uint32_t(i & 63), w, h);
    xdk::detail::Set32(p.a.Host(0x50000), w), xdk::detail::Set32(p.a.Host(0x50004), h);
  });
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  const double type_model = time([&](int i) { sink = uint32_t(ref::GetType(p.b, i & 1 ? 0x20000 : 0x10000)); });
  const double block_model = time([&](int i) { ref::FormatBlock(p.b, uint64_t(i & 63), 0x50000, 0x50004); });
  CHECK(spent.count == 0 && spent.bytes == 0);
  std::printf("xdk_d3d_tests: GetType %.2f ns a call (reference %.2f), format block %.2f ns (reference %.2f); "
              "%llu bytes, %llu allocations\n",
              type_native, type_model, block_native, block_model, static_cast<unsigned long long>(spent.bytes),
              static_cast<unsigned long long>(spent.count));
}

// A texture's sizes from its fetch constant (Crackdown sub_826AD598) and a texture level's bytes (sub_826AD108) against
// their code read line by line. Sizes: random fetch words, every dimension, +40's bit 31; the outputs apart, the same
// word, or over the texture's word at +36 (whole or in part). Level bytes: sizes small and any 32 bits, bits 0-128,
// formats 0-70 and past, dimensions 0-3, the fixed pitch on or off, the size words apart or the same; r3 and memory the
// same, the decline (a block of 0 bytes without the fixed pitch) leaving memory as it was. Then the time a call and
// the bytes and allocations a call.
void TextureSizes() {
  namespace xdk = any360::xdk;
  namespace ref = any360::xdk::reference::resource;
  xdk_test::Pair p(59);
  std::mt19937_64 rng(61);
  const auto put = [&](uint32_t address, uint32_t value) {
    xdk::detail::Set32(p.a.Host(address), value);
    xdk::detail::Set32(p.b.Host(address), value);
  };
  uint64_t by_dimension[4] = {}, overlapped = 0;
  for (int i = 0; i < 100000; ++i) {
    const uint32_t texture = 0x10000 + 64 * uint32_t(rng() % 1024);
    put(texture + 36, uint32_t(rng())), put(texture + 40, uint32_t(rng())), put(texture + 48, uint32_t(rng()));
    uint32_t out[3] = {0x50000, 0x50004, 0x50008};
    for (uint32_t& o : out) {
      switch (rng() % 12) {
        case 0: o = texture + 36; break;
        case 1: o = texture + 34; break;
        case 2: o = out[0]; break;
        default: o = 0x50000 + 4 * uint32_t(rng() % 64); break;
      }
    }
    overlapped += out[0] == texture + 36 || out[1] == texture + 36 || out[0] == texture + 34;
    CHECK(xdk::D3dTextureDimensions(p.a, texture, out[0], out[1], out[2]));
    ref::TextureDimensions(p.b, texture, out[0], out[1], out[2]);
    CHECK(p.Same(texture, texture + 64) && p.Same(0x50000, 0x50100));
    ++by_dimension[(xdk::detail::At32(p.b.Host(texture + 48)) >> 9) & 3];
  }
  for (uint64_t n : by_dimension) CHECK(n > 20000);
  CHECK(overlapped > 10000 && p.SameEverywhere());
  CHECK(!xdk::D3dTextureDimensions(p.a, 0x7F000000u - 40, 0x50000, 0x50004, 0x50008));  // its words not plain
  uint64_t taken = 0, declined = 0, wide = 0;
  for (int i = 0; i < 200000; ++i) {
    uint32_t at[3];
    for (uint32_t& a : at) {
      a = 0x60000 + 4 * uint32_t(rng() % 16);
      put(a, rng() % 8 ? uint32_t(rng() % 4097) : uint32_t(rng()));
    }
    if (rng() % 8 == 0) at[1] = at[0];
    if (rng() % 16 == 0) at[2] = at[1];
    constexpr uint32_t kBits[] = {4, 8, 16, 32, 64, 128, 0, 24};
    const uint32_t bits = kBits[rng() % 8], format = rng() % 16 ? uint32_t(rng() % 71) : uint32_t(rng());
    const uint32_t kind = uint32_t(rng() % 4), fixed = rng() % 2 ? 0 : 1 + uint32_t(rng() % 4);
    uint64_t bytes = 0;
    if (!xdk::D3dTextureLevelBytes(p.a, at[0], at[1], at[2], bits, format, kind, fixed, bytes)) {
      ++declined;
      uint32_t w, h;
      xdk::D3dFormatBlock(format, w, h);
      CHECK(!fixed && ((w * h * bits) >> 3) == 0);
      CHECK(p.Same(0x60000, 0x60040));
      continue;
    }
    ++taken;
    CHECK(bytes == ref::TextureLevelBytes(p.b, at[0], at[1], at[2], bits, format, kind, fixed));
    wide += bytes >> 32 != 0;
    CHECK(p.Same(0x60000, 0x60040));
  }
  CHECK(taken > 150000 && declined > 5000 && wide > 1000 && p.SameEverywhere());  // wide: past 32 bits
  // Time a call, and the bytes and allocations a call (none), the native against the reference.
  put(0x10024, 0x0123ABCDu), put(0x10028, 0x80000000u), put(0x10030, 0x200u);
  const auto time = [&](auto&& call) {
    const auto start = steady_clock::now();
    for (int i = 0; i < 10000000; ++i) call(i);
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(steady_clock::now() - start).count()) / 1e7;
  };
  volatile uint32_t sink = 0;
  const auto sizes = [&](const xdk::Memory& m, int i) {
    xdk::detail::Set32(m.Host(0x60000), 1024 + uint32_t(i & 255));
    xdk::detail::Set32(m.Host(0x60004), 512), xdk::detail::Set32(m.Host(0x60008), 1);
  };
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const double dims_native = time([&](int) { xdk::D3dTextureDimensions(p.a, 0x10000, 0x50000, 0x50004, 0x50008); });
  const double level_native = time([&](int i) {
    sizes(p.a, i);
    uint64_t bytes = 0;
    xdk::D3dTextureLevelBytes(p.a, 0x60000, 0x60004, 0x60008, 32, uint32_t(i & 63), 1, 0, bytes);
    sink = uint32_t(bytes);
  });
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  const double dims_model = time([&](int) { ref::TextureDimensions(p.b, 0x10000, 0x50000, 0x50004, 0x50008); });
  const double level_model = time([&](int i) {
    sizes(p.b, i);
    sink = uint32_t(ref::TextureLevelBytes(p.b, 0x60000, 0x60004, 0x60008, 32, uint64_t(i & 63), 1, 0));
  });
  CHECK(spent.count == 0 && spent.bytes == 0);
  std::printf("xdk_d3d_tests: texture sizes %.2f ns a call (reference %.2f), level bytes %.2f ns (reference %.2f), "
              "%llu level calls compared, %llu declined; %llu bytes, %llu allocations\n",
              dims_native, dims_model, level_native, level_model, static_cast<unsigned long long>(taken),
              static_cast<unsigned long long>(declined), static_cast<unsigned long long>(spent.bytes),
              static_cast<unsigned long long>(spent.count));
}

int main() {
  ResourceRefCounts();
  ResourceTypesAndBlocks();
  TextureSizes();
  Orders();
  Raced(20000, false);
  Raced(20000, true);
  TwoWaiters();
  DirtyConstants();
  DirtyWords();
  ShaderConstants();
  UploadRecords();
  return xdk_test::Finish("xdk_d3d_tests");
}
