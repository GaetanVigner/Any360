// PowerPC ABI adapters: one `any360_<Export>(PPCContext&, uint8_t*)` per system call the layer provides, with the same
// calling convention as the kernel import thunks of a static recompilation (arguments in r3..r10, result in r3), so a
// title binds them in place of the reference runtime's `__imp__<Export>`. kExports lists them by name for the binder and
// for tools/build_db.py, which counts every name here as native.
#include <rex/ppc.h>

#include <vector>

#include "any360/sys/config.h"
#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/format.h"
#include "any360/sys/info.h"
#include "any360/sys/memory.h"
#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/rtl.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/threading.h"
#include "any360/sys/video.h"
#include "any360/sys/xam.h"

#define ANY360_SYS_EXPORT(name) extern "C" void any360_##name(PPCContext& ctx, [[maybe_unused]] uint8_t* base)

namespace any360::sys {

ANY360_SYS_EXPORT(RtlCompareMemoryUlong) { ctx.r3.u64 = RtlCompareMemoryUlong({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(RtlFillMemoryUlong) { RtlFillMemoryUlong({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(RtlUpperChar) { ctx.r3.u64 = RtlUpperChar(ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlLowerChar) { ctx.r3.u64 = RtlLowerChar(ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlCompareString) { ctx.r3.u64 = RtlCompareString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(RtlCompareStringN) {
  ctx.r3.u64 = RtlCompareStringN({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(RtlInitAnsiString) { RtlInitAnsiString({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlInitUnicodeString) { RtlInitUnicodeString({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlMultiByteToUnicodeN) {
  ctx.r3.u64 = RtlMultiByteToUnicodeN({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(RtlUnicodeToMultiByteN) {
  ctx.r3.u64 = RtlUnicodeToMultiByteN({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(RtlTimeToTimeFields) { RtlTimeToTimeFields({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlTimeFieldsToTime) { ctx.r3.u64 = RtlTimeFieldsToTime({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlComputeCrc32) { ctx.r3.u64 = RtlComputeCrc32({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XGetAVPack) { ctx.r3.u64 = XGetAVPack(); }
ANY360_SYS_EXPORT(XGetGameRegion) { ctx.r3.u64 = XGetGameRegion(); }
ANY360_SYS_EXPORT(XGetLanguage) { ctx.r3.u64 = XGetLanguage(); }
ANY360_SYS_EXPORT(XamGetSystemVersion) { ctx.r3.u64 = XamGetSystemVersion(); }

// Arguments past the eighth come from the caller's stack frame, one 8-byte slot each from r1 + 0x54.
static uint32_t StackArgument(PPCContext& ctx, uint8_t* base, unsigned index) {
  return Guest{base}.Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
}

ANY360_SYS_EXPORT(RtlNtStatusToDosError) { ctx.r3.u64 = RtlNtStatusToDosError(ctx.r3.u32); }
ANY360_SYS_EXPORT(ExGetXConfigSetting) {
  ctx.r3.u64 = ExGetXConfigSetting({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(RtlInitializeCriticalSection) { RtlInitializeCriticalSection({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlInitializeCriticalSectionAndSpinCount) {
  ctx.r3.u64 = RtlInitializeCriticalSectionAndSpinCount({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(VdQueryVideoMode) { VdQueryVideoMode({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XGetVideoMode) { VdQueryVideoMode({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(VdQueryVideoFlags) { ctx.r3.u64 = VdQueryVideoFlags(); }
ANY360_SYS_EXPORT(VdGetCurrentDisplayInformation) { VdGetCurrentDisplayInformation({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(VdGetCurrentDisplayGamma) { VdGetCurrentDisplayGamma({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(VdSetDisplayMode) { ctx.r3.u64 = VdSetDisplayMode(ctx.r3.u32); }
ANY360_SYS_EXPORT(VdEnableDisableClockGating) { ctx.r3.u64 = VdEnableDisableClockGating(ctx.r3.u32); }
ANY360_SYS_EXPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = VdIsHSIOTrainingSucceeded(); }
ANY360_SYS_EXPORT(VdRetrainEDRAM) { ctx.r3.u64 = VdRetrainEDRAM(); }
ANY360_SYS_EXPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = VdRetrainEDRAMWorker(); }
ANY360_SYS_EXPORT(VdGetGraphicsAsicID) { ctx.r3.u64 = VdGetGraphicsAsicID(); }
ANY360_SYS_EXPORT(VdGetSystemCommandBuffer) { VdGetSystemCommandBuffer({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = VdCallGraphicsNotificationRoutines(); }
ANY360_SYS_EXPORT(VdInitializeScalerCommandBuffer) {
  ctx.r3.u64 = VdInitializeScalerCommandBuffer({base}, StackArgument(ctx, base, 10), StackArgument(ctx, base, 11));
}
ANY360_SYS_EXPORT(NetDll_inet_addr) { ctx.r3.u64 = NetDll_inet_addr({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_XNetInAddrToString) { NetDll_XNetInAddrToString({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = NetDll_XNetXnAddrToInAddr(); }
ANY360_SYS_EXPORT(NetDll_XNetInAddrToXnAddr) { ctx.r3.u64 = NetDll_XNetInAddrToXnAddr(); }
ANY360_SYS_EXPORT(NetDll___WSAFDIsSet) { ctx.r3.u64 = NetDll___WSAFDIsSet({base}, ctx.r3.u32, ctx.r4.u32); }

// The variadic arguments of a call: r3..r10 from `first`, then the caller's stack slots.
class RegisterArguments : public ArgumentSource {
 public:
  RegisterArguments(PPCContext& ctx, uint8_t* base, unsigned first) : ctx_(ctx), base_(base), index_(first) {}
  uint64_t Next64() override {
    static constexpr PPCRegister PPCContext::* kRegisters[] = {&PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6,
                                                               &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10};
    const unsigned i = index_++;
    if (i < 8) return (ctx_.*kRegisters[i]).u64;
    return Guest{base_}.Load64(ctx_.r1.u32 + 0x54 + (i - 8) * 8);
  }

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  unsigned index_;
};

ANY360_SYS_EXPORT(sprintf) {
  RegisterArguments args(ctx, base, 2);
  ctx.r3.u64 = uint64_t(int64_t(Sprintf({base}, ctx.r3.u32, ctx.r4.u32, args, false)));
}
ANY360_SYS_EXPORT(_snprintf) {
  RegisterArguments args(ctx, base, 3);
  ctx.r3.u64 = uint64_t(int64_t(Snprintf({base}, ctx.r3.u32, int32_t(ctx.r4.u32), ctx.r5.u32, args, false, false)));
}
ANY360_SYS_EXPORT(swprintf) {
  RegisterArguments args(ctx, base, 2);
  ctx.r3.u64 = uint64_t(int64_t(Sprintf({base}, ctx.r3.u32, ctx.r4.u32, args, true)));
}
ANY360_SYS_EXPORT(_snwprintf) {
  RegisterArguments args(ctx, base, 3);
  ctx.r3.u64 = uint64_t(int64_t(Snprintf({base}, ctx.r3.u32, int32_t(ctx.r4.u32), ctx.r5.u32, args, true, false)));
}
ANY360_SYS_EXPORT(vsprintf) {
  ArrayArguments args({base}, ctx.r5.u32);
  ctx.r3.u64 = uint64_t(int64_t(Sprintf({base}, ctx.r3.u32, ctx.r4.u32, args, false)));
}
ANY360_SYS_EXPORT(_vsnprintf) {
  ArrayArguments args({base}, ctx.r6.u32);
  ctx.r3.u64 = uint64_t(int64_t(Snprintf({base}, ctx.r3.u32, int32_t(ctx.r4.u32), ctx.r5.u32, args, false, true)));
}
ANY360_SYS_EXPORT(vswprintf) {
  ArrayArguments args({base}, ctx.r5.u32);
  ctx.r3.u64 = uint64_t(int64_t(Sprintf({base}, ctx.r3.u32, ctx.r4.u32, args, true)));
}
ANY360_SYS_EXPORT(_vsnwprintf) {
  ArrayArguments args({base}, ctx.r6.u32);
  ctx.r3.u64 = uint64_t(int64_t(Snprintf({base}, ctx.r3.u32, int32_t(ctx.r4.u32), ctx.r5.u32, args, true, true)));
}
ANY360_SYS_EXPORT(_vscwprintf) {
  ArrayArguments args({base}, ctx.r4.u32);
  ctx.r3.u64 = uint64_t(int64_t(Vscwprintf({base}, ctx.r3.u32, args)));
}
ANY360_SYS_EXPORT(DbgPrint) {
  RegisterArguments args(ctx, base, 1);
  ctx.r3.u64 = DbgPrint({base}, ctx.r3.u32, args);
}
ANY360_SYS_EXPORT(RtlCompareMemory) { ctx.r3.u64 = RtlCompareMemory({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(RtlCaptureContext) { RtlCaptureContext(); }
ANY360_SYS_EXPORT(RtlUnwind) { RtlUnwind(); }
ANY360_SYS_EXPORT(__C_specific_handler) { __C_specific_handler(); }
ANY360_SYS_EXPORT(VdShutdownEngines) { VdShutdownEngines(); }
ANY360_SYS_EXPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { VdSetSystemCommandBufferGpuIdentifierAddress(); }

// Batch 4: the thread's own kernel structures through r13 (the PCR).
ANY360_SYS_EXPORT(KeGetCurrentProcessType) { ctx.r3.u64 = KeGetCurrentProcessType({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KeSetCurrentProcessType) { KeSetCurrentProcessType({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = KeRaiseIrqlToDpcLevel({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KfRaiseIrql) { ctx.r3.u64 = KfRaiseIrql({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KfLowerIrql) { KfLowerIrql({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KfAcquireSpinLock) { ctx.r3.u64 = KfAcquireSpinLock({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KfReleaseSpinLock) { KfReleaseSpinLock({base}, ctx.r13.u32, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeAcquireSpinLockAtRaisedIrql) { KeAcquireSpinLockAtRaisedIrql({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeTryToAcquireSpinLockAtRaisedIrql) {
  ctx.r3.u64 = KeTryToAcquireSpinLockAtRaisedIrql({base}, ctx.r13.u32, ctx.r3.u32);
}
ANY360_SYS_EXPORT(KeReleaseSpinLockFromRaisedIrql) { KeReleaseSpinLockFromRaisedIrql({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlEnterCriticalSection) { RtlEnterCriticalSection({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlTryEnterCriticalSection) { ctx.r3.u64 = RtlTryEnterCriticalSection({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlLeaveCriticalSection) { RtlLeaveCriticalSection({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeEnterCriticalRegion) { KeEnterCriticalRegion({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KeLeaveCriticalRegion) { KeLeaveCriticalRegion({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KeTlsAlloc) { ctx.r3.u64 = KeTlsAlloc({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KeTlsFree) { ctx.r3.u64 = KeTlsFree({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeTlsGetValue) { ctx.r3.u64 = KeTlsGetValue({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeTlsSetValue) { ctx.r3.u64 = KeTlsSetValue({base}, ctx.r13.u32, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = KeQueryPerformanceFrequency(); }

// Batch 5: the object manager (verified by sys_object_tests against the NT semantics, see docs/SYS.md).
static uint32_t ThreadOf(PPCContext& ctx, uint8_t* base) { return Guest{base}.Load32(ctx.r13.u32 + 0x100); }
static const int64_t* Timeout(uint8_t* base, uint32_t pointer, int64_t& storage) {
  if (!pointer) return nullptr;
  storage = int64_t(Guest{base}.Load64(pointer));
  return &storage;
}

ANY360_SYS_EXPORT(KeInitializeEvent) { Objects().InitializeEvent({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeSetEvent) { ctx.r3.u64 = Objects().SetEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeResetEvent) { ctx.r3.u64 = Objects().ResetEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(KePulseEvent) { ctx.r3.u64 = Objects().PulseEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeInitializeSemaphore) { Objects().InitializeSemaphore({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeReleaseSemaphore) {
  uint32_t status;
  ctx.r3.u64 = Objects().ReleaseSemaphore({base}, ctx.r3.u32, ctx.r5.u32, status);
}
ANY360_SYS_EXPORT(NtCreateEvent) { ctx.r3.u64 = Objects().CreateEvent({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NtSetEvent) { ctx.r3.u64 = Objects().NtSetEvent({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtClearEvent) { ctx.r3.u64 = Objects().NtClearEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NtPulseEvent) { ctx.r3.u64 = Objects().NtPulseEvent({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtQueryEvent) { ctx.r3.u64 = Objects().NtQueryEvent({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtCreateSemaphore) { ctx.r3.u64 = Objects().CreateSemaphore({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NtReleaseSemaphore) { ctx.r3.u64 = Objects().NtReleaseSemaphore({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NtCreateMutant) {
  ctx.r3.u64 = Objects().CreateMutant({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ThreadOf(ctx, base));
}
ANY360_SYS_EXPORT(NtReleaseMutant) { ctx.r3.u64 = Objects().NtReleaseMutant({base}, ctx.r3.u32, ThreadOf(ctx, base)); }
ANY360_SYS_EXPORT(NtClose) { ctx.r3.u64 = Objects().Close(ctx.r3.u32); }
ANY360_SYS_EXPORT(NtDuplicateObject) { ctx.r3.u64 = Objects().Duplicate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(ObReferenceObjectByHandle) {
  ctx.r3.u64 = Objects().ReferenceObjectByHandle({base}, ctx.r3.u32, ThreadOf(ctx, base), ctx.r5.u32);
}
ANY360_SYS_EXPORT(ObDereferenceObject) { ctx.r3.u64 = Objects().DereferenceObject({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeWaitForSingleObject) {
  int64_t storage;
  ctx.r3.u64 = Objects().WaitForObject({base}, ctx.r3.u32, ThreadOf(ctx, base), Timeout(base, ctx.r7.u32, storage));
}
ANY360_SYS_EXPORT(NtWaitForSingleObjectEx) {
  int64_t storage;
  ctx.r3.u64 = Objects().WaitForHandle({base}, ctx.r3.u32, ThreadOf(ctx, base), Timeout(base, ctx.r6.u32, storage));
}

// Batch 6: the guest clock, timers, multiple-object waits (sys_object_tests).
ANY360_SYS_EXPORT(KeQuerySystemTime) {
  if (ctx.r3.u32) Guest{base}.Store64(ctx.r3.u32, SystemTime());
}
ANY360_SYS_EXPORT(KeInitializeTimerEx) { Objects().InitializeTimer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NtCreateTimer) { ctx.r3.u64 = Objects().CreateTimer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NtSetTimerEx) {
  // (handle, due time, APC routine, 1, APC context, resume, period in ms, previous state out); APCs are not delivered yet.
  ctx.r3.u64 = Objects().NtSetTimerEx({base}, ctx.r3.u32, ctx.r4.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(NtCancelTimer) { ctx.r3.u64 = Objects().NtCancelTimer({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeWaitForMultipleObjects) {
  // (count, objects, wait type, reason, mode, alertable, timeout, wait blocks)
  const uint32_t count = ctx.r3.u32;
  std::vector<uint32_t> objects(count);
  for (uint32_t i = 0; i < count; ++i) objects[i] = Guest{base}.Load32(ctx.r4.u32 + 4 * i);
  int64_t storage;
  ctx.r3.u64 = Objects().WaitForObjects({base}, count, objects.data(), ctx.r5.u32, ThreadOf(ctx, base), Timeout(base, ctx.r9.u32, storage));
}
ANY360_SYS_EXPORT(NtWaitForMultipleObjectsEx) {
  // (count, handles, wait type, mode, alertable, timeout)
  int64_t storage;
  ctx.r3.u64 = Objects().WaitForHandles({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ThreadOf(ctx, base), Timeout(base, ctx.r8.u32, storage));
}

// Batch 7: the scheduler (sys_object_tests).
ANY360_SYS_EXPORT(ExCreateThread) {
  ctx.r3.u64 = Threads().CreateThread({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(ExTerminateThread) { ctx.r3.u64 = Threads().TerminateThread({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeDelayExecutionThread) {
  ctx.r3.u64 = Threads().Delay({base}, ctx.r13.u32, ctx.r5.u32 ? int64_t(Guest{base}.Load64(ctx.r5.u32)) : 0);
}
ANY360_SYS_EXPORT(NtYieldExecution) { ctx.r3.u64 = Threads().Yield({base}, ctx.r13.u32); }
ANY360_SYS_EXPORT(KeResumeThread) { ctx.r3.u64 = Threads().Resume({base}, ctx.r3.u32, nullptr); }
ANY360_SYS_EXPORT(NtResumeThread) { ctx.r3.u64 = Threads().NtResumeThread({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtSuspendThread) { ctx.r3.u64 = Threads().NtSuspendThread({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeSetAffinityThread) { ctx.r3.u64 = Threads().SetAffinity({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = Threads().QueryBasePriority({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeSetBasePriorityThread) { ctx.r3.u64 = Threads().SetBasePriority({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeSetDisableBoostThread) { ctx.r3.u64 = Threads().SetDisableBoost({base}, ctx.r3.u32, ctx.r4.u32); }

// Batch 8: the memory manager (sys_object_tests).
ANY360_SYS_EXPORT(NtAllocateVirtualMemory) {
  ctx.r3.u64 = Memory().AllocateVirtual({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtFreeVirtualMemory) { ctx.r3.u64 = Memory().FreeVirtual({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NtQueryVirtualMemory) { ctx.r3.u64 = Memory().QueryVirtual({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(MmAllocatePhysicalMemoryEx) {
  ctx.r3.u64 = Memory().AllocatePhysical({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}
ANY360_SYS_EXPORT(MmAllocatePhysicalMemory) {
  ctx.r3.u64 = Memory().AllocatePhysical({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0, 0xFFFFFFFF, 0);
}
ANY360_SYS_EXPORT(MmFreePhysicalMemory) { Memory().FreePhysical(ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(MmGetPhysicalAddress) {
  const uint32_t physical = Memory().PhysicalAddress(ctx.r3.u32);
  ctx.r3.u64 = physical == 0xFFFFFFFF ? 0 : physical;
}
ANY360_SYS_EXPORT(MmQueryAddressProtect) { ctx.r3.u64 = Memory().QueryProtect(ctx.r3.u32); }
ANY360_SYS_EXPORT(MmSetAddressProtect) { Memory().SetProtect(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(MmQueryAllocationSize) { ctx.r3.u64 = Memory().QueryAllocationSize(ctx.r3.u32); }
ANY360_SYS_EXPORT(MmQueryStatistics) { ctx.r3.u64 = Memory().QueryStatistics({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamAlloc) { ctx.r3.u64 = Memory().XamAlloc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamFree) { ctx.r3.u64 = Memory().XamFree({base}, ctx.r3.u32); }

// Batch 9: the file system (sys_object_tests).
ANY360_SYS_EXPORT(NtCreateFile) {
  // (handle, access, attributes, I/O status, allocation size, file attributes, share, disposition, options)
  ctx.r3.u64 = Files().CreateFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}
ANY360_SYS_EXPORT(NtOpenFile) {
  // (handle, access, attributes, I/O status, options)
  ctx.r3.u64 = Files().CreateFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, kOpen, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtReadFile) {
  // (handle, event, APC routine, APC context, I/O status, buffer, length, offset); APCs are not delivered yet.
  ctx.r3.u64 = Files().ReadFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(NtWriteFile) {
  ctx.r3.u64 = Files().WriteFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(NtQueryInformationFile) {
  ctx.r3.u64 = Files().QueryInformation({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtSetInformationFile) {
  ctx.r3.u64 = Files().SetInformation({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtQueryFullAttributesFile) { ctx.r3.u64 = Files().QueryFullAttributes({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtQueryDirectoryFile) {
  // (handle, event, APC routine, APC context, I/O status, information, length, file name, restart scan)
  ctx.r3.u64 = Files().QueryDirectory({base}, ctx.r3.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}
ANY360_SYS_EXPORT(NtQueryVolumeInformationFile) {
  ctx.r3.u64 = Files().QueryVolume({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtFlushBuffersFile) { ctx.r3.u64 = Files().Flush({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(ObCreateSymbolicLink) { ctx.r3.u64 = Files().CreateSymbolicLink({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(ObDeleteSymbolicLink) { ctx.r3.u64 = Files().DeleteSymbolicLink({base}, ctx.r3.u32); }

// Batch 10: XAM, the title module, stops and the offline network (sys_object_tests).
ANY360_SYS_EXPORT(XMsgStartIORequest) { ctx.r3.u64 = XamState().MsgStartIORequest({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(XMsgStartIORequestEx) { ctx.r3.u64 = XamState().MsgStartIORequest({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(XMsgInProcessCall) { ctx.r3.u64 = XamState().MsgInProcessCall(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XMsgCancelIORequest) { ctx.r3.u64 = XamState().MsgCancelIORequest({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamNotifyCreateListener) { ctx.r3.u64 = XamState().NotifyCreateListener({base}, ctx.r3.u64, ctx.r4.u32); }
ANY360_SYS_EXPORT(XNotifyGetNext) { ctx.r3.u64 = XamState().NotifyGetNext({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamUserGetSigninState) { ctx.r3.u64 = XamState().UserGetSigninState(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamUserGetXUID) { ctx.r3.u64 = XamState().UserGetXUID({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserGetName) { ctx.r3.u64 = XamState().UserGetName({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserGetSigninInfo) { ctx.r3.u64 = XamState().UserGetSigninInfo({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserCheckPrivilege) { ctx.r3.u64 = XamState().UserCheckPrivilege({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserReadProfileSettings) {
  ctx.r3.u64 = XamState().UserReadProfileSettings({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                                  ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}
ANY360_SYS_EXPORT(XamUserWriteProfileSettings) {
  ctx.r3.u64 = XamState().UserWriteProfileSettings({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamSessionCreateHandle) { ctx.r3.u64 = XamState().SessionCreateHandle({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamSessionRefObjByHandle) { ctx.r3.u64 = XamState().SessionRefObjByHandle({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamShowSigninUI) { ctx.r3.u64 = XamState().ShowSigninUI(ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamShowMessageBoxUI) { ctx.r3.u64 = XamState().ShowMessageBoxUI({base}, ctx.r10.u32, StackArgument(ctx, base, 8)); }
ANY360_SYS_EXPORT(XamShowMessageBoxUIEx) { ctx.r3.u64 = XamState().ShowMessageBoxUI({base}, ctx.r10.u32, StackArgument(ctx, base, 8)); }
ANY360_SYS_EXPORT(XamShowDeviceSelectorUI) { ctx.r3.u64 = XamState().ShowDeviceSelectorUI({base}, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(XamCreateEnumeratorHandle) { ctx.r3.u64 = kErrorInvalidParameter; }
ANY360_SYS_EXPORT(XamEnumerate) {
  ctx.r3.u64 = XamState().Enumerate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}
ANY360_SYS_EXPORT(XamGetPrivateEnumStructureFromHandle) { ctx.r3.u64 = XamState().GetPrivateEnumStructure({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamContentCreateEx) {
  // (user, root name, content data, flags, disposition, license mask, cache size, content size, overlapped)
  ctx.r3.u64 = XamState().ContentCreate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                        StackArgument(ctx, base, 8));
}
ANY360_SYS_EXPORT(XamContentClose) { ctx.r3.u64 = XamState().ContentClose({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamContentDelete) { ctx.r3.u64 = XamState().ContentDelete({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamContentCreateEnumerator) {
  ctx.r3.u64 = XamState().ContentCreateEnumerator({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(XamContentGetDeviceState) { ctx.r3.u64 = XamState().ContentGetDeviceState({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamInputGetCapabilities) { ctx.r3.u64 = XamState().InputGetCapabilities({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamInputGetState) { ctx.r3.u64 = XamState().InputGetState({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamInputSetState) { ctx.r3.u64 = XamState().InputSetState({base}, ctx.r3.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamInputGetKeystrokeEx) {
  const uint32_t user = ctx.r3.u32 ? Guest{base}.Load32(ctx.r3.u32) : 0;
  ctx.r3.u64 = XamState().InputGetKeystroke({base}, user, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(RtlImageXexHeaderField) { ctx.r3.u64 = XamState().ImageXexHeaderField({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XexGetModuleHandle) { ctx.r3.u64 = XamState().GetModuleHandle({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XexGetProcedureAddress) { ctx.r3.u64 = XamState().GetProcedureAddress({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = XamState().CheckExecutablePrivilege({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetExecutionId) { ctx.r3.u64 = XamState().GetExecutionId({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExRegisterTitleTerminateNotification) { XamState().RegisterTitleTerminateNotification(ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeBugCheck) { XamState().Stop("bug check", ctx.r3.u32); }
ANY360_SYS_EXPORT(KeBugCheckEx) { XamState().Stop("bug check", ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlRaiseException) { XamState().RaiseException({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(DbgBreakPoint) {}
ANY360_SYS_EXPORT(HalReturnToFirmware) { XamState().Stop("return to firmware", 0); }
ANY360_SYS_EXPORT(XamLoaderTerminateTitle) { XamState().Stop("title terminated", 0); }
ANY360_SYS_EXPORT(XamLoaderLaunchTitle) { XamState().Stop("title launched another title", 0); }
ANY360_SYS_EXPORT(KeEnableFpuExceptions) {}
ANY360_SYS_EXPORT(KeLockL2) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(KeUnlockL2) {}
ANY360_SYS_EXPORT(KiApcNormalRoutineNop) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_WSAStartup) { ctx.r3.u64 = XamState().WsaStartup({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_XNetStartup) { ctx.r3.u64 = XamState().NetStartup(); }
ANY360_SYS_EXPORT(NetDll_XNetGetTitleXnAddr) { ctx.r3.u64 = XamState().XNetGetTitleXnAddr({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = XamState().WsaGetLastError(); }
ANY360_SYS_EXPORT(NetDll_socket) { ctx.r3.u64 = XamState().Socket(); }
ANY360_SYS_EXPORT(NetDll_closesocket) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_shutdown) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_setsockopt) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_getsockopt) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_bind) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_connect) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_listen) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_accept) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_select) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_recv) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_recvfrom) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_send) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_sendto) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_ioctlsocket) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }

#define ANY360_SYS_ENTRY(name) {#name, &any360_##name}
const Export kExports[] = {
    ANY360_SYS_ENTRY(RtlCompareMemoryUlong), ANY360_SYS_ENTRY(RtlFillMemoryUlong),     ANY360_SYS_ENTRY(RtlUpperChar),
    ANY360_SYS_ENTRY(RtlLowerChar),          ANY360_SYS_ENTRY(RtlCompareString),       ANY360_SYS_ENTRY(RtlCompareStringN),
    ANY360_SYS_ENTRY(RtlInitAnsiString),     ANY360_SYS_ENTRY(RtlInitUnicodeString),   ANY360_SYS_ENTRY(RtlMultiByteToUnicodeN),
    ANY360_SYS_ENTRY(RtlUnicodeToMultiByteN), ANY360_SYS_ENTRY(RtlTimeToTimeFields),   ANY360_SYS_ENTRY(RtlTimeFieldsToTime),
    ANY360_SYS_ENTRY(RtlComputeCrc32),       ANY360_SYS_ENTRY(XGetAVPack),             ANY360_SYS_ENTRY(XGetGameRegion),
    ANY360_SYS_ENTRY(XGetLanguage),          ANY360_SYS_ENTRY(XamGetSystemVersion),
    // Batch 2: configuration, errors, critical sections, video mode, offline networking.
    ANY360_SYS_ENTRY(RtlNtStatusToDosError),
    ANY360_SYS_ENTRY(ExGetXConfigSetting),
    ANY360_SYS_ENTRY(RtlInitializeCriticalSection),
    ANY360_SYS_ENTRY(RtlInitializeCriticalSectionAndSpinCount),
    ANY360_SYS_ENTRY(VdQueryVideoMode),
    ANY360_SYS_ENTRY(XGetVideoMode),
    ANY360_SYS_ENTRY(VdQueryVideoFlags),
    ANY360_SYS_ENTRY(VdGetCurrentDisplayInformation),
    ANY360_SYS_ENTRY(VdGetCurrentDisplayGamma),
    ANY360_SYS_ENTRY(VdSetDisplayMode),
    ANY360_SYS_ENTRY(VdEnableDisableClockGating),
    ANY360_SYS_ENTRY(VdIsHSIOTrainingSucceeded),
    ANY360_SYS_ENTRY(VdRetrainEDRAM),
    ANY360_SYS_ENTRY(VdRetrainEDRAMWorker),
    ANY360_SYS_ENTRY(VdGetGraphicsAsicID),
    ANY360_SYS_ENTRY(VdGetSystemCommandBuffer),
    ANY360_SYS_ENTRY(VdCallGraphicsNotificationRoutines),
    ANY360_SYS_ENTRY(VdInitializeScalerCommandBuffer),
    ANY360_SYS_ENTRY(NetDll_inet_addr),
    ANY360_SYS_ENTRY(NetDll_XNetInAddrToString),
    ANY360_SYS_ENTRY(NetDll_XNetXnAddrToInAddr),
    ANY360_SYS_ENTRY(NetDll_XNetInAddrToXnAddr),
    ANY360_SYS_ENTRY(NetDll___WSAFDIsSet),
    // Batch 3: the CRT formatting family, DbgPrint, RtlCompareMemory, the no-op exception and video helpers.
    ANY360_SYS_ENTRY(sprintf),
    ANY360_SYS_ENTRY(_snprintf),
    ANY360_SYS_ENTRY(swprintf),
    ANY360_SYS_ENTRY(_snwprintf),
    ANY360_SYS_ENTRY(vsprintf),
    ANY360_SYS_ENTRY(_vsnprintf),
    ANY360_SYS_ENTRY(vswprintf),
    ANY360_SYS_ENTRY(_vsnwprintf),
    ANY360_SYS_ENTRY(_vscwprintf),
    ANY360_SYS_ENTRY(DbgPrint),
    ANY360_SYS_ENTRY(RtlCompareMemory),
    ANY360_SYS_ENTRY(RtlCaptureContext),
    ANY360_SYS_ENTRY(RtlUnwind),
    ANY360_SYS_ENTRY(__C_specific_handler),
    ANY360_SYS_ENTRY(VdShutdownEngines),
    ANY360_SYS_ENTRY(VdSetSystemCommandBufferGpuIdentifierAddress),
    // Batch 4: IRQL, spinlocks, critical sections and regions, TLS, process type (verified by sys_runtime_tests).
    ANY360_SYS_ENTRY(KeGetCurrentProcessType),
    ANY360_SYS_ENTRY(KeSetCurrentProcessType),
    ANY360_SYS_ENTRY(KeRaiseIrqlToDpcLevel),
    ANY360_SYS_ENTRY(KfRaiseIrql),
    ANY360_SYS_ENTRY(KfLowerIrql),
    ANY360_SYS_ENTRY(KfAcquireSpinLock),
    ANY360_SYS_ENTRY(KfReleaseSpinLock),
    ANY360_SYS_ENTRY(KeAcquireSpinLockAtRaisedIrql),
    ANY360_SYS_ENTRY(KeTryToAcquireSpinLockAtRaisedIrql),
    ANY360_SYS_ENTRY(KeReleaseSpinLockFromRaisedIrql),
    ANY360_SYS_ENTRY(RtlEnterCriticalSection),
    ANY360_SYS_ENTRY(RtlTryEnterCriticalSection),
    ANY360_SYS_ENTRY(RtlLeaveCriticalSection),
    ANY360_SYS_ENTRY(KeEnterCriticalRegion),
    ANY360_SYS_ENTRY(KeLeaveCriticalRegion),
    ANY360_SYS_ENTRY(KeTlsAlloc),
    ANY360_SYS_ENTRY(KeTlsFree),
    ANY360_SYS_ENTRY(KeTlsGetValue),
    ANY360_SYS_ENTRY(KeTlsSetValue),
    ANY360_SYS_ENTRY(KeQueryPerformanceFrequency),
    // Batch 5: events, semaphores, mutants, handles, waits (sys_object_tests).
    ANY360_SYS_ENTRY(KeInitializeEvent),
    ANY360_SYS_ENTRY(KeSetEvent),
    ANY360_SYS_ENTRY(KeResetEvent),
    ANY360_SYS_ENTRY(KePulseEvent),
    ANY360_SYS_ENTRY(KeInitializeSemaphore),
    ANY360_SYS_ENTRY(KeReleaseSemaphore),
    ANY360_SYS_ENTRY(NtCreateEvent),
    ANY360_SYS_ENTRY(NtSetEvent),
    ANY360_SYS_ENTRY(NtClearEvent),
    ANY360_SYS_ENTRY(NtPulseEvent),
    ANY360_SYS_ENTRY(NtQueryEvent),
    ANY360_SYS_ENTRY(NtCreateSemaphore),
    ANY360_SYS_ENTRY(NtReleaseSemaphore),
    ANY360_SYS_ENTRY(NtCreateMutant),
    ANY360_SYS_ENTRY(NtReleaseMutant),
    ANY360_SYS_ENTRY(NtClose),
    ANY360_SYS_ENTRY(NtDuplicateObject),
    ANY360_SYS_ENTRY(ObReferenceObjectByHandle),
    ANY360_SYS_ENTRY(ObDereferenceObject),
    ANY360_SYS_ENTRY(KeWaitForSingleObject),
    ANY360_SYS_ENTRY(NtWaitForSingleObjectEx),
    // Batch 6: clock, timers, multiple waits.
    ANY360_SYS_ENTRY(KeQuerySystemTime),
    ANY360_SYS_ENTRY(KeInitializeTimerEx),
    ANY360_SYS_ENTRY(NtCreateTimer),
    ANY360_SYS_ENTRY(NtSetTimerEx),
    ANY360_SYS_ENTRY(NtCancelTimer),
    ANY360_SYS_ENTRY(KeWaitForMultipleObjects),
    ANY360_SYS_ENTRY(NtWaitForMultipleObjectsEx),
    // Batch 7: the scheduler.
    ANY360_SYS_ENTRY(ExCreateThread),
    ANY360_SYS_ENTRY(ExTerminateThread),
    ANY360_SYS_ENTRY(KeDelayExecutionThread),
    ANY360_SYS_ENTRY(NtYieldExecution),
    ANY360_SYS_ENTRY(KeResumeThread),
    ANY360_SYS_ENTRY(NtResumeThread),
    ANY360_SYS_ENTRY(NtSuspendThread),
    ANY360_SYS_ENTRY(KeSetAffinityThread),
    ANY360_SYS_ENTRY(KeQueryBasePriorityThread),
    ANY360_SYS_ENTRY(KeSetBasePriorityThread),
    ANY360_SYS_ENTRY(KeSetDisableBoostThread),
    // Batch 8: the memory manager.
    ANY360_SYS_ENTRY(NtAllocateVirtualMemory),
    ANY360_SYS_ENTRY(NtFreeVirtualMemory),
    ANY360_SYS_ENTRY(NtQueryVirtualMemory),
    ANY360_SYS_ENTRY(MmAllocatePhysicalMemoryEx),
    ANY360_SYS_ENTRY(MmAllocatePhysicalMemory),
    ANY360_SYS_ENTRY(MmFreePhysicalMemory),
    ANY360_SYS_ENTRY(MmGetPhysicalAddress),
    ANY360_SYS_ENTRY(MmQueryAddressProtect),
    ANY360_SYS_ENTRY(MmSetAddressProtect),
    ANY360_SYS_ENTRY(MmQueryAllocationSize),
    ANY360_SYS_ENTRY(MmQueryStatistics),
    ANY360_SYS_ENTRY(XamAlloc),
    ANY360_SYS_ENTRY(XamFree),
    // Batch 9: the file system.
    ANY360_SYS_ENTRY(NtCreateFile),
    ANY360_SYS_ENTRY(NtOpenFile),
    ANY360_SYS_ENTRY(NtReadFile),
    ANY360_SYS_ENTRY(NtWriteFile),
    ANY360_SYS_ENTRY(NtQueryInformationFile),
    ANY360_SYS_ENTRY(NtSetInformationFile),
    ANY360_SYS_ENTRY(NtQueryFullAttributesFile),
    ANY360_SYS_ENTRY(NtQueryDirectoryFile),
    ANY360_SYS_ENTRY(NtQueryVolumeInformationFile),
    ANY360_SYS_ENTRY(NtFlushBuffersFile),
    ANY360_SYS_ENTRY(ObCreateSymbolicLink),
    ANY360_SYS_ENTRY(ObDeleteSymbolicLink),
    // Batch 10: XAM, the title module, stops, the offline network.
    ANY360_SYS_ENTRY(XMsgStartIORequest),
    ANY360_SYS_ENTRY(XMsgStartIORequestEx),
    ANY360_SYS_ENTRY(XMsgInProcessCall),
    ANY360_SYS_ENTRY(XMsgCancelIORequest),
    ANY360_SYS_ENTRY(XamNotifyCreateListener),
    ANY360_SYS_ENTRY(XNotifyGetNext),
    ANY360_SYS_ENTRY(XamUserGetSigninState),
    ANY360_SYS_ENTRY(XamUserGetXUID),
    ANY360_SYS_ENTRY(XamUserGetName),
    ANY360_SYS_ENTRY(XamUserGetSigninInfo),
    ANY360_SYS_ENTRY(XamUserCheckPrivilege),
    ANY360_SYS_ENTRY(XamUserReadProfileSettings),
    ANY360_SYS_ENTRY(XamUserWriteProfileSettings),
    ANY360_SYS_ENTRY(XamSessionCreateHandle),
    ANY360_SYS_ENTRY(XamSessionRefObjByHandle),
    ANY360_SYS_ENTRY(XamShowSigninUI),
    ANY360_SYS_ENTRY(XamShowMessageBoxUI),
    ANY360_SYS_ENTRY(XamShowMessageBoxUIEx),
    ANY360_SYS_ENTRY(XamShowDeviceSelectorUI),
    ANY360_SYS_ENTRY(XamCreateEnumeratorHandle),
    ANY360_SYS_ENTRY(XamEnumerate),
    ANY360_SYS_ENTRY(XamGetPrivateEnumStructureFromHandle),
    ANY360_SYS_ENTRY(XamContentCreateEx),
    ANY360_SYS_ENTRY(XamContentClose),
    ANY360_SYS_ENTRY(XamContentDelete),
    ANY360_SYS_ENTRY(XamContentCreateEnumerator),
    ANY360_SYS_ENTRY(XamContentGetDeviceState),
    ANY360_SYS_ENTRY(XamInputGetCapabilities),
    ANY360_SYS_ENTRY(XamInputGetState),
    ANY360_SYS_ENTRY(XamInputSetState),
    ANY360_SYS_ENTRY(XamInputGetKeystrokeEx),
    ANY360_SYS_ENTRY(RtlImageXexHeaderField),
    ANY360_SYS_ENTRY(XexGetModuleHandle),
    ANY360_SYS_ENTRY(XexGetProcedureAddress),
    ANY360_SYS_ENTRY(XexCheckExecutablePrivilege),
    ANY360_SYS_ENTRY(XamGetExecutionId),
    ANY360_SYS_ENTRY(ExRegisterTitleTerminateNotification),
    ANY360_SYS_ENTRY(KeBugCheck),
    ANY360_SYS_ENTRY(KeBugCheckEx),
    ANY360_SYS_ENTRY(RtlRaiseException),
    ANY360_SYS_ENTRY(DbgBreakPoint),
    ANY360_SYS_ENTRY(HalReturnToFirmware),
    ANY360_SYS_ENTRY(XamLoaderTerminateTitle),
    ANY360_SYS_ENTRY(XamLoaderLaunchTitle),
    ANY360_SYS_ENTRY(KeEnableFpuExceptions),
    ANY360_SYS_ENTRY(KeLockL2),
    ANY360_SYS_ENTRY(KeUnlockL2),
    ANY360_SYS_ENTRY(KiApcNormalRoutineNop),
    ANY360_SYS_ENTRY(NetDll_WSAStartup),
    ANY360_SYS_ENTRY(NetDll_WSACleanup),
    ANY360_SYS_ENTRY(NetDll_XNetStartup),
    ANY360_SYS_ENTRY(NetDll_XNetGetTitleXnAddr),
    ANY360_SYS_ENTRY(NetDll_WSAGetLastError),
    ANY360_SYS_ENTRY(NetDll_socket),
    ANY360_SYS_ENTRY(NetDll_closesocket),
    ANY360_SYS_ENTRY(NetDll_shutdown),
    ANY360_SYS_ENTRY(NetDll_setsockopt),
    ANY360_SYS_ENTRY(NetDll_getsockopt),
    ANY360_SYS_ENTRY(NetDll_bind),
    ANY360_SYS_ENTRY(NetDll_connect),
    ANY360_SYS_ENTRY(NetDll_listen),
    ANY360_SYS_ENTRY(NetDll_accept),
    ANY360_SYS_ENTRY(NetDll_select),
    ANY360_SYS_ENTRY(NetDll_recv),
    ANY360_SYS_ENTRY(NetDll_recvfrom),
    ANY360_SYS_ENTRY(NetDll_send),
    ANY360_SYS_ENTRY(NetDll_sendto),
    ANY360_SYS_ENTRY(NetDll_ioctlsocket),
};
const unsigned kExportCount = sizeof(kExports) / sizeof(kExports[0]);

}  // namespace any360::sys
