// The memory manager: the guest address space's heaps as the Xbox 360 kernel lays them out (virtual 4 KB pages below
// 0x40000000, 64 KB pages from 0x40000000; the physical views at 0xA0000000 (64 KB), 0xC0000000 (16 MB) and
// 0xE0000000 (4 KB) over one 512 MB physical space; the executable's own range is not allocatable except its
// encrypted part at 0x8C000000, 64 KB pages only NtAllocateEncryptedMemory hands out), with per-page
// state (free, reserved, committed), protection and the region a page belongs to. Addresses are the manager's own:
// a title gets consistent regions, not the reference's numbers.
#pragma once

#include <cstdint>
#include <mutex>
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
  bool encrypted = false;  // NtAllocateEncryptedMemory's alone: no other allocation lands there
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
  // NtProtectVirtualMemory: in/out base and size (rounded out to whole pages); the old protection of the first page
  // written. Every page must be committed (STATUS_NOT_COMMITTED); execute rights are not granted to data
  // (STATUS_INVALID_PAGE_PROTECTION), as the reference does.
  uint32_t ProtectVirtual(Guest g, uint32_t base_ptr, uint32_t size_ptr, uint32_t protect, uint32_t old_protect_ptr);
  uint32_t QueryAllocationSize(uint32_t address) const;
  uint32_t QueryStatistics(Guest g, uint32_t stats_ptr) const;
  // XamAlloc / XamFree: committed 4 KB pages, zeroed.
  uint32_t XamAlloc(Guest g, uint32_t flags, uint32_t size, uint32_t out_ptr);
  uint32_t XamFree(Guest g, uint32_t address);

  bool QueryRegion(uint32_t address, RegionInfo& info) const;
  // Another allocator sharing the guest address space (a binder's runtime, which goes on allocating its own
  // structures): every range handed out here is claimed there first and released there when released here. A refused
  // claim (the other side holds part of the range) leaves the range to it: the binder marks what it holds with
  // MarkForeign, and the search goes on elsewhere. Unset, the manager owns the address space alone.
  struct SharedOwner {
    bool (*claim)(uint32_t address, uint32_t size, uint32_t protect, void* user) = nullptr;
    void (*release)(uint32_t address, void* user) = nullptr;
    void* user = nullptr;
  };
  void SetSharedOwner(const SharedOwner& owner);
  // Pages the other allocator holds: committed read-write, a region of their own, never handed out here.
  void MarkForeign(uint32_t address, uint32_t size);
  // MmIsAddressValid: whether the page is committed and accessible.
  uint32_t IsAddressValid(uint32_t address) const;
  // MmCreateKernelStack / MmDeleteKernelStack: committed read-write pages from the 4 KB heap; Create returns the stack's
  // top (its base address plus the size), Delete takes the low end back (STATUS_UNSUCCESSFUL for anything else).
  uint32_t CreateKernelStack(Guest g, uint32_t size);
  // NtAllocateEncryptedMemory: committed read-write 64 KB pages of the encrypted heap, the size rounded up to 64 KB
  // (16 MB at most); NtFreeEncryptedMemory takes back the allocation at the address `base_ptr` holds.
  uint32_t AllocateEncrypted(Guest g, uint32_t size, uint32_t base_out);
  uint32_t FreeEncrypted(Guest g, uint32_t base_ptr);
  // KeGetImagePageTableEntry: an address of the executable's range as its page number (64 KB pages from 0x80000000,
  // 4 KB pages from 0x90000000 flagged 0x40000000), masked to 0x400FFFFF; 0 outside it.
  static uint32_t ImagePageTableEntry(uint32_t address);
  uint32_t DeleteKernelStack(uint32_t stack_end);
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
  // The pages [start, start + pages) of `heap` claimed from the shared owner (true without one).
  bool Claim(Heap& heap, uint32_t start, uint32_t pages, uint32_t protect);
  bool Release(Heap& heap, uint32_t address, uint32_t* size);
  bool Decommit(Heap& heap, uint32_t address, uint32_t size);
  bool PhysicalFree(uint32_t physical, uint32_t size) const;
  void MarkPhysical(uint32_t physical, uint32_t size, bool used);

  std::vector<Heap> heaps_;
  std::vector<uint8_t> physical_pages_;  // 4 KB pages of the physical space, 1 = in use
  uint32_t physical_space_ = 0;
  SharedOwner owner_;
  mutable std::recursive_mutex mutex_;  // the public calls, from any thread
};

MemoryManager& Memory();

}  // namespace any360::sys
