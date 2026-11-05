#include "any360/sys/memory.h"

#include <algorithm>
#include <cstring>

namespace any360::sys {

namespace {

constexpr uint32_t kStatusSuccess = 0, kStatusInvalidParameter = 0xC000000D, kStatusNoMemory = 0xC0000017,
                   kStatusBufferTooSmall = 0xC0000023;
constexpr uint8_t kFree = 0, kReserved = 1, kCommitted = 3;

uint32_t RoundUp(uint32_t value, uint32_t to) { return (value + to - 1) / to * to; }

// The protection the manager keeps: NOACCESS / READONLY / READWRITE plus the cache bits, as the kernel reports it.
uint32_t Normalize(uint32_t protect) {
  uint32_t result;
  if (protect & (kPageReadOnly | kPageExecuteRead)) result = kPageReadOnly;
  else if (protect & (kPageReadWrite | kPageExecuteReadWrite)) result = kPageReadWrite;
  else result = kPageNoAccess;
  return result | (protect & (kPageNoCache | kPageWriteCombine));
}

}  // namespace

MemoryManager& Memory() {
  static MemoryManager manager;
  return manager;
}

MemoryManager::MemoryManager() { Reset(); }

void MemoryManager::Reset() {
  Reset({{0x00000000, 0x40000000, 0x1000, false},
         {0x40000000, 0x3F000000, 0x10000, false},
         {0xA0000000, 0x20000000, 0x10000, true},
         {0xC0000000, 0x20000000, 0x1000000, true},
         {0xE0000000, 0x1FD00000, 0x1000, true},
         {0x8C000000, 0x04000000, 0x10000, false, true}});
}

void MemoryManager::Reset(const std::vector<HeapSpec>& specs) {
  heaps_.clear();
  physical_space_ = 0;
  for (const HeapSpec& spec : specs) {
    Heap heap;
    heap.spec = spec;
    heap.shift = uint32_t(std::countr_zero(spec.page_size));
    heap.pages.resize(spec.size / spec.page_size);
    heaps_.push_back(std::move(heap));
    if (spec.physical) physical_space_ = std::max(physical_space_, spec.size + (spec.base >= 0xE0000000 ? 0x1000u : 0u));
  }
  physical_pages_.assign(physical_space_ / 0x1000 + 1, 0);
}

const MemoryManager::Heap* MemoryManager::Find(uint32_t address) const {
  for (const Heap& heap : heaps_) {
    if (address >= heap.spec.base && address - heap.spec.base < heap.spec.size) return &heap;
  }
  return nullptr;
}

MemoryManager::Heap* MemoryManager::ByPageSize(bool physical, uint32_t page_size) {
  for (Heap& heap : heaps_) {
    if (heap.spec.physical == physical && !heap.spec.encrypted && heap.spec.page_size == page_size) return &heap;
  }
  return nullptr;
}

uint32_t MemoryManager::PhysicalAddress(uint32_t address) const {
  const Heap* heap = Find(address);
  if (!heap || !heap->spec.physical) return 0xFFFFFFFF;
  return address - heap->spec.base + (heap->spec.base >= 0xE0000000 ? 0x1000 : 0);
}

bool MemoryManager::PhysicalFree(uint32_t physical, uint32_t size) const {
  for (uint32_t p = physical / 0x1000; p < (physical + size) / 0x1000 && p < physical_pages_.size(); ++p) {
    if (physical_pages_[p]) return false;
  }
  return true;
}

void MemoryManager::MarkPhysical(uint32_t physical, uint32_t size, bool used) {
  for (uint32_t p = physical / 0x1000; p < (physical + size) / 0x1000 && p < physical_pages_.size(); ++p) {
    physical_pages_[p] = used ? 1 : 0;
  }
}

bool MemoryManager::AllocRange(Heap& heap, uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, uint8_t state,
                               uint32_t protect, bool top_down, uint32_t& address) {
  const uint32_t pages = size >> heap.shift, align_pages = std::max<uint32_t>(1, alignment >> heap.shift);
  const uint32_t first = (low - heap.spec.base) >> heap.shift, last = (high - heap.spec.base) >> heap.shift;
  if (pages == 0 || pages > heap.pages.size()) return false;
  auto fits = [&](uint32_t start) {
    if (start + pages > heap.pages.size() || start + pages - 1 > last) return false;
    for (uint32_t p = start; p < start + pages; ++p) {
      if (heap.pages[p].state != kFree) return false;
    }
    if (heap.spec.physical) {
      const uint32_t physical = PhysicalAddress(heap.spec.base + (start << heap.shift));
      if (!PhysicalFree(physical, size)) return false;
    }
    return true;
  };
  auto take = [&](uint32_t start) {
    for (uint32_t p = start; p < start + pages; ++p) {
      heap.pages[p] = {start, pages, state, uint16_t(protect), uint16_t(protect)};
    }
    if (heap.spec.physical) MarkPhysical(PhysicalAddress(heap.spec.base + (start << heap.shift)), size, true);
    address = heap.spec.base + (start << heap.shift);
    return true;
  };
  // Heaps that do not start at zero skip their first page, as the kernel does, so no allocation is at a heap's base
  // that could be mistaken for a null or an image address; the low heap skips page 0 (null).
  const uint32_t start_min = std::max<uint32_t>(first, 1);
  if (top_down) {
    uint32_t start = std::min<uint32_t>(last, uint32_t(heap.pages.size() - 1));
    start = start + 1 >= pages ? (start + 1 - pages) / align_pages * align_pages : 0;
    for (;; start -= align_pages) {
      if (start >= start_min && fits(start)) return take(start);
      if (start < align_pages || start < start_min) break;
    }
    return false;
  }
  for (uint32_t start = RoundUp(start_min, align_pages); start + pages <= heap.pages.size(); start += align_pages) {
    if (fits(start)) return take(start);
  }
  return false;
}

bool MemoryManager::AllocFixed(Heap& heap, uint32_t address, uint32_t size, uint8_t state, uint32_t protect) {
  const uint32_t start = (address - heap.spec.base) >> heap.shift, pages = size >> heap.shift;
  if (start + pages > heap.pages.size() || pages == 0) return false;
  // Reserving needs free pages; committing needs free or reserved ones (committed pages stay as they are).
  for (uint32_t p = start; p < start + pages; ++p) {
    if (state == kReserved && heap.pages[p].state != kFree) return false;
  }
  for (uint32_t p = start; p < start + pages; ++p) {
    Page& page = heap.pages[p];
    if (page.state == kFree) {
      page = {start, pages, state, uint16_t(protect), uint16_t(protect)};
    } else if (state == kCommitted && page.state == kReserved) {
      page.state = kCommitted;
      page.protect = uint16_t(protect);
    }
  }
  return true;
}

bool MemoryManager::Release(Heap& heap, uint32_t address, uint32_t* size) {
  const uint32_t start = (address - heap.spec.base) >> heap.shift;
  if (start >= heap.pages.size() || heap.pages[start].state == kFree || heap.pages[start].region_start != start) return false;
  if (heap.spec.base == 0 && start == 0) return false;
  const uint32_t pages = heap.pages[start].region_pages;
  if (size) *size = pages << heap.shift;
  if (heap.spec.physical) MarkPhysical(PhysicalAddress(address), pages << heap.shift, false);
  for (uint32_t p = start; p < start + pages && p < heap.pages.size(); ++p) heap.pages[p] = {};
  return true;
}

bool MemoryManager::Decommit(Heap& heap, uint32_t address, uint32_t size) {
  const uint32_t start = (address - heap.spec.base) >> heap.shift, pages = size >> heap.shift;
  if (start + pages > heap.pages.size()) return false;
  for (uint32_t p = start; p < start + pages; ++p) {
    if (heap.pages[p].state == kCommitted) heap.pages[p].state = kReserved;
  }
  return true;
}

bool MemoryManager::QueryRegion(uint32_t address, RegionInfo& info) const {
  const Heap* heap = Find(address);
  if (!heap) return false;
  const uint32_t start = (address - heap->spec.base) >> heap->shift;
  const Page& first = heap->pages[start];
  info = {};
  info.base_address = address;
  if (first.state != kFree) {
    info.allocation_base = heap->spec.base + (first.region_start << heap->shift);
    info.allocation_protect = first.allocation_protect;
    info.allocation_size = first.region_pages << heap->shift;
    info.state = first.state == kCommitted ? kMemCommit : kMemReserve;
    info.protect = first.protect;
    for (uint32_t p = start; p < first.region_start + first.region_pages && p < heap->pages.size(); ++p) {
      const Page& page = heap->pages[p];
      if (page.region_start != first.region_start || page.state != first.state || page.protect != first.protect) break;
      info.region_size += heap->spec.page_size;
    }
  } else {
    info.state = kMemFree;
    for (uint32_t p = start; p < heap->pages.size() && heap->pages[p].state == kFree; ++p) info.region_size += heap->spec.page_size;
  }
  return true;
}

uint32_t MemoryManager::AllocateVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t allocation_type, uint32_t protect,
                                        uint32_t) {
  if (!base_ptr || !size_ptr || !g.Load32(size_ptr)) return kStatusInvalidParameter;
  if (!(allocation_type & (kMemCommit | kMemReset | kMemReserve))) return kStatusInvalidParameter;
  if ((allocation_type & kMemReset) && (allocation_type & ~kMemReset)) return kStatusInvalidParameter;
  const uint32_t base = g.Load32(base_ptr), requested = g.Load32(size_ptr);
  uint32_t page_size;
  Heap* heap = nullptr;
  if (base) {
    heap = Find(base);
    if (!heap || heap->spec.physical || heap->spec.encrypted) return kStatusInvalidParameter;
    page_size = heap->spec.page_size;
  } else {
    page_size = (allocation_type & kMemLargePages) ? 0x10000 : 0x1000;
    heap = ByPageSize(false, page_size);
    if (!heap) return kStatusInvalidParameter;
  }
  const uint32_t adjusted_base = base - base % page_size;
  const uint32_t size = RoundUp(int32_t(requested) < 0 ? uint32_t(-int32_t(requested)) : requested, adjusted_base ? page_size : 0x10000);
  const uint8_t state = (allocation_type & kMemCommit) ? kCommitted : kReserved;
  const uint32_t kept = Normalize(protect);
  uint32_t address = 0;
  bool was_committed = false;
  if (adjusted_base) {
    RegionInfo previous;
    was_committed = QueryRegion(adjusted_base, previous) && previous.state == kMemCommit;
    if (!AllocFixed(*heap, adjusted_base, size, state, kept)) return kStatusNoMemory;
    address = adjusted_base;
  } else if (!AllocRange(*heap, heap->spec.base, heap->spec.base + heap->spec.size - 1, size, page_size, state, kept,
                         (allocation_type & kMemTopDown) != 0, address)) {
    return kStatusNoMemory;
  }
  if ((allocation_type & kMemCommit) && !(allocation_type & kMemNoZero) && !was_committed) std::memset(g.host(address), 0, size);
  g.Store32(base_ptr, address);
  g.Store32(size_ptr, size);
  return kStatusSuccess;
}

