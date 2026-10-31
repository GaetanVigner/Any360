// A 4 GB guest address space for the tests, committed page by page on first touch (Windows: a vectored exception
// handler; POSIX: a lazily backed mapping), so a guest pointer is `base + address` as in a static recompilation.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

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

namespace any360::test {

constexpr uint64_t kGuestSpace = 0x100000000ull + 0x1000;
constexpr uint32_t kPageSize = 4096;

class GuestMemory {
 public:
  static GuestMemory& Get() {
    static GuestMemory memory;
    return memory;
  }
  uint8_t* base() const { return base_; }

  void Fill(uint32_t address, uint32_t size, uint8_t value) { std::memset(base_ + address, value, size); }

 private:
  GuestMemory() {
#if defined(_WIN32)
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestSpace, MEM_RESERVE, PAGE_NOACCESS));
    AddVectoredExceptionHandler(1, &GuestMemory::Handler);
#else
    void* mapping = mmap(nullptr, kGuestSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base_ = mapping == MAP_FAILED ? nullptr : static_cast<uint8_t*>(mapping);
    struct sigaction action = {};
    action.sa_sigaction = &GuestMemory::SignalHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
#endif
    if (!base_) {
      std::printf("guest memory: cannot reserve the guest address space\n");
      std::exit(2);
    }
  }

#if defined(_WIN32)
  static LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        info->ExceptionRecord->NumberParameters < 2) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    GuestMemory& memory = Get();
    uint8_t* address = reinterpret_cast<uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
    if (address < memory.base_ || address >= memory.base_ + kGuestSpace) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    uint8_t* page = memory.base_ + ((address - memory.base_) & ~uint64_t(kPageSize - 1));
    if (!VirtualAlloc(page, kPageSize, MEM_COMMIT, PAGE_READWRITE)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
#else
  static void SignalHandler(int, siginfo_t* info, void*) {
    GuestMemory& memory = Get();
    uint8_t* address = static_cast<uint8_t*>(info->si_addr);
    if (address < memory.base_ || address >= memory.base_ + kGuestSpace) {
      std::_Exit(139);
    }
    uint8_t* page = memory.base_ + ((address - memory.base_) & ~uint64_t(kPageSize - 1));
    mprotect(page, kPageSize, PROT_READ | PROT_WRITE);
  }
#endif

  uint8_t* base_ = nullptr;
};

}  // namespace any360::test
