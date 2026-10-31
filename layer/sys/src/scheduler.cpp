#include "any360/sys/scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "any360/sys/objects.h"

namespace any360::sys {

namespace {

// KTHREAD fields.
constexpr uint32_t kThreadSize = 0xAB0, kStackBase = 0x5C, kStackLimit = 0x60, kStackKernel = 0x64, kTlsAddress = 0x68,
                   kThreadState = 0x6C, kPriority = 0x70, kProcessTypeDup = 0x72, kProcessType = 0x73, kApcLists = 0x74,
                   kProcess = 0x84, kMayQueueApcs = 0x8B, kBoostDisabled = 0xBB, kSuspendCount = 0xBC,
                   kTerminated = 0xBE, kCurrentCpu = 0xBF, kPrcbA = 0xC0, kPrcbB = 0xC4, kExitStatus = 0x140, kThreadId = 0x14C,
                   kCreationFlags = 0x16C;
// PCR fields.
constexpr uint32_t kPcrSize = 0x2D8, kPcrTls = 0x0, kPcrIrql = 0x18, kPcrSelf = 0x30, kPcrStackBase = 0x70, kPcrStackEnd = 0x74,
                   kPrcb = 0x100, kPrcbCurrentThread = 0x100, kPrcbCurrentCpu = 0x10C, kPcrPrcb = 0x2A8;
// KPROCESS fields.
constexpr uint32_t kProcessSize = 0x60, kProcessTypeField = 0x2F, kProcessTlsBitmap = 0x30;
constexpr uint32_t kTlsBytes = 1024 * 4;
constexpr uint32_t kStatusSuccess = 0;

// Thrown by TerminateThread on a scheduler thread: unwinds to the trampoline.
struct ThreadExit {
  uint32_t code;
};

std::atomic<uint8_t>& Byte(Guest g, uint32_t address) { return *reinterpret_cast<std::atomic<uint8_t>*>(g.host(address)); }

uint8_t CpuOf(uint32_t affinity) {
  for (uint8_t i = 0; i < 6; ++i) {
    if (affinity & (1u << i)) return i;
  }
  return 0;
}

}  // namespace

struct Scheduler::Thread {
  uint32_t kthread = 0, pcr = 0, stack_top = 0, start = 0, context = 0, xapi = 0;
  std::thread host;
  std::mutex mutex;
  std::condition_variable resumed;
  std::atomic<bool> done{false};
};

Scheduler& Threads() {
  static Scheduler scheduler;
  return scheduler;
}

void Scheduler::SetDispatcher(Dispatcher dispatcher, void* user) {
  dispatcher_ = dispatcher;
  dispatcher_user_ = user;
}

void Scheduler::SetProcess(uint32_t kprocess) { process_ = kprocess; }

void Scheduler::Shutdown() {
  for (Thread* t : threads_) {
    if (t->host.joinable()) t->host.join();
    delete t;
  }
  threads_.clear();
  process_ = 0;
  next_thread_id_ = 1;
}

Scheduler::Thread* Scheduler::Find(uint32_t kthread) const {
  for (Thread* t : threads_) {
    if (t->kthread == kthread) return t;
  }
  return nullptr;
}

uint32_t Scheduler::ThreadOf(Guest g, uint32_t pcr) const {
  const uint32_t kthread = g.Load32(pcr + kPrcbCurrentThread);
  return Find(kthread) ? kthread : 0;
}

uint32_t Scheduler::CreateThread(Guest g, uint32_t handle_out, uint32_t stack_size, uint32_t thread_id_out, uint32_t xapi_startup,
                                 uint32_t start_address, uint32_t start_context, uint32_t creation_flags) {
  if (!dispatcher_) return kStatusInvalidParameter;
  if (!process_) {
    process_ = Objects().AllocateObject(kProcessSize);
    if (!process_) return kStatusNoMemory;
    std::memset(g.host(process_), 0, kProcessSize);
    g.Store8(process_ + kProcessTypeField, 2);  // the title process
    for (uint32_t i = 0; i < 8; ++i) g.Store32(process_ + kProcessTlsBitmap + 4 * i, 0xFFFFFFFF);
  }
  if (!stack_size) stack_size = default_stack_size_;
  stack_size = std::max<uint32_t>(0x4000, (stack_size + 0xFFF) & ~0xFFFu);

  const uint32_t kthread = Objects().AllocateObject(kThreadSize), pcr = Objects().AllocateObject(kPcrSize),
                 tls = Objects().AllocateObject(kTlsBytes), stack = Objects().AllocateObject(stack_size + 0x1000);
  if (!kthread || !pcr || !tls || !stack) return kStatusNoMemory;
  std::memset(g.host(kthread), 0, kThreadSize);
  std::memset(g.host(pcr), 0, kPcrSize);
  std::memset(g.host(tls), 0, kTlsBytes);
  const uint32_t stack_limit = (stack + 0xFFF) & ~0xFFFu, stack_base = stack_limit + stack_size;

  // The thread object: a dispatcher object of type 6, unsignalled until the thread ends.
  g.Store8(kthread + kHeaderType, kThread);
  g.Store32(kthread + kHeaderSignalState, 0);
  g.Store32(kthread + kHeaderWaitListFlink, kthread + kHeaderWaitListFlink);
  g.Store32(kthread + kHeaderWaitListBlink, kthread + kHeaderWaitListFlink);
  g.Store32(kthread + kStackBase, stack_base);
  g.Store32(kthread + kStackLimit, stack_limit);
  g.Store32(kthread + kStackKernel, stack_base - 240);
  g.Store32(kthread + kTlsAddress, tls);
  g.Store8(kthread + kPriority, 8);
  g.Store8(kthread + kProcessType, g.Load8(process_ + kProcessTypeField));
  g.Store8(kthread + kProcessTypeDup, g.Load8(process_ + kProcessTypeField));
  for (uint32_t list : {kApcLists, kApcLists + 8}) {
    g.Store32(kthread + list, kthread + list);
    g.Store32(kthread + list + 4, kthread + list);
  }
  g.Store32(kthread + kProcess, process_);
  g.Store8(kthread + kMayQueueApcs, 1);
  g.Store8(kthread + kSuspendCount, (creation_flags & kCreateSuspended) ? 1 : 0);
  g.Store32(kthread + kPrcbA, pcr + kPrcb);
  g.Store32(kthread + kPrcbB, pcr + kPrcb);
  const uint32_t thread_id = next_thread_id_++;
  g.Store32(kthread + kThreadId, thread_id);
  g.Store32(kthread + kCreationFlags, creation_flags);
  // The processor control region the thread's r13 points at.
  g.Store32(pcr + kPcrTls, tls);
  g.Store64(pcr + kPcrSelf, pcr);
  g.Store8(pcr + kPcrIrql, 0);
  g.Store32(pcr + kPcrStackBase, stack_base);
  g.Store32(pcr + kPcrStackEnd, stack_limit);
  g.Store32(pcr + kPrcbCurrentThread, kthread);
  g.Store32(pcr + kPcrPrcb, pcr + kPrcb);

  Thread* t = new Thread;
  t->kthread = kthread;
  t->pcr = pcr;
  t->stack_top = stack_base - 0x40;
  t->start = start_address;
  t->context = start_context;
  t->xapi = xapi_startup;
  threads_.push_back(t);
  if (handle_out) g.Store32(handle_out, (creation_flags & kCreateHandleIsObject) ? kthread : Objects().OpenHandle(kthread));
  if (thread_id_out) g.Store32(thread_id_out, thread_id);
  t->host = std::thread([this, t] { Run(t); });
  return kStatusSuccess;
}

void Scheduler::PauseWhileSuspended(Guest g, Thread* thread) {
  std::unique_lock<std::mutex> lock(thread->mutex);
  thread->resumed.wait(lock, [&] { return g.Load8(thread->kthread + kSuspendCount) == 0; });
}

void Scheduler::Run(Thread* t) {
  // The guest memory is the same for every thread: read it back through the objects' heap base.
  const Guest g = Objects().guest();
  PauseWhileSuspended(g, t);
  g.Store8(t->kthread + kThreadState, 2);  // running
  uint32_t exit_code;
  try {
    const uint64_t r3 = t->xapi ? dispatcher_(t->xapi, t->stack_top, t->pcr, t->start, t->context, dispatcher_user_)
                                : dispatcher_(t->start, t->stack_top, t->pcr, t->context, 0, dispatcher_user_);
    exit_code = t->xapi ? 0 : uint32_t(r3);
  } catch (const ThreadExit& exit) {
    exit_code = exit.code;
  }
  g.Store32(t->kthread + kExitStatus, exit_code);
  g.Store8(t->kthread + kTerminated, 1);
  g.Store8(t->kthread + kThreadState, 4);  // terminated
  g.Store32(t->kthread + kHeaderSignalState, 1);
  t->done = true;
}

uint32_t Scheduler::TerminateThread(Guest g, uint32_t pcr, uint32_t exit_code) {
  const uint32_t kthread = ThreadOf(g, pcr);
  if (kthread) throw ThreadExit{exit_code};
  return exit_code;  // not a thread of the scheduler: nothing to unwind
}

uint32_t Scheduler::Delay(Guest g, uint32_t pcr, int64_t interval) {
  if (Thread* t = Find(g.Load32(pcr + kPrcbCurrentThread))) PauseWhileSuspended(g, t);
  if (interval < 0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(-interval * 100));
  } else if (interval > 0) {
    const uint64_t now = SystemTime();
    if (uint64_t(interval) > now) std::this_thread::sleep_for(std::chrono::nanoseconds((uint64_t(interval) - now) * 100));
  } else {
    std::this_thread::yield();
  }
  return kStatusSuccess;
}