uint32_t MemoryManager::FreeVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t free_type, uint32_t) {
  if (!base_ptr || !size_ptr) return kStatusInvalidParameter;
  uint32_t address = g.Load32(base_ptr), size = g.Load32(size_ptr);
  if (!address) return kStatusMemoryNotAllocated;
  Heap* heap = Find(address);
  if (!heap || heap->spec.physical) return kStatusInvalidParameter;
  bool ok;
  if (free_type == kMemDecommit) {
    size = RoundUp(size, heap->spec.page_size);
    ok = Decommit(*heap, address, size);
  } else {
    ok = Release(*heap, address, &size);
  }
  if (!ok) return kStatusUnsuccessful;
  g.Store32(base_ptr, address);
  g.Store32(size_ptr, size);
  return kStatusSuccess;
}

uint32_t MemoryManager::QueryVirtual(Guest g, uint32_t address, uint32_t info_ptr, uint32_t region_type) {
  if (region_type > 2) return kStatusInvalidParameter;
  RegionInfo info;
  if (!QueryRegion(address, info)) return kStatusInvalidParameter;
  g.Store32(info_ptr + 0, info.base_address);
  g.Store32(info_ptr + 4, info.allocation_base);
  g.Store32(info_ptr + 8, info.allocation_protect);
  g.Store32(info_ptr + 12, info.region_size);
  g.Store32(info_ptr + 16, info.state);
  g.Store32(info_ptr + 20, info.protect);
  g.Store32(info_ptr + 24, kMemPrivate);
  return kStatusSuccess;
}

