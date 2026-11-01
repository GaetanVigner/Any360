// The memory manager: the guest address space's heaps as the Xbox 360 kernel lays them out (virtual 4 KB pages below
// 0x40000000, 64 KB pages from 0x40000000; the physical views at 0xA0000000 (64 KB), 0xC0000000 (16 MB) and
// 0xE0000000 (4 KB) over one 512 MB physical space; the executable's own range is not allocatable), with per-page
// state (free, reserved, committed), protection and the region a page belongs to. Addresses are the manager's own:
// a title gets consistent regions, not the reference's numbers.
#pragma once

#include <cstdint>
#include <vector>

#include "guest.h"

namespace any360::sys {

// Allocation types and protections, as the XDK defines them.
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
                   kMemFree = 0x10000, kMemPrivate = 0x20000, kMemReset = 0x80000, kMemTopDown = 0x100000,
                   kMemNoZero = 0x800000, kMemLargePages = 0x20000000, kMem16MbPages = 0x80000000;
constexpr uint32_t kPageNoAccess = 0x1, kPageReadOnly = 0x2, kPageReadWrite = 0x4, kPageExecuteRead = 0x20,
                   kPageExecuteReadWrite = 0x40, kPageNoCache = 0x200, kPageWriteCombine = 0x400;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001, kStatusAccessDenied = 0xC0000022, kStatusMemoryNotAllocated = 0xC00000A0;

struct HeapSpec {
  uint32_t base, size, page_size;
  bool physical;
};

struct RegionInfo {
  uint32_t base_address = 0, allocation_base = 0, allocation_protect = 0, allocation_size = 0, region_size = 0;
  uint32_t state = 0;  // kMemCommit, kMemReserve or kMemFree
  uint32_t protect = 0;
};

class MemoryManager {
 public:
  MemoryManager();
  // The Xbox 360 layout, or a layout of the caller's (tests).
  void Reset();
  void Reset(const std::vector<HeapSpec>& heaps);

  // NtAllocateVirtualMemory: in/out base and size through guest pointers.
  uint32_t AllocateVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t allocation_type, uint32_t protect,
                           uint32_t debug_memory);
  uint32_t FreeVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t free_type, uint32_t debug_memory);
  uint32_t QueryVirtual(Guest g, uint32_t address, uint32_t info_ptr, uint32_t region_type);
  // MmAllocatePhysicalMemoryEx: the address, or 0.
  uint32_t AllocatePhysical(Guest g, uint32_t flags, uint32_t size, uint32_t protect, uint32_t min_address,
                            uint32_t max_address, uint32_t alignment);
  void FreePhysical(uint32_t type, uint32_t address);
  uint32_t PhysicalAddress(uint32_t address) const;
  uint32_t QueryProtect(uint32_t address) const;
  void SetProtect(uint32_t address, uint32_t size, uint32_t protect);
  uint32_t QueryAllocationSize(uint32_t address) const;
  uint32_t QueryStatistics(Guest g, uint32_t stats_ptr) const;
  // XamAlloc / XamFree: committed 4 KB pages, zeroed.
  uint32_t XamAlloc(Guest g, uint32_t flags, uint32_t size, uint32_t out_ptr);
  uint32_t XamFree(Guest g, uint32_t address);

  bool QueryRegion(uint32_t address, RegionInfo& info) const;
  // Committed, zeroed read-write pages from the 4 KB heap (the kernel pool, strings the kernel allocates); 0 when full.
  uint32_t AllocatePages(Guest g, uint32_t size);
  void FreePages(uint32_t address);

 private:
  struct Page {
    uint32_t region_start = 0, region_pages = 0;  // page numbers within the heap
    uint8_t state = 0;                            // 0 free, 1 reserved, 3 committed
    uint16_t allocation_protect = 0, protect = 0;  // XDK flags
  };
  struct Heap {
    HeapSpec spec;
    uint32_t shift = 0;
    std::vector<Page> pages;
  };
  const Heap* Find(uint32_t address) const;
  Heap* Find(uint32_t address) { return const_cast<Heap*>(const_cast<const MemoryManager*>(this)->Find(address)); }
  Heap* ByPageSize(bool physical, uint32_t page_size);
  bool AllocRange(Heap& heap, uint32_t low, uint32_t high, uint32_t size, uint32_t alignment, uint8_t state,
                  uint32_t protect, bool top_down, uint32_t& address);
  bool AllocFixed(Heap& heap, uint32_t address, uint32_t size, uint8_t state, uint32_t protect);
  bool Release(Heap& heap, uint32_t address, uint32_t* size);
  bool Decommit(Heap& heap, uint32_t address, uint32_t size);
  bool PhysicalFree(uint32_t physical, uint32_t size) const;
  void MarkPhysical(uint32_t physical, uint32_t size, bool used);

  std::vector<Heap> heaps_;
  std::vector<uint8_t> physical_pages_;  // 4 KB pages of the physical space, 1 = in use
  uint32_t physical_space_ = 0;
};

MemoryManager& Memory();

}  // namespace any360::sys
