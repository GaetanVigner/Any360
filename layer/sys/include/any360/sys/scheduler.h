// The scheduler: the layer's guest threads. Each is a host thread with its own KTHREAD, PCR, TLS block and stack in
// guest memory (the structures the rest of the layer reads through r13), run through the dispatcher the title
// provides: the layer never executes guest code itself, it hands the start address and the thread's registers to the
// recompiled or decompiled code. A thread is a dispatcher object (type 6) signalled when it ends, so handles to it
// can be waited on. Suspension is cooperative: a thread created suspended waits before its first instruction, a
// running one pauses at its next delay, yield or wait.
#pragma once

#include <cstdint>
#include <vector>

#include "guest.h"

namespace any360::sys {

constexpr uint32_t kStatusThreadIsTerminating = 0xC000004B, kStatusObjectTypeMismatch = 0xC0000024;
constexpr uint32_t kCreateSuspended = 0x1, kCreateHandleIsObject = 0x80;

// Runs guest code at `address` with r1 = `stack_top`, r13 = `pcr`, r3 / r4 = the arguments; returns r3.
using Dispatcher = uint64_t (*)(uint32_t address, uint32_t stack_top, uint32_t pcr, uint64_t arg0, uint64_t arg1, void* user);

class Scheduler {
 public:
  void SetDispatcher(Dispatcher dispatcher, void* user);
  // The KPROCESS new threads belong to (its type and TLS bitmap); allocated in the kernel heap when none is set.
  void SetProcess(uint32_t kprocess);
  uint32_t process() const { return process_; }
  // Default stack size of a thread created with none (the title's XEX says; 64 KB otherwise).
  void SetDefaultStackSize(uint32_t size) { default_stack_size_ = size; }
  // Joins every thread; call before the guest memory goes away.
  void Shutdown();

  // ExCreateThread: handle (or the KTHREAD itself with kCreateHandleIsObject) and thread id written when asked.
  uint32_t CreateThread(Guest g, uint32_t handle_out, uint32_t stack_size, uint32_t thread_id_out, uint32_t xapi_startup,
                        uint32_t start_address, uint32_t start_context, uint32_t creation_flags);
  // The calling thread (its PCR) ends with `exit_code`; does not return for a thread the scheduler runs.
  uint32_t TerminateThread(Guest g, uint32_t pcr, uint32_t exit_code);
  // A relative interval (negative, 100 ns) sleeps, zero yields, an absolute one waits until that system time.
  uint32_t Delay(Guest g, uint32_t pcr, int64_t interval);
  uint32_t Yield(Guest g, uint32_t pcr);

  // On a KTHREAD. Resume and Suspend return STATUS_SUCCESS and the previous suspend count.
  uint32_t Resume(Guest g, uint32_t kthread, uint32_t* previous_count);
  uint32_t Suspend(Guest g, uint32_t kthread, uint32_t* previous_count);
  uint32_t SetAffinity(Guest g, uint32_t kthread, uint32_t affinity, uint32_t previous_out);
  uint32_t QueryBasePriority(Guest g, uint32_t kthread);
  uint32_t SetBasePriority(Guest g, uint32_t kthread, uint32_t increment);
  uint32_t SetDisableBoost(Guest g, uint32_t kthread, uint32_t disabled);

  // Handle-based forms.
  uint32_t NtResumeThread(Guest g, uint32_t handle, uint32_t count_out);
  uint32_t NtSuspendThread(Guest g, uint32_t handle, uint32_t count_out);

  // The current KTHREAD of a PCR, 0 when the scheduler does not run that thread.
  uint32_t ThreadOf(Guest g, uint32_t pcr) const;

 private:
  struct Thread;
  Thread* Find(uint32_t kthread) const;
  void Run(Thread* thread);
  void PauseWhileSuspended(Guest g, Thread* thread);

  Dispatcher dispatcher_ = nullptr;
  void* dispatcher_user_ = nullptr;
  uint32_t process_ = 0;
  uint32_t default_stack_size_ = 0x10000;
  uint32_t next_thread_id_ = 1;
  std::vector<Thread*> threads_;
};

Scheduler& Threads();

}  // namespace any360::sys
