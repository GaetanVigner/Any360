// The scheduler: the layer's guest threads. Each is a host thread with its own KTHREAD, PCR, TLS block and stack in
// guest memory (the structures the rest of the layer reads through r13), run through the dispatcher the title
// provides: the layer never executes guest code itself, it hands the start address and the thread's registers to the
// recompiled or decompiled code. A thread is a dispatcher object (type 6) signalled when it ends, so handles to it
// can be waited on. Suspension is cooperative: a thread created suspended waits before its first instruction, a
// running one pauses at its next delay, yield or wait.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <vector>

#include "guest.h"

namespace any360::sys {

constexpr uint32_t kStatusThreadIsTerminating = 0xC000004B, kStatusObjectTypeMismatch = 0xC0000024, kStatusUserApc = 0xC0;
constexpr uint32_t kCreateSuspended = 0x1, kCreateHandleIsObject = 0x80;

// Runs guest code at `address` with r1 = `stack_top`, r13 = `pcr`, r3 onwards = the `count` arguments (at most 8, up
// to r10); returns r3.
constexpr uint32_t kMaxDispatchArguments = 8;
using Dispatcher = uint64_t (*)(uint32_t address, uint32_t stack_top, uint32_t pcr, const uint64_t* args, uint32_t count,
                                void* user);

// A guest execution context: a KTHREAD, its PCR, TLS block and stack. Threads run on one each; host-driven callbacks
// (graphics interrupts, the audio clock) run guest code on one through Dispatch.
struct GuestContext {
  uint32_t kthread = 0, pcr = 0, stack_top = 0;
};

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
  // A relative interval (negative, 100 ns) sleeps, zero yields, an absolute one waits until that system time. An
  // alertable delay ends early with STATUS_USER_APC once a user APC is queued on the thread (not delivered here).
  uint32_t Delay(Guest g, uint32_t pcr, int64_t interval, bool alertable = false);
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

  // A context of its own for host-driven guest callbacks; not a scheduler thread (its handle is never opened).
  uint32_t CreateContext(Guest g, uint32_t stack_size, GuestContext& context, uint32_t creation_flags = 0);
  // Runs guest code at `address` on `context` through the dispatcher; returns r3 (0 without a dispatcher).
  uint64_t Dispatch(uint32_t address, const GuestContext& context, std::initializer_list<uint64_t> args);

  // NtQueueApcThread: a user APC (routine, context, two arguments) queued on a thread by handle (the current-thread
  // pseudo handle is `current`). The thread runs it at its next alertable wait or delay.
  uint32_t QueueApc(Guest g, uint32_t handle, uint32_t current, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2);
  // The same on a KTHREAD, from the kernel itself (timer expiry, I/O completion).
  void QueueApcTo(uint32_t kthread, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2);
  // An I/O call's completion APC: routine(context, I/O status block, 0) queued on the caller when the transfer
  // succeeded and both routine and context are given, as the reference; the routine's low bit (no completion port)
  // is dropped.
  void QueueIoApc(uint32_t kthread, uint32_t routine, uint32_t context, uint32_t io_status, uint32_t status);
  // User-mode APCs queued on a thread (the ones an alertable wait runs).
  size_t PendingApcs(uint32_t kthread) const;

  // KAPC (40 bytes): type 18, mode (0 kernel, 1 user), enqueued flag, thread, list entry, kernel, rundown and normal
  // routines, normal context, two arguments. Initialize lays it out as the reference (no normal routine: kernel mode,
  // no context). Insert records the arguments and queues it on its thread (0 with no thread, a thread that may not
  // queue APCs, or one already queued); Remove takes a queued one back (1) or answers 0.
  void KeInitializeApc(Guest g, uint32_t apc, uint32_t kthread, uint32_t kernel_routine, uint32_t rundown_routine,
                       uint32_t normal_routine, uint32_t mode, uint32_t normal_context);
  uint32_t KeInsertQueueApc(Guest g, uint32_t apc, uint32_t arg1, uint32_t arg2);
  uint32_t KeRemoveQueueApc(Guest g, uint32_t apc);

  // Runs the APCs queued on the thread of `pcr` on that thread, its stack continuing below `stack_pointer` (the
  // caller's r1): kernel-mode ones always (those without a normal routine first), user-mode ones when `user_mode`,
  // oldest first. A KAPC's kernel routine runs first as kernel(apc, &normal, &context, &arg1, &arg2) and may change
  // or clear what follows; the normal routine then runs as normal(context, arg1, arg2). Returns how many user-mode
  // APCs ran.
  size_t DeliverApcs(Guest g, uint32_t pcr, uint32_t stack_pointer, bool user_mode = true);
  // ObLookupThreadByThreadId: the KTHREAD of a thread id, referenced; STATUS_NOT_FOUND when no thread has it.
  uint32_t LookupThreadById(Guest g, uint32_t thread_id, uint32_t object_out) const;

  // The current KTHREAD of a PCR, 0 when the scheduler does not run that thread.
  uint32_t ThreadOf(Guest g, uint32_t pcr) const;

 private:
  struct Thread;
  Thread* Find(uint32_t kthread) const;
  uint32_t EnsureProcess(Guest g);
  void Run(Thread* thread);
  void PauseWhileSuspended(Guest g, Thread* thread);

  Dispatcher dispatcher_ = nullptr;
  void* dispatcher_user_ = nullptr;
  uint32_t process_ = 0;
  uint32_t default_stack_size_ = 0x10000;
  uint32_t next_thread_id_ = 1;
  struct Apc {
    uint32_t kthread, routine, context, arg1, arg2;
    uint32_t kapc = 0;  // the guest KAPC, 0 for NtQueueApcThread and the kernel's own APCs
    bool user = true;
  };
  mutable std::mutex apc_mutex_;
  std::vector<Apc> apcs_;
  std::vector<Thread*> threads_;
};

Scheduler& Threads();

}  // namespace any360::sys