uint32_t MemoryManager::AllocatePhysical(Guest g, uint32_t, uint32_t size, uint32_t protect, uint32_t min_address,
                                         uint32_t max_address, uint32_t alignment) {
  if (!(protect & (kPageReadOnly | kPageReadWrite))) return 0;
  const uint32_t page_size = (protect & kMemLargePages) ? 0x10000 : (protect & kMem16MbPages) ? 0x1000000 : 0x1000;
  Heap* heap = ByPageSize(true, page_size);
  if (!heap) return 0;
  const uint32_t adjusted_size = RoundUp(size, page_size), adjusted_alignment = RoundUp(alignment, page_size);
  // The range is given in physical addresses; the heap's view starts at its own physical offset.
  const uint32_t offset = PhysicalAddress(heap->spec.base);
  const uint32_t low = heap->spec.base + std::min(min_address > offset ? min_address - offset : 0, heap->spec.size - 1);
  const uint32_t high = heap->spec.base + std::min(max_address > offset ? max_address - offset : 0, heap->spec.size - 1);
  uint32_t address = 0;
  if (!AllocRange(*heap, low, high, adjusted_size, adjusted_alignment, kCommitted, Normalize(protect), true, address)) return 0;
  std::memset(g.host(address), 0, adjusted_size);
  return address;
}

