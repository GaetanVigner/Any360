// Guest address spaces for the CPU translator's tests: 4 GB each, committed page by page on first touch, every page
// filled on commit with the same words in every space (a function of its address and the space's seed). Two spaces
// run the two versions of a function on the same memory; MemoryDiffers() then reads every page either one touched.
// Refill() writes the first words back into every page touched, keeping them committed.
//
// A page's words, chosen by a hash of their address: 0 (6 in 16), a small value (0-7, 0-63, 1, -1: 5 in 16), or a
// pointer into the data window (kWindow, 64 KB, word-aligned: 5 in 16). Registers draw from the same values.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#endif

namespace any360::cpu::test {

constexpr uint64_t kSpaceSize = 0x100000000ull + 0x1000;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kPages = uint32_t((kSpaceSize + kPage - 1) / kPage);
constexpr uint32_t kWindow = 0x40000000u, kWindowSize = 0x10000u;
constexpr uint32_t kStackTop = 0x70010000u;

inline uint32_t Hash(uint32_t x, uint32_t seed) {
  x ^= seed * 0x9E3779B9u;
  x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
  return x;
}

inline uint32_t DataWord(uint32_t h) {
  switch (h & 15) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
      return 0;
    case 6:
    case 7:
      return (h >> 8) & 7;
    case 8:
      return (h >> 8) & 63;
    case 9:
      return 1;
    case 10:
      return 0xFFFFFFFFu;
    default:
      return kWindow + ((h >> 8) & (kWindowSize - 4) & ~3u);
  }
}

class Space {
 public:
  explicit Space(uint32_t seed) : seed_(seed), committed_(kPages / 8) {
#if defined(_WIN32)
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpaceSize, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* mapping = mmap(nullptr, kSpaceSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base_ = mapping == MAP_FAILED ? nullptr : static_cast<uint8_t*>(mapping);
#endif
    if (!base_) {
      std::printf("cpu space: cannot reserve a guest address space\n");
      std::exit(2);
    }
    touched_.reserve(1 << 16);
    Register(this);
  }
  Space(const Space&) = delete;
  Space& operator=(const Space&) = delete;

  uint8_t* base() const { return base_; }
  const std::vector<uint32_t>& touched() const { return touched_; }
  bool Committed(uint32_t page) const { return committed_[page >> 3] & (1u << (page & 7)); }

  void Commit(uint32_t page) {
    uint8_t* host = base_ + uint64_t(page) * kPage;
#if defined(_WIN32)
    VirtualAlloc(host, kPage, MEM_COMMIT, PAGE_READWRITE);
#else
    mprotect(host, kPage, PROT_READ | PROT_WRITE);
#endif
    Fill(page);
    committed_[page >> 3] |= uint8_t(1u << (page & 7));
    touched_.push_back(page);
  }

  void Refill() {
    for (uint32_t page : touched_) Fill(page);
  }

  void Reset() {
    for (uint32_t page : touched_) {
      uint8_t* host = base_ + uint64_t(page) * kPage;
#if defined(_WIN32)
      VirtualFree(host, kPage, MEM_DECOMMIT);
#else
      madvise(host, kPage, MADV_DONTNEED);
      mprotect(host, kPage, PROT_NONE);
#endif
      committed_[page >> 3] = 0;
    }
    touched_.clear();
  }

  uint8_t* Page(uint32_t page) {
    if (!Committed(page)) Commit(page);
    return base_ + uint64_t(page) * kPage;
  }

  static bool Owns(uint8_t* address, Space** owner) {
    for (int i = 0; i < 4; ++i) {
      Space* s = spaces()[i];
      if (s && address >= s->base_ && address < s->base_ + kSpaceSize) {
        *owner = s;
        return true;
      }
    }
    return false;
  }

 private:
  void Fill(uint32_t page) {
    uint8_t* host = base_ + uint64_t(page) * kPage;
    const uint32_t address = page * kPage;
    for (uint32_t i = 0; i < kPage; i += 4) {
      const uint32_t w = DataWord(Hash(address + i, seed_));
      host[i] = uint8_t(w >> 24), host[i + 1] = uint8_t(w >> 16), host[i + 2] = uint8_t(w >> 8), host[i + 3] = uint8_t(w);
    }
  }

  static Space** spaces() {
    static Space* list[4] = {};
    return list;
  }
  static void Register(Space* space) {
    static bool installed = false;
    for (int i = 0; i < 4; ++i) {
      if (!spaces()[i]) {
        spaces()[i] = space;
        break;
      }
    }
    if (installed) return;
    installed = true;
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, &Space::Handler);
#else
    struct sigaction action = {};
    action.sa_sigaction = &Space::SignalHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
#endif
  }

#if defined(_WIN32)
  static LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        info->ExceptionRecord->NumberParameters < 2) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    uint8_t* address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
    Space* space = nullptr;
    if (!Owns(address, &space)) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = uint32_t((address - space->base_) / kPage);
    if (space->Committed(page)) return EXCEPTION_CONTINUE_SEARCH;
    space->Commit(page);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
#else
  static void SignalHandler(int, siginfo_t* info, void*) {
    uint8_t* address = static_cast<uint8_t*>(info->si_addr);
    Space* space = nullptr;
    if (!Owns(address, &space)) std::_Exit(139);
    const uint32_t page = uint32_t((address - space->base_) / kPage);
    if (space->Committed(page)) std::_Exit(139);
    space->Commit(page);
  }
#endif

  uint32_t seed_;
  uint8_t* base_ = nullptr;
  std::vector<uint8_t> committed_;
  std::vector<uint32_t> touched_;
};

}  // namespace any360::cpu::test
