// The global operator new / delete, counting each thread's allocations (any360/sys/alloc_meter.h). Every form
// forwards to the C runtime's heap: malloc / free, and _aligned_malloc / _aligned_free for the aligned ones (as the
// C runtime's own operator new does), so memory one module allocates another may free.
#include "any360/sys/alloc_meter.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

// A thread's counts, written by that thread only, read by ProcessAllocations from any thread. The blocks form a list
// linked through themselves (registering a thread allocates nothing); an ending thread's counts move into g_ended.
struct ThreadBlock {
  std::atomic<uint64_t> bytes{0}, count{0};
  ThreadBlock* next = nullptr;
  ThreadBlock* previous = nullptr;
  ThreadBlock();
  ~ThreadBlock();
  void Add(std::size_t size) {
    bytes.store(bytes.load(std::memory_order_relaxed) + size, std::memory_order_relaxed);
    count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
};

// Held to register or end a thread and to sum the blocks: a few instructions each, rare.
constinit std::atomic_flag g_blocks_lock = ATOMIC_FLAG_INIT;
struct BlocksLock {
  BlocksLock() {
    while (g_blocks_lock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
  }
  ~BlocksLock() { g_blocks_lock.clear(std::memory_order_release); }
};
constinit ThreadBlock* g_blocks = nullptr;
constinit any360::sys::AllocCounts g_ended{};

ThreadBlock::ThreadBlock() {
  BlocksLock lock;
  next = g_blocks;
  if (next) next->previous = this;
  g_blocks = this;
}

ThreadBlock::~ThreadBlock() {
  BlocksLock lock;
  g_ended.bytes += bytes.load(std::memory_order_relaxed);
  g_ended.count += count.load(std::memory_order_relaxed);
  (previous ? previous->next : g_blocks) = next;
  if (next) next->previous = previous;
}

thread_local ThreadBlock t_block;

void* Allocate(std::size_t size) {
  t_block.Add(size);
  for (;;) {
    if (void* p = std::malloc(size ? size : 1)) return p;
    std::new_handler handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
}

void* AllocateAligned(std::size_t size, std::align_val_t alignment) {
  t_block.Add(size);
  for (;;) {
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size ? size : 1, std::size_t(alignment))) return p;
#else
    void* p = nullptr;
    if (posix_memalign(&p, std::max(std::size_t(alignment), sizeof(void*)), size ? size : 1) == 0) return p;
#endif
    std::new_handler handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
}

void Free(void* p) noexcept { std::free(p); }

void FreeAligned(void* p) noexcept {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

}  // namespace

namespace any360::sys {
AllocCounts ThreadAllocations() {
  return {t_block.bytes.load(std::memory_order_relaxed), t_block.count.load(std::memory_order_relaxed)};
}

AllocCounts ProcessAllocations() {
  BlocksLock lock;
  AllocCounts total = g_ended;
  for (const ThreadBlock* b = g_blocks; b; b = b->next) {
    total.bytes += b->bytes.load(std::memory_order_relaxed);
    total.count += b->count.load(std::memory_order_relaxed);
  }
  return total;
}
}  // namespace any360::sys

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return AllocateAligned(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return AllocateAligned(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete(void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
