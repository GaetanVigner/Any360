// The bench's guest address space, laid out as a title's host lays it out: 4 GB reserved, the virtual part
// (0x00000000-0x9FFFFFFF) committed page by page on first touch, the physical memory (512 MB) seen through the
// three physical views (0xA0000000 64 KB pages, 0xC0000000 16 MB pages, 0xE0000000 4 KB pages, its guest address
// 0x1000 bytes further on the host), plus a fourth view for the GPU side that the write watch never protects.
// The write watch: physical pages armed read-only in the CPU's views; the first CPU write to one unprotects it and
// reports the page to the hook (the texture cache and the renderer then take it as changed).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace any360::bench {

class GuestSpace {
 public:
  static constexpr uint32_t kPhysicalBytes = 0x20000000, kPhysicalHostOffset = 0x1000;
  static GuestSpace& Get();

  bool Create();
  uint8_t* base() const { return base_; }
  uint8_t* physical() const { return gpu_view_; }

  // Arms the one-shot write watch over physical [begin, end).
  void Watch(uint32_t begin, uint32_t end);
  void SetWriteHook(std::function<void(uint32_t, uint32_t)> hook) { hook_ = std::move(hook); }
  uint64_t write_faults() const { return faults_.load(std::memory_order_relaxed); }
  uint64_t committed_pages() const { return committed_.load(std::memory_order_relaxed); }

 private:
  GuestSpace() = default;
  static long __stdcall Handler(void* info);
  bool OnFault(uint8_t* address, bool write);

  uint8_t* base_ = nullptr;
  uint8_t* gpu_view_ = nullptr;
  void* mapping_ = nullptr;
  uint8_t* views_[3] = {};
  std::atomic<uint8_t>* watched_ = nullptr;  // a byte a 4 KB physical page
  std::function<void(uint32_t, uint32_t)> hook_;
  std::atomic<uint64_t> faults_{0}, committed_{0};
};

}  // namespace any360::bench
