// PowerPC ABI adapters: one `any360_<Export>(PPCContext&, uint8_t*)` per system call the layer provides, with the same
// calling convention as the kernel import thunks of a static recompilation (arguments in r3..r10, result in r3), so a
// title binds them in place of the reference runtime's `__imp__<Export>`. kExports lists them by name for the binder and
// for tools/build_db.py, which counts every name here as native.
#include <rex/ppc.h>

#include <vector>

#include "any360/sys/config.h"
#include "any360/sys/exports.h"
#include "any360/sys/format.h"
#include "any360/sys/info.h"
#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/rtl.h"
#include "any360/sys/threading.h"
#include "any360/sys/video.h"

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
};
const unsigned kExportCount = sizeof(kExports) / sizeof(kExports[0]);

}  // namespace any360::sys