void MemoryManager::FreePhysical(uint32_t, uint32_t address) {
  if (Heap* heap = Find(address)) Release(*heap, address, nullptr);
}

uint32_t MemoryManager::QueryProtect(uint32_t address) const {
  const Heap* heap = Find(address);
  if (!heap) return 0;
  const Page& page = heap->pages[(address - heap->spec.base) >> heap->shift];
  return page.state == kFree ? 0 : page.protect;
}

void MemoryManager::SetProtect(uint32_t address, uint32_t size, uint32_t protect) {
  const uint32_t required = kPageNoAccess | kPageReadOnly | kPageReadWrite | kPageExecuteRead | kPageExecuteReadWrite;
  if (std::popcount(protect & required) != 1) return;
  Heap* heap = Find(address);
  if (!heap) return;
  const uint32_t start = (address - heap->spec.base) >> heap->shift, pages = RoundUp(size, heap->spec.page_size) >> heap->shift;
  for (uint32_t p = start; p < start + pages && p < heap->pages.size(); ++p) {
    if (heap->pages[p].state != kFree) heap->pages[p].protect = uint16_t(Normalize(protect));
  }
}

uint32_t MemoryManager::ProtectVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t protect, uint32_t old_protect_ptr) {
  constexpr uint32_t kStatusNotCommitted = 0xC000002D, kStatusInvalidPageProtection = 0xC0000045;
  if (!base_ptr || !size_ptr || !g.Load32(size_ptr)) return kStatusInvalidParameter;
  if (protect & (kPageExecuteRead | kPageExecuteReadWrite | 0x10 | 0x80)) return kStatusInvalidPageProtection;
  const uint32_t required = kPageNoAccess | kPageReadOnly | kPageReadWrite;
  if (std::popcount(protect & required) != 1) return kStatusInvalidPageProtection;
  const uint32_t address = g.Load32(base_ptr);
  Heap* heap = Find(address);
  if (!heap || heap->spec.physical) return kStatusInvalidParameter;
  const uint32_t start = (address - heap->spec.base) >> heap->shift;
  const uint64_t end_address = uint64_t(address) + g.Load32(size_ptr);
  const uint32_t end = uint32_t((end_address - heap->spec.base + heap->spec.page_size - 1) >> heap->shift);
  if (end > heap->pages.size()) return kStatusNotCommitted;
  for (uint32_t p = start; p < end; ++p) {
    if (heap->pages[p].state != kCommitted) return kStatusNotCommitted;
  }
  if (old_protect_ptr) g.Store32(old_protect_ptr, heap->pages[start].protect);
  for (uint32_t p = start; p < end; ++p) heap->pages[p].protect = uint16_t(Normalize(protect));
  g.Store32(base_ptr, heap->spec.base + (start << heap->shift));
  g.Store32(size_ptr, (end - start) << heap->shift);
  return kStatusSuccess;
}

uint32_t MemoryManager::QueryAllocationSize(uint32_t address) const {
  const Heap* heap = Find(address);
  if (!heap) return 0;
  const Page& page = heap->pages[(address - heap->spec.base) >> heap->shift];
  return page.state == kFree ? 0 : page.region_pages << heap->shift;
}

uint32_t MemoryManager::QueryStatistics(Guest g, uint32_t stats) const {
  if (!stats) return kStatusInvalidParameter;
  if (g.Load32(stats) != 104) return kStatusBufferTooSmall;
  uint32_t used_physical_pages = 0, reserved_virtual_bytes = 0;
  for (uint8_t p : physical_pages_) used_physical_pages += p;
  for (const Heap& heap : heaps_) {
    if (heap.spec.physical) continue;
    for (const Page& page : heap.pages) {
      if (page.state != kFree) reserved_virtual_bytes += heap.spec.page_size;
    }
  }
  std::memset(g.host(stats), 0, 104);
  g.Store32(stats + 0, 104);
  g.Store32(stats + 4, 0x20000);   // 512 MB of 4 KB pages
  g.Store32(stats + 8, 0x100);     // kernel pages
  g.Store32(stats + 12, 0x20000 - 0x100 - used_physical_pages);  // title: available pages
  g.Store32(stats + 16, 0x2FFE0000);
  g.Store32(stats + 20, reserved_virtual_bytes);
  g.Store32(stats + 24, 0x1000);
  for (uint32_t field = 28; field <= 56; field += 4) g.Store32(stats + field, field == 28 ? 0x10 : 0x100);
  return kStatusSuccess;
}

