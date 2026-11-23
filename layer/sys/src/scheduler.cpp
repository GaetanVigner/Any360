#include "any360/sys/scheduler.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "any360/sys/ldi.h"
#include "any360/sys/objects.h"
#include "any360/sys/spin_pacer.h"
#include "any360/sys/threading.h"
#include "any360/sys/variables.h"

namespace any360::sys {

namespace {

// KAPC fields.
constexpr uint32_t kApcType = 0, kApcMode = 2, kApcEnqueued = 3, kApcThread = 4, kApcKernelRoutine = 16,
                   kApcRundownRoutine = 20, kApcNormalRoutine = 24, kApcNormalContext = 28, kApcArg1 = 32, kApcArg2 = 36;

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
  uint32_t stack_limit = 0, stack_base = 0;
  uint8_t stack_reported = 0;  // stack watch: 1 = past 3/4 logged, 2 = past its end logged (threads_mutex_)
  uint32_t stack_logged = 0;   // stack watch, verbose: the deepest use logged so far (threads_mutex_)
  std::thread host;
  std::thread::id runner;  // the host thread running Run: `host` itself, or one a ThreadRunner runs it on
  std::mutex mutex;
  std::condition_variable resumed;
  std::atomic<bool> done{false};
};

namespace {
std::atomic<bool> g_shutting_down{false};
}

void BeginShutdown() { g_shutting_down.store(true, std::memory_order_release); }

bool ShuttingDown() { return g_shutting_down.load(std::memory_order_acquire); }

void ParkIfShuttingDown() {
  if (!ShuttingDown()) return;
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

Scheduler& Threads() {
  static Scheduler scheduler;
  return scheduler;
}

void Scheduler::SetDispatcher(Dispatcher dispatcher, void* user) {
  dispatcher_ = dispatcher;
  dispatcher_user_ = user;
}

void Scheduler::SetThreadRunner(ThreadRunner runner, void* user) {
  runner_ = runner;
  runner_user_ = user;
}

void Scheduler::RunAsGuestThread(void (*body)(void*), void* arg) const {
  if (runner_) {
    runner_(body, arg, runner_user_);
  } else {
    body(arg);
  }
}

void Scheduler::SetProcess(uint32_t kprocess) { process_ = kprocess; }

void Scheduler::Shutdown() {
  // Joined outside the lock: a thread ending meanwhile frees itself through OnFree, which takes it.
  std::vector<Thread*> threads;
  {
    std::lock_guard lock(threads_mutex_);
    threads.swap(threads_);
    blocks_.clear();
  }
  for (Thread* t : threads) {
    if (t->host.joinable()) t->host.join();
    delete t;
  }
  process_ = 0;
  next_thread_id_ = first_thread_id_;
  next_cpu_ = 0;
  ResetDpcs();           // the DPC thread runs guest code too
  Variables().Reset();   // and the clock thread writes guest memory
  Ldi().Reset();         // the LZX contexts are keyed by guest addresses
  std::lock_guard lock(apc_mutex_);
  apcs_.clear();
  apc_count_.store(0, std::memory_order_release);
}

Scheduler::Thread* Scheduler::Find(uint32_t kthread) const {
  std::lock_guard lock(threads_mutex_);
  for (Thread* t : threads_) {
    if (t->kthread == kthread) return t;
  }
  return nullptr;
}

uint32_t Scheduler::QueueApc(Guest g, uint32_t handle, uint32_t current, uint32_t routine, uint32_t context, uint32_t arg1,
                             uint32_t arg2) {
  const uint32_t kthread = handle == kCurrentThreadHandle ? current : Objects().Lookup(handle);
  if (!kthread) return kStatusInvalidHandle;
  if (g.Load8(kthread) != kThread) return kStatusObjectTypeMismatch;
  if (!routine) return kStatusInvalidParameter;
  QueueApcTo(kthread, routine, context, arg1, arg2);
  return kStatusSuccess;
}

void Scheduler::QueueIoApc(uint32_t kthread, uint32_t routine, uint32_t context, uint32_t io_status, uint32_t status) {
  if (status == kStatusSuccess && (routine & ~1u) && context) QueueApcTo(kthread, routine & ~1u, context, io_status, 0);
}

void Scheduler::QueueApcTo(uint32_t kthread, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2) {
  {
    std::lock_guard lock(apc_mutex_);
    apcs_.push_back({kthread, routine, context, arg1, arg2});
    apc_count_.store(apcs_.size(), std::memory_order_release);
  }
  Objects().AlertThread(kthread);  // its wait, if it sleeps in one, looks now
}

size_t Scheduler::PendingApcs(uint32_t kthread) const {
  if (apc_count_.load(std::memory_order_acquire) == 0) return 0;  // none queued for any thread: no lock
  std::lock_guard lock(apc_mutex_);
  size_t n = 0;
  for (const Apc& apc : apcs_) n += apc.kthread == kthread && apc.user;
  return n;
}

uint32_t Scheduler::LookupThreadById(Guest g, uint32_t thread_id, uint32_t object_out) const {
  uint32_t kthread = 0;
  {
    std::lock_guard lock(threads_mutex_);
    for (const Thread* t : threads_) {
      if (g.Load32(t->kthread + kThreadId) == thread_id) kthread = t->kthread;
    }
  }
  if (!kthread) return 0xC0000225;  // STATUS_NOT_FOUND
  Objects().ReferenceObject(kthread);
  if (object_out) g.Store32(object_out, kthread);
  return kStatusSuccess;
}

uint32_t Scheduler::ThreadOf(Guest g, uint32_t pcr) const {
  const uint32_t kthread = g.Load32(pcr + kPrcbCurrentThread);
  return Find(kthread) ? kthread : 0;
}

uint32_t Scheduler::EnsureProcess(Guest g) {
  if (!process_) {
    process_ = Objects().AllocateObject(kProcessSize);
    if (!process_) return kStatusNoMemory;
    std::memset(g.host(process_), 0, kProcessSize);
    // The title process: type 1 (user; 0 is the idle process, 2 the system's), as the console and the reference.
    // Call of Duty: Black Ops asks it (KeGetCurrentProcessType) to choose between two D3D devices, the title's and
    // the system's: with 2 it took the system's, never created, and read through a null pointer.
    g.Store8(process_ + kProcessTypeField, 1);
    for (uint32_t i = 0; i < 8; ++i) g.Store32(process_ + kProcessTlsBitmap + 4 * i, 0xFFFFFFFF);
  }
  return kStatusSuccess;
}

uint32_t Scheduler::CreateContext(Guest g, uint32_t stack_size, GuestContext& context, uint32_t creation_flags,
                                  bool reclaimable) {
  if (EnsureProcess(g) != kStatusSuccess) return kStatusNoMemory;
  if (!stack_size) stack_size = default_stack_size_;
  stack_size = std::max<uint32_t>(0x4000, (stack_size + 0xFFF) & ~0xFFFu);

  const uint32_t kthread = Objects().AllocateObject(kThreadSize, reclaimable),
                 pcr = Objects().AllocateObject(kPcrSize, reclaimable), tls = Objects().AllocateObject(kTlsBytes, reclaimable),
                 stack = Objects().AllocateObject(stack_size + kStackRedZone + 0x1000, reclaimable);
  if (!kthread || !pcr || !tls || !stack) {
    for (const uint32_t block : {kthread, pcr, tls, stack}) {
      if (block && reclaimable) Objects().FreeObject(block);
    }
    return kStatusNoMemory;
  }
  if (reclaimable) {
    std::lock_guard lock(threads_mutex_);
    blocks_[kthread] = {pcr, tls, stack};
  }
  std::memset(g.host(kthread), 0, kThreadSize);
  std::memset(g.host(pcr), 0, kPcrSize);
  std::memset(g.host(tls), 0, kTlsBytes);
  // The red zone below the stack belongs to the stack's block: a thread running past its end writes there first.
  const uint32_t stack_limit = (stack + kStackRedZone + 0xFFF) & ~0xFFFu, stack_base = stack_limit + stack_size;
  std::memset(g.host(stack), kStackFill, stack_size + kStackRedZone + 0x1000);

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
  g.Store32(kthread + kThreadId, next_thread_id_++);
  g.Store32(kthread + kCreationFlags, creation_flags);
  // The processor control region the thread's r13 points at.
  g.Store32(pcr + kPcrTls, tls);
  g.Store64(pcr + kPcrSelf, pcr);
  g.Store8(pcr + kPcrIrql, 0);
  g.Store32(pcr + kPcrStackBase, stack_base);
  g.Store32(pcr + kPcrStackEnd, stack_limit);
  g.Store32(pcr + kPrcbCurrentThread, kthread);
  g.Store32(pcr + kPcrPrcb, pcr + kPrcb);
  // Its hardware thread, as the reference: the processor mask in the creation flags' top byte (the highest bit set),
  // else the next of the six in turn. Titles read it from the PCR (XAudio's mix workers meet by it).
  const uint8_t mask = uint8_t(creation_flags >> 24) & 0x3F;
  uint8_t cpu = 0;
  if (mask) {
    cpu = uint8_t(31 - std::countl_zero(uint32_t(mask)));
  } else {
    next_cpu_ = uint8_t((next_cpu_ + 1) % 6);
    cpu = next_cpu_;
  }
  g.Store8(kthread + kCurrentCpu, cpu);
  g.Store8(pcr + kPrcbCurrentCpu, cpu);
  context = {kthread, pcr, stack_base - 0x40};
  return kStatusSuccess;
}

uint64_t Scheduler::Dispatch(uint32_t address, const GuestContext& context, std::initializer_list<uint64_t> args) {
  if (!dispatcher_ || !context.pcr) return 0;
  return dispatcher_(address, context.stack_top, context.pcr, args.begin(), uint32_t(args.size()), dispatcher_user_);
}

void Scheduler::KeInitializeApc(Guest g, uint32_t apc, uint32_t kthread, uint32_t kernel_routine, uint32_t rundown_routine,
                                uint32_t normal_routine, uint32_t mode, uint32_t normal_context) {
  g.Store16(apc + kApcType, 18);
  g.Store8(apc + kApcMode, uint8_t(normal_routine ? mode : 0));
  g.Store8(apc + kApcEnqueued, 0);
  g.Store32(apc + kApcThread, kthread);
  g.Store32(apc + kApcKernelRoutine, kernel_routine);
  g.Store32(apc + kApcRundownRoutine, rundown_routine);
  g.Store32(apc + kApcNormalRoutine, normal_routine);
  g.Store32(apc + kApcNormalContext, normal_routine ? normal_context : 0);
}

uint32_t Scheduler::KeInsertQueueApc(Guest g, uint32_t apc, uint32_t arg1, uint32_t arg2) {
  const uint32_t kthread = g.Load32(apc + kApcThread);
  if (!kthread) return 0;
  std::lock_guard lock(apc_mutex_);
  if (!g.Load8(kthread + kMayQueueApcs) || g.Load8(apc + kApcEnqueued)) return 0;
  g.Store32(apc + kApcArg1, arg1);
  g.Store32(apc + kApcArg2, arg2);
  g.Store8(apc + kApcEnqueued, 1);
  apcs_.push_back({kthread, 0, 0, 0, 0, apc, g.Load8(apc + kApcMode) != 0});
  apc_count_.store(apcs_.size(), std::memory_order_release);
  return 1;
}

uint32_t Scheduler::KeRemoveQueueApc(Guest g, uint32_t apc) {
  std::lock_guard lock(apc_mutex_);
  const auto it = std::find_if(apcs_.begin(), apcs_.end(), [&](const Apc& a) { return a.kapc == apc; });
  if (it == apcs_.end()) return 0;
  apcs_.erase(it);
  apc_count_.store(apcs_.size(), std::memory_order_release);
  g.Store8(apc + kApcEnqueued, 0);
  return 1;
}

size_t Scheduler::DeliverApcs(Guest g, uint32_t pcr, uint32_t stack_pointer, bool user_mode) {
  const uint32_t kthread = g.Load32(pcr + kPrcbCurrentThread);
  // Below the caller's frame and the red zone its saved registers may use; a KAPC's kernel routine gets pointers to
  // the normal routine, context and arguments in the gap above that.
  const GuestContext context{kthread, pcr, (stack_pointer - 0x400) & ~0xFu};
  const uint32_t scratch = context.stack_top + 0x40;
  // Kernel-mode APCs without a normal routine, then the other kernel-mode ones, then user-mode ones.
  auto rank = [&](const Apc& a) { return a.user ? 2 : a.kapc && g.Load32(a.kapc + kApcNormalRoutine) ? 1 : 0; };
  size_t delivered = 0;
  for (;;) {
    // None queued for any thread (every wait and delay looks): no lock. One queued after this look is delivered at
    // the thread's next wait, as one queued just after the lock would be.
    if (apc_count_.load(std::memory_order_acquire) == 0) return delivered;
    Apc apc;
    {
      std::lock_guard lock(apc_mutex_);
      auto best = apcs_.end();
      for (auto it = apcs_.begin(); it != apcs_.end(); ++it) {
        if (it->kthread != kthread || (it->user && !user_mode)) continue;
        if (best == apcs_.end() || rank(*it) < rank(*best)) best = it;
      }
      if (best == apcs_.end()) return delivered;
      apc = *best;
      apcs_.erase(best);
      apc_count_.store(apcs_.size(), std::memory_order_release);
      if (apc.kapc) g.Store8(apc.kapc + kApcEnqueued, 0);
    }
    if (apc.kapc) {
      g.Store32(scratch, g.Load32(apc.kapc + kApcNormalRoutine));
      g.Store32(scratch + 4, g.Load32(apc.kapc + kApcNormalContext));
      g.Store32(scratch + 8, g.Load32(apc.kapc + kApcArg1));
      g.Store32(scratch + 12, g.Load32(apc.kapc + kApcArg2));
      if (const uint32_t kernel = g.Load32(apc.kapc + kApcKernelRoutine)) {
        Dispatch(kernel, context, {apc.kapc, scratch, scratch + 4, scratch + 8, scratch + 12});
      }
      apc.routine = g.Load32(scratch), apc.context = g.Load32(scratch + 4);
      apc.arg1 = g.Load32(scratch + 8), apc.arg2 = g.Load32(scratch + 12);
    }
    if (apc.routine) Dispatch(apc.routine, context, {apc.context, apc.arg1, apc.arg2});
    delivered += apc.user;
  }
}

uint32_t Scheduler::CreateThread(Guest g, uint32_t handle_out, uint32_t stack_size, uint32_t thread_id_out, uint32_t xapi_startup,
                                 uint32_t start_address, uint32_t start_context, uint32_t creation_flags) {
  uint32_t handle = 0;
  const uint32_t status = CreateThreadHandle(g, handle_out ? &handle : nullptr, stack_size, thread_id_out, xapi_startup,
                                       start_address, start_context, creation_flags);
  if (status == kStatusSuccess && handle_out) g.Store32(handle_out, handle);
  return status;
}

uint32_t Scheduler::CreateThreadHandle(Guest g, uint32_t* handle, uint32_t stack_size, uint32_t thread_id_out, uint32_t xapi_startup,
                                 uint32_t start_address, uint32_t start_context, uint32_t creation_flags) {
  if (!dispatcher_) return kStatusInvalidParameter;
  GuestContext context;
  if (const uint32_t status = CreateContext(g, stack_size, context, creation_flags, true); status != kStatusSuccess) {
    return status;
  }
  const uint32_t kthread = context.kthread;
  Objects().AddFreeHook([](uint32_t object) { return Threads().OnFree(object); });
  // The thread keeps its own object until it ends; a title given the KTHREAD itself (no handle) keeps it for good.
  Objects().ReferenceObject(kthread);
  if (creation_flags & kCreateHandleIsObject) Objects().ReferenceObject(kthread);

  Thread* t = new Thread;
  t->kthread = kthread;
  t->pcr = context.pcr;
  t->stack_top = context.stack_top;
  t->start = start_address;
  t->context = start_context;
  t->xapi = xapi_startup;
  t->stack_limit = g.Load32(kthread + kStackLimit);
  t->stack_base = g.Load32(kthread + kStackBase);
  {
    std::lock_guard lock(threads_mutex_);
    threads_.push_back(t);
  }
  if (log_creation_ && stack_log_) {
    char line[200];
    std::snprintf(line, sizeof line, "thread %u starts at %08X (context %08X), stack %08X-%08X: %u KB (%s)",
                  g.Load32(kthread + kThreadId), start_address, start_context,
                  t->stack_limit, t->stack_base, (t->stack_base - t->stack_limit) / 1024,
                  stack_size ? "asked" : "the default");
    stack_log_(line);
  }
  if (handle) *handle = (creation_flags & kCreateHandleIsObject) ? kthread : Objects().OpenHandle(kthread);
  if (thread_id_out) g.Store32(thread_id_out, g.Load32(kthread + kThreadId));
  {
    // Run starts by taking this lock, so a thread that ends at once cannot free `t` before it holds its host thread.
    std::lock_guard lock(t->mutex);
    t->host = std::thread([this, t] { RunAsGuestThread([](void* thread) { Threads().Run(static_cast<Thread*>(thread)); }, t); });
  }
  return kStatusSuccess;
}

namespace {
// The lowest address in [low, high) whose word is no longer the stack fill (a stack's deepest write), `high` if none.
uint32_t LowestWritten(Guest g, uint32_t low, uint32_t high) {
  constexpr uint32_t kFillWord = 0x01010101u * Scheduler::kStackFill;
  for (uint32_t at = low; at + 4 <= high; at += 4) {
    uint32_t word;
    std::memcpy(&word, g.host(at), 4);
    if (word != kFillWord) return at;
  }
  return high;
}
}  // namespace

void Scheduler::SetStackLog(std::function<void(const std::string&)> log, std::chrono::milliseconds interval,
                            bool log_creation) {
  stack_log_ = std::move(log);
  stack_interval_ = interval;
  log_creation_ = log_creation;
  next_stack_report_ = 0;
}

void Scheduler::ReportStacks(Guest g) {
  if (!stack_log_) return;
  const int64_t now =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = next_stack_report_.load(std::memory_order_relaxed);
  if (now < next || !next_stack_report_.compare_exchange_strong(next, now + stack_interval_.count())) return;
  std::vector<std::string> lines;
  {
    std::lock_guard lock(threads_mutex_);
    for (Thread* t : threads_) {
      if (t->done || !t->stack_base || t->stack_reported >= 2) continue;
      const uint32_t size = t->stack_base - t->stack_limit;
      const uint32_t lowest = LowestWritten(g, t->stack_limit - kStackRedZone, t->stack_base);
      const uint32_t depth = t->stack_base - lowest;
      char line[300];
      if (lowest < t->stack_limit) {
        std::snprintf(line, sizeof line,
                      "thread %u (start %08X) ran %u bytes past the end of its %u KB stack (%08X-%08X)%s: what lies "
                      "below is other blocks of the kernel heap (another thread's stack, KTHREAD, PCR), written over",
                      g.Load32(t->kthread + kThreadId), t->start, t->stack_limit - lowest, size / 1024, t->stack_limit,
                      t->stack_base, lowest == t->stack_limit - kStackRedZone ? " at least (the whole red zone)" : "");
        t->stack_reported = 2;
      } else if ((depth > size / 4 * 3 && t->stack_reported < 1) ||
                 (log_creation_ && depth > size / 2 && depth >= t->stack_logged + 0x1000)) {
        std::snprintf(line, sizeof line, "thread %u (start %08X) used %u of its %u KB stack (%08X-%08X)",
                      g.Load32(t->kthread + kThreadId), t->start, (depth + 1023) / 1024, size / 1024, t->stack_limit,
                      t->stack_base);
        if (depth > size / 4 * 3) t->stack_reported = 1;
        t->stack_logged = depth;
      } else {
        continue;
      }
      lines.emplace_back(line);
    }
  }
  for (const std::string& line : lines) stack_log_(line);
}

uint32_t Scheduler::StackDepth(Guest g, uint32_t kthread) const {
  std::lock_guard lock(threads_mutex_);
  for (const Thread* t : threads_) {
    if (t->kthread != kthread || !t->stack_base) continue;
    return t->stack_base - LowestWritten(g, t->stack_limit - kStackRedZone, t->stack_base);
  }
  return 0;
}

void Scheduler::PauseWhileSuspended(Guest g, Thread* thread) {
  std::unique_lock<std::mutex> lock(thread->mutex);
  thread->resumed.wait(lock, [&] { return g.Load8(thread->kthread + kSuspendCount) == 0; });
}

void Scheduler::Run(Thread* t) {
  // The guest memory is the same for every thread: read it back through the objects' heap base.
  const Guest g = Objects().guest();
  t->runner = std::this_thread::get_id();
  PauseWhileSuspended(g, t);
  g.Store8(t->kthread + kThreadState, 2);  // running
  uint32_t exit_code;
  try {
    const GuestContext context{t->kthread, t->pcr, t->stack_top};
    const uint64_t r3 = t->xapi ? Dispatch(t->xapi, context, {t->start, t->context}) : Dispatch(t->start, context, {t->context});
    exit_code = t->xapi ? 0 : uint32_t(r3);
  } catch (const ThreadExit& exit) {
    exit_code = exit.code;
  }
  g.Store32(t->kthread + kExitStatus, exit_code);
  g.Store8(t->kthread + kTerminated, 1);
  g.Store8(t->kthread + kThreadState, 4);  // terminated
  g.Store32(t->kthread + kHeaderSignalState, 1);
  Objects().SignalChanged(t->kthread);
  t->done = true;
  // Last: with no handle left this frees the thread, `t` included.
  Objects().DereferenceObject(g, t->kthread);
}

bool Scheduler::OnFree(uint32_t kthread) {
  Thread* thread = nullptr;
  Blocks blocks{};
  {
    std::lock_guard lock(threads_mutex_);
    const auto it = blocks_.find(kthread);
    if (it == blocks_.end()) return false;
    blocks = it->second;
    blocks_.erase(it);
    for (auto t = threads_.begin(); t != threads_.end(); ++t) {
      if ((*t)->kthread != kthread) continue;
      thread = *t;
      threads_.erase(t);
      break;
    }
  }
  for (const uint32_t block : {blocks.pcr, blocks.tls, blocks.stack}) Objects().FreeObject(block);
  {
    std::lock_guard lock(apc_mutex_);
    std::erase_if(apcs_, [&](const Apc& apc) { return apc.kthread == kthread; });
    apc_count_.store(apcs_.size(), std::memory_order_release);
  }
  if (!thread) return true;
  // Freed from its own last instruction (Run's dereference) or from another thread once it has ended.
  if (thread->host.get_id() == std::this_thread::get_id() || thread->runner == std::this_thread::get_id()) {
    thread->host.detach();
  } else if (thread->host.joinable()) {
    thread->host.join();
  }
  delete thread;
  return true;
}

uint32_t Scheduler::TerminateThread(Guest g, uint32_t pcr, uint32_t exit_code) {
  const uint32_t kthread = ThreadOf(g, pcr);
  if (kthread) throw ThreadExit{exit_code};
  return exit_code;  // not a thread of the scheduler: nothing to unwind
}

void Scheduler::SetSpinPacing(bool on, std::function<void(const std::string&)> log) {
  spin_log_ = std::move(log);
  spin_pacing_.store(on, std::memory_order_relaxed);
}

void Scheduler::ZeroDelay() {
  if (!spin_pacing_.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
    return;
  }
  struct Local {
    SpinPacer pacer;
    SpinPacer::Stats added{};  // what this thread has added to the totals so far
    int64_t next_flush_ms = 0;
    bool no_timer = false;
  };
  static thread_local Local local;
  if (local.no_timer) {
    std::this_thread::yield();
    return;
  }
  const std::chrono::nanoseconds sleep = local.pacer.Delay(SpinPacer::Clock::now());
  if (sleep > std::chrono::nanoseconds::zero()) {
    if (!PreciseSleep(sleep)) local.no_timer = true;  // this thread yields from now on
  } else {
    std::this_thread::yield();
  }
  const auto back = SpinPacer::Clock::now();
  local.pacer.Returned(back);
  // This thread's counts into the totals once a second; the totals reported every 10 s by whichever thread passes.
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(back.time_since_epoch()).count();
  if (ms < local.next_flush_ms) return;
  local.next_flush_ms = ms + 1000;
  const SpinPacer::Stats& s = local.pacer.stats();
  spin_calls_.fetch_add(s.calls - local.added.calls, std::memory_order_relaxed);
  spin_sleeps_.fetch_add(s.sleeps - local.added.sleeps, std::memory_order_relaxed);
  spin_slept_ns_.fetch_add(s.slept_ns - local.added.slept_ns, std::memory_order_relaxed);
  spin_unsafe_.fetch_add(s.unsafe_ends - local.added.unsafe_ends, std::memory_order_relaxed);
  local.added = s;
  int64_t next = next_spin_report_.load(std::memory_order_relaxed);
  if (ms < next || !next_spin_report_.compare_exchange_strong(next, ms + 10000)) return;
  if (next == 0 || !spin_log_) return;  // the first pass: the report starts in 10 s
  const uint64_t calls = spin_calls_.exchange(0), sleeps = spin_sleeps_.exchange(0);
  const uint64_t slept = spin_slept_ns_.exchange(0), unsafe = spin_unsafe_.exchange(0);
  char line[200];
  std::snprintf(line, sizeof line,
                "spin pacing in 10 s: %llu zero delays, %llu slept (%.1f ms in all), %llu waits ended at a sleep's return",
                (unsigned long long)calls, (unsigned long long)sleeps, slept / 1e6, (unsigned long long)unsafe);
  spin_log_(line);
}

uint32_t Scheduler::Delay(Guest g, uint32_t pcr, int64_t interval, bool alertable) {
  // A zero delay (Sleep(0)) is what a title's spin loops call, thousands of times a frame: it takes no lock and reads
  // no clock unless it has to. The stack monitor (which reads the clock) looks at every delay that sleeps and at every
  // 64th zero delay of a thread (counted per thread: no shared counter for spinning threads to fight over).
  static thread_local uint32_t zero_delays = 0;
  if (interval != 0 || (zero_delays++ & 63) == 0) ReportStacks(g);
  const uint32_t kthread = g.Load32(pcr + kPrcbCurrentThread);
  // Suspended by another thread: waits for its resume. Its count is read first (no lock and no search when it is 0,
  // where PauseWhileSuspended returns at once).
  if (kthread && g.Load8(kthread + kSuspendCount) != 0) {
    if (Thread* t = Find(kthread)) PauseWhileSuspended(g, t);
  }
  std::chrono::nanoseconds duration{0};
  if (interval < 0) {
    duration = std::chrono::nanoseconds(-interval * 100);
  } else if (interval > 0) {
    const uint64_t now = SystemTime();
    if (uint64_t(interval) > now) duration = std::chrono::nanoseconds((uint64_t(interval) - now) * 100);
  }
  if (alertable && PendingApcs(kthread)) return kStatusUserApc;
  if (duration.count() == 0) {
    // Alertable ones yield (a sleep would hold back an APC queued meanwhile); the others may be paced.
    if (alertable) {
      std::this_thread::yield();
    } else {
      ZeroDelay();
    }
  } else if (!alertable) {
    std::this_thread::sleep_for(duration);
  } else {
    // Sleeps in slices short enough for a queued APC to end the delay.
    const auto end = std::chrono::steady_clock::now() + duration;
    for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now()) {
      Objects().Poll(g);  // a timer expiring may queue an APC on this thread
      if (PendingApcs(kthread)) return kStatusUserApc;
      std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(end - now, std::chrono::milliseconds(1)));
    }
  }
  return kStatusSuccess;
}

uint32_t Scheduler::Yield(Guest g, uint32_t pcr) {
  if (Thread* t = Find(g.Load32(pcr + kPrcbCurrentThread))) PauseWhileSuspended(g, t);
  std::this_thread::yield();
  return kStatusSuccess;
}

void Scheduler::SetCurrentStackPointers(Guest g, uint32_t pcr, uint32_t kthread, uint32_t alloc_base, uint32_t base,
                                        uint32_t limit) {
  constexpr uint32_t kStackAllocBase = 0xD0;
  g.Store32(kthread + kStackAllocBase, alloc_base);
  g.Store32(kthread + kStackBase, base);
  g.Store32(kthread + kStackLimit, limit);
  g.Store32(pcr + kPcrStackBase, base);
  g.Store32(pcr + kPcrStackEnd, limit);
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

uint32_t Scheduler::SetPriority(Guest g, uint32_t kthread, uint32_t priority) {
  if (!kthread) return 0;
  const uint32_t previous = g.Load8(kthread + kPriority);
  if (Find(kthread)) g.Store8(kthread + kPriority, uint8_t(std::max(int32_t(priority), 0)));
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
