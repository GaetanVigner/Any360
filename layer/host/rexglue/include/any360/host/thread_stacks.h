// The call stacks of a process's threads, for diagnostics (hang reports, the hitch tracer): a thread is suspended, its
// context unwound into return addresses, and resumed. Nothing is allocated, logged or symbolized while a thread is
// suspended (it may hold the heap's lock); names come afterwards (Describe). Windows only; elsewhere nothing is
// captured.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace any360::host::diagnostics {

constexpr size_t kMaxStackFrames = 48;

struct ThreadStack {
  uint32_t id = 0;
  size_t count = 0;
  uint64_t frames[kMaxStackFrames] = {};  // return addresses, the innermost first
};

// The process's threads but the calling one (allocates: call it with no thread suspended).
std::vector<uint32_t> ProcessThreads();

// A thread opened for repeated captures (suspend, unwind, resume) and for its processor cycles.
class ThreadHandle {
 public:
  explicit ThreadHandle(uint32_t id);
  ~ThreadHandle();
  ThreadHandle(const ThreadHandle&) = delete;
  ThreadHandle& operator=(const ThreadHandle&) = delete;
  ThreadHandle(ThreadHandle&& o) noexcept : id_(o.id_), handle_(o.handle_) { o.handle_ = nullptr; }
  bool valid() const { return handle_ != nullptr; }
  uint32_t id() const { return id_; }
  // The thread's stack now; false when it could not be suspended or read. Allocation-free.
  bool Capture(ThreadStack& out) const;
  // The processor cycles the thread has used (QueryThreadCycleTime).
  uint64_t Cycles() const;
  // Its description (SetThreadDescription), or empty.
  std::string Name() const;

 private:
  uint32_t id_ = 0;
  void* handle_ = nullptr;
};

// The executable's own address range (its image): where a title's functions are.
bool InExecutable(uint64_t address);
// The start of the function `address` is in (its unwind data), or `address` itself without any. Allocation-free.
uint64_t FunctionStart(uint64_t address);
// "module!function+0xoffset (file:line)" from the executable's PDB and the DLLs' symbols, or the address in hex.
// Takes a lock of its own (dbghelp is single-threaded).
std::string Describe(uint64_t address);

}  // namespace any360::host::diagnostics