uint32_t MemoryManager::XamAlloc(Guest g, uint32_t, uint32_t size, uint32_t out_ptr) {
  if (!out_ptr) return kStatusInvalidParameter;
  Heap* heap = ByPageSize(false, 0x1000);
  uint32_t address = 0;
  if (!heap || !AllocRange(*heap, heap->spec.base, heap->spec.base + heap->spec.size - 1, RoundUp(std::max<uint32_t>(size, 1), 0x1000),
                           0x1000, kCommitted, kPageReadWrite, false, address)) {
    return kStatusNoMemory;
  }
  std::memset(g.host(address), 0, RoundUp(std::max<uint32_t>(size, 1), 0x1000));
  g.Store32(out_ptr, address);
  return kStatusSuccess;
}

uint32_t MemoryManager::AllocatePages(Guest g, uint32_t size) {
  Heap* heap = ByPageSize(false, 0x1000);
  const uint32_t bytes = RoundUp(std::max<uint32_t>(size, 1), 0x1000);
  uint32_t address = 0;
  if (!heap || !AllocRange(*heap, heap->spec.base, heap->spec.base + heap->spec.size - 1, bytes, 0x1000, kCommitted, kPageReadWrite,
                           false, address)) {
    return 0;
  }
  std::memset(g.host(address), 0, bytes);
  return address;
}

void MemoryManager::FreePages(uint32_t address) {
  if (Heap* heap = Find(address)) Release(*heap, address, nullptr);
}

uint32_t MemoryManager::XamFree(Guest, uint32_t address) {
  if (Heap* heap = Find(address)) Release(*heap, address, nullptr);
  return kStatusSuccess;
}

uint32_t MemoryManager::IsAddressValid(uint32_t address) const {
  RegionInfo info;
  return QueryRegion(address, info) && info.state == kMemCommit && info.protect && !(info.protect & 0x01 /* PAGE_NOACCESS */) ? 1 : 0;
}

uint32_t MemoryManager::CreateKernelStack(Guest g, uint32_t size) {
  const uint32_t aligned = (size + 0xFFF) & ~0xFFFu;
  const uint32_t base = AllocatePages(g, aligned);
  return base ? base + aligned : 0;
}

uint32_t MemoryManager::DeleteKernelStack(uint32_t stack_end) {
  if (!QueryAllocationSize(stack_end)) return 0xC0000001;
  FreePages(stack_end);
  return 0;
}

uint32_t MemoryManager::AllocateEncrypted(Guest g, uint32_t size, uint32_t base_out) {
  if (!size) return kStatusInvalidParameter;
  const uint32_t bytes = RoundUp(size, 0x10000);
  if (bytes > 0x1000000) return kStatusInvalidParameter;
  for (Heap& heap : heaps_) {
    if (!heap.spec.encrypted) continue;
    uint32_t address = 0;
    if (!AllocRange(heap, heap.spec.base, heap.spec.base + heap.spec.size - 1, bytes, 0x10000, kCommitted, kPageReadWrite, false,
                    address)) {
      return kStatusNoMemory;
    }
    std::memset(g.host(address), 0, bytes);
    if (base_out) g.Store32(base_out, address);
    return 0;
  }
  return kStatusUnsuccessful;
}

uint32_t MemoryManager::FreeEncrypted(Guest g, uint32_t base_ptr) {
  if (!base_ptr) return kStatusInvalidParameter;
  const uint32_t address = g.Load32(base_ptr);
  const Heap* heap = Find(address);
  if (!heap || !heap->spec.encrypted || !QueryAllocationSize(address)) return kStatusInvalidParameter;
  FreePages(address);
  return 0;
}

uint32_t MemoryManager::ImagePageTableEntry(uint32_t address) {
  if (address >= 0x80000000 && address < 0x90000000) return ((address - 0x80000000) >> 16) & 0x400FFFFF;
  if (address >= 0x90000000 && address < 0xA0000000) return (((address - 0x90000000) >> 12) | 0x40000000) & 0x400FFFFF;
  return 0;
}

}  // namespace any360::sys
