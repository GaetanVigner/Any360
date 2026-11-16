// The global operator new / delete, counting each thread's allocations (any360/sys/alloc_meter.h). Every form
// forwards to the C runtime's heap: malloc / free, and _aligned_malloc / _aligned_free for the aligned ones (as the
// C runtime's own operator new does), so memory one module allocates another may free.
#include "any360/sys/alloc_meter.h"

#include <algorithm>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

thread_local any360::sys::AllocCounts t_counts;

void* Allocate(std::size_t size) {
  t_counts.bytes += size, ++t_counts.count;
  for (;;) {
    if (void* p = std::malloc(size ? size : 1)) return p;
    std::new_handler handler = std::get_new_handler();
    if (!handler) throw std::bad_alloc();
    handler();
  }
}

void* AllocateAligned(std::size_t size, std::align_val_t alignment) {
  t_counts.bytes += size, ++t_counts.count;
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
AllocCounts ThreadAllocations() { return t_counts; }
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