uint32_t Scheduler::Yield(Guest g, uint32_t pcr) {
  if (Thread* t = Find(g.Load32(pcr + kPrcbCurrentThread))) PauseWhileSuspended(g, t);
  std::this_thread::yield();
  return kStatusSuccess;
}

uint32_t Scheduler::Resume(Guest g, uint32_t kthread, uint32_t* previous_count) {
  Thread* t = Find(kthread);
  if (!t) return kStatusInvalidHandle;
  const uint8_t previous = Byte(g, kthread + kSuspendCount).fetch_sub(1);
  if (previous_count) *previous_count = previous;
  if (previous == 0) Byte(g, kthread + kSuspendCount).store(0);  // not suspended: stays so
  {
    std::lock_guard<std::mutex> lock(t->mutex);
  }
  t->resumed.notify_all();
  return kStatusSuccess;
}

uint32_t Scheduler::Suspend(Guest g, uint32_t kthread, uint32_t* previous_count) {
  if (!Find(kthread)) return kStatusInvalidHandle;
  if (g.Load8(kthread + kTerminated)) return kStatusThreadIsTerminating;
  const uint8_t previous = Byte(g, kthread + kSuspendCount).fetch_add(1);
  if (previous_count) *previous_count = previous;
  return kStatusSuccess;
}

