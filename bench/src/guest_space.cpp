#include "guest_space.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace any360::bench {

namespace {

constexpr uint64_t kReserved = 0x100010000ull;
constexpr uint32_t kVirtualEnd = 0xA0000000, kPage = 0x1000, kCommitChunk = 0x10000;
constexpr uint32_t kViewBases[3] = {0xA0000000, 0xC0000000, 0xE0000000};

}  // namespace

GuestSpace& GuestSpace::Get() {
  static GuestSpace space;
  return space;
}

bool GuestSpace::Create() {
  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kPhysicalBytes, nullptr);
  if (!mapping_) return false;
  for (int attempt = 0; attempt < 16 && !base_; ++attempt) {
    auto* probe = static_cast<uint8_t*>(VirtualAlloc(nullptr, kReserved, MEM_RESERVE, PAGE_NOACCESS));
    if (!probe) return false;
    VirtualFree(probe, 0, MEM_RELEASE);
    if (VirtualAlloc(probe, kVirtualEnd, MEM_RESERVE, PAGE_NOACCESS) != probe) continue;
    bool mapped = true;
    for (int v = 0; v < 3 && mapped; ++v) {
      views_[v] = static_cast<uint8_t*>(MapViewOfFileEx(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalBytes, probe + kViewBases[v]));
      mapped = views_[v] != nullptr;
    }
    if (!mapped) {
      for (uint8_t*& view : views_) {
        if (view) UnmapViewOfFile(view), view = nullptr;
      }
      VirtualFree(probe, 0, MEM_RELEASE);
      continue;
    }
    base_ = probe;
  }
  if (!base_) return false;
  gpu_view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalBytes));
  if (!gpu_view_) return false;
  watched_ = new std::atomic<uint8_t>[kPhysicalBytes / kPage]();
  AddVectoredExceptionHandler(1, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(&GuestSpace::Handler));
  return true;
}

void GuestSpace::Watch(uint32_t begin, uint32_t end) {
  begin &= ~(kPage - 1);
  end = std::min<uint32_t>(kPhysicalBytes, (end + kPage - 1) & ~(kPage - 1));
  uint32_t run = UINT32_MAX;
  auto protect = [&](uint32_t from, uint32_t to) {
    DWORD old;
    for (uint8_t* view : views_) VirtualProtect(view + from, to - from, PAGE_READONLY, &old);
  };
  for (uint32_t p = begin; p < end; p += kPage) {
    const bool armed = watched_[p / kPage].exchange(1) == 0;
    if (armed && run == UINT32_MAX) run = p;
    if (!armed && run != UINT32_MAX) protect(run, p), run = UINT32_MAX;
  }
  if (run != UINT32_MAX) protect(run, end);
}

bool GuestSpace::OnFault(uint8_t* address, bool write) {
  if (address >= base_ && address < base_ + kVirtualEnd) {
    uint8_t* chunk = base_ + ((address - base_) & ~uint64_t(kCommitChunk - 1));
    if (!VirtualAlloc(chunk, kCommitChunk, MEM_COMMIT, PAGE_READWRITE)) return false;
    committed_.fetch_add(kCommitChunk / kPage, std::memory_order_relaxed);
    return true;
  }
  if (!write) return false;
  for (uint8_t* view : views_) {
    if (address < view || address >= view + kPhysicalBytes) continue;
    const uint32_t page = uint32_t(address - view) & ~(kPage - 1);
    if (watched_[page / kPage].exchange(0) == 1) {
      DWORD old;
      for (uint8_t* v : views_) VirtualProtect(v + page, kPage, PAGE_READWRITE, &old);
      faults_.fetch_add(1, std::memory_order_relaxed);
      if (hook_) hook_(page, page + kPage);
      return true;
    }
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) && info.Protect == PAGE_READWRITE;
  }
  return false;
}

long __stdcall GuestSpace::Handler(void* raw) {
  auto* info = static_cast<EXCEPTION_POINTERS*>(raw);
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  const bool write = info->ExceptionRecord->ExceptionInformation[0] == 1;
  auto* address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
  return Get().OnFault(address, write) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace any360::bench
