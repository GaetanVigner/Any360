// Throwaway (A/B worktree only, never committed): counts the calls of the bound functions (through the binding
// macros, CALL_COUNT) and of the hot unbound ones (call_counter_hot.inc), with snapshots of the counts 62 s and 142 s
// after the process starts (call_counts_a.txt, call_counts_b.txt in the working directory): the profile's window.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace call_counter {
struct Slot {
  uint32_t address;
  std::atomic<uint64_t> count{0};
  explicit Slot(uint32_t a) : address(a) {}
};
inline std::mutex g_lock;
inline std::vector<Slot*> g_slots;
inline Slot& Register(uint32_t address) {
  Slot* s = new Slot(address);
  std::lock_guard<std::mutex> l(g_lock);
  g_slots.push_back(s);
  return *s;
}
inline void Snapshot(const char* path) {
  std::lock_guard<std::mutex> l(g_lock);
  std::ofstream o(path, std::ios::trunc);
  for (Slot* s : g_slots)
    if (const uint64_t n = s->count.load(std::memory_order_relaxed)) o << std::hex << s->address << " " << std::dec << n << std::endl;
}
struct Writer {
  Writer() {
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(62));
      Snapshot("call_counts_a.txt");
      std::this_thread::sleep_for(std::chrono::seconds(80));
      Snapshot("call_counts_b.txt");
    }).detach();
  }
};
inline Writer g_writer;
}  // namespace call_counter

#define CALL_COUNT_JOIN2(a, b) a##b
#define CALL_COUNT_JOIN(a, b) CALL_COUNT_JOIN2(a, b)
#define CALL_COUNT(address)                                                                                       \
  do {                                                                                                            \
    static ::call_counter::Slot& slot_ = ::call_counter::Register(CALL_COUNT_JOIN(0x, CALL_COUNT_JOIN(address, u))); \
    slot_.count.fetch_add(1, std::memory_order_relaxed);                                                          \
  } while (0)