uint32_t Scheduler::SetAffinity(Guest g, uint32_t kthread, uint32_t affinity, uint32_t previous_out) {
  // Unlike Windows NT the previous affinity goes through the pointer; the result is a status.
  if (!affinity) return kStatusInvalidParameter;
  if (!Find(kthread)) return kStatusInvalidHandle;
  if (previous_out) g.Store32(previous_out, 1u << g.Load8(kthread + kCurrentCpu));
  const uint8_t cpu = CpuOf(affinity);
  g.Store8(kthread + kCurrentCpu, cpu);
  if (Thread* t = Find(kthread)) g.Store8(t->pcr + kPrcbCurrentCpu, cpu);
  return kStatusSuccess;
}

uint32_t Scheduler::QueryBasePriority(Guest g, uint32_t kthread) {
  return Find(kthread) ? g.Load8(kthread + kPriority) : 0;
}

uint32_t Scheduler::SetBasePriority(Guest g, uint32_t kthread, uint32_t increment) {
  if (!Find(kthread)) return 0;
  const uint32_t previous = g.Load8(kthread + kPriority);
  const int32_t value = int32_t(increment);
  g.Store8(kthread + kPriority, uint8_t(value < 0 ? 0 : value > 31 ? 31 : value));
  return previous;
}

uint32_t Scheduler::SetDisableBoost(Guest g, uint32_t kthread, uint32_t disabled) {
  return Byte(g, kthread + kBoostDisabled).exchange(uint8_t(disabled));
}

uint32_t Scheduler::NtResumeThread(Guest g, uint32_t handle, uint32_t count_out) {
  const uint32_t kthread = Objects().Lookup(handle);
  uint32_t count = 0;
  const uint32_t status = kthread ? Resume(g, kthread, &count) : kStatusInvalidHandle;
  if (count_out) g.Store32(count_out, count);
  return status;
}

uint32_t Scheduler::NtSuspendThread(Guest g, uint32_t handle, uint32_t count_out) {
  const uint32_t kthread = Objects().Lookup(handle);
  if (!kthread) return kStatusInvalidHandle;
  if (g.Load8(kthread + kHeaderType) != kThread) return kStatusObjectTypeMismatch;
  uint32_t count = 0;
  const uint32_t status = Suspend(g, kthread, &count);
  if (count_out) g.Store32(count_out, count);
  return status;
}

}  // namespace any360::sys
