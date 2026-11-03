// PowerPC ABI adapters: one `any360_<Export>(PPCContext&, uint8_t*)` per system call the layer provides, with the same
// calling convention as the kernel import thunks of a static recompilation (arguments in r3..r10, result in r3), so a
// title binds them in place of the reference runtime's `__imp__<Export>`. kExports lists them by name for the binder and
// for tools/build_db.py, which counts every name here as native.
#include <rex/ppc.h>

#include <vector>

#include "any360/sys/audio.h"
#include "any360/sys/config.h"
#include "any360/sys/crypt.h"
#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/format.h"
#include "any360/sys/graphics.h"
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
ANY360_SYS_EXPORT(VdQueryRealVideoMode) { VdQueryRealVideoMode({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XGetVideoFlags) { ctx.r3.u64 = XGetVideoFlags(); }
ANY360_SYS_EXPORT(XGetVideoStandard) { ctx.r3.u64 = XGetVideoStandard(); }
ANY360_SYS_EXPORT(XGetVideoCapabilities) { ctx.r3.u64 = XGetVideoCapabilities(); }
// The display hardware a host has no counterpart for: accepted, success (see video.h).
ANY360_SYS_EXPORT(VdBlockUntilGUIIdle) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdDisplayFatalError) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdEnableClosedCaption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdEnableDisablePowerSavingMode) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGenerateGPUCSCCoefficients) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGetClosedCaptionReadyStatus) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGetDisplayModeOverride) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdInitializeScaler) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdReadDVERegisterUlong) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdReadWriteHSIOCalibrationFlag) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdRegisterGraphicsNotification) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdRegisterXamGraphicsNotification) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSendClosedCaptionData) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetCGMSOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetColorProfileAdjustment) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetCscMatricesOverride) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetHDCPOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetMacrovisionOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetWSSData) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetWSSOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdTurnDisplayOff) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdTurnDisplayOn) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdWriteDVERegisterUlong) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdInitializeEDRAM) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdReadEEDIDBlock) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdEnumerateVideoModes) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdEnableHDCP) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdRegisterHDCPNotification) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGetDisplayDiscoveryData) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdStartDisplayDiscovery) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetHDCPRevocationList) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdEnableWMAProOverHDMI) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetCGMSState) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetSCMSState) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGetOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetOption) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdQueryVideoCapabilities) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGet3dVideoFormat) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdGetWSS2Data) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSet3dVideoFormat) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetWSS2Data) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(VdSetStudioRGBMode) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(AniBlockOnAnimation) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(AniTerminateAnimation) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(AniSetLogo) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
ANY360_SYS_EXPORT(AniStartBootAnimation) { ctx.r3.u64 = VdDisplayHardwareRequest(); }
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
// An alertable wait first runs the user APCs queued on the calling thread and then returns STATUS_USER_APC without
// waiting; one that an APC ends runs it the same way.
// The I/O calls' completion APC: routine in r5, context in r6, I/O status block in r7.
static uint32_t IoApc(PPCContext& ctx, uint8_t* base, uint32_t status) {
  Threads().QueueIoApc(ThreadOf(ctx, base), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, status);
  return status;
}
// Kernel-mode APCs run at every wait.
template <typename Wait>
static uint32_t AlertableWait(PPCContext& ctx, uint8_t* base, uint32_t alertable, Wait wait) {
  if (Threads().DeliverApcs({base}, ctx.r13.u32, ctx.r1.u32, alertable != 0) && alertable) return kStatusUserApc;
  const uint32_t status = wait(alertable != 0);
  if (status == kStatusUserApc) Threads().DeliverApcs({base}, ctx.r13.u32, ctx.r1.u32);
  return status;
}
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
  // (object, reason, mode, alertable, timeout)
  int64_t storage;
  ctx.r3.u64 = AlertableWait(ctx, base, ctx.r6.u32, [&](bool alertable) {
    return Objects().WaitForObject({base}, ctx.r3.u32, ThreadOf(ctx, base), Timeout(base, ctx.r7.u32, storage), alertable);
  });
}
ANY360_SYS_EXPORT(NtWaitForSingleObjectEx) {
  // (handle, mode, alertable, timeout)
  int64_t storage;
  ctx.r3.u64 = AlertableWait(ctx, base, ctx.r5.u32, [&](bool alertable) {
    return Objects().WaitForHandle({base}, ctx.r3.u32, ThreadOf(ctx, base), Timeout(base, ctx.r6.u32, storage), alertable);
  });
}

// Batch 6: the guest clock, timers, multiple-object waits (sys_object_tests).
ANY360_SYS_EXPORT(KeQuerySystemTime) {
  if (ctx.r3.u32) Guest{base}.Store64(ctx.r3.u32, SystemTime());
}
ANY360_SYS_EXPORT(KeInitializeTimerEx) { Objects().InitializeTimer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NtCreateTimer) { ctx.r3.u64 = Objects().CreateTimer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NtSetTimerEx) {
  // (handle, due time, APC routine, 1, APC context, resume, period in ms, previous state out)
  ctx.r3.u64 = Objects().NtSetTimerEx({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32, ThreadOf(ctx, base), ctx.r9.u32,
                                      ctx.r10.u32);
}
ANY360_SYS_EXPORT(NtCancelTimer) { ctx.r3.u64 = Objects().NtCancelTimer({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(KeWaitForMultipleObjects) {
  // (count, objects, wait type, reason, mode, alertable, timeout, wait blocks)
  const uint32_t count = ctx.r3.u32;
  std::vector<uint32_t> objects(count);
  for (uint32_t i = 0; i < count; ++i) objects[i] = Guest{base}.Load32(ctx.r4.u32 + 4 * i);
  int64_t storage;
  ctx.r3.u64 = AlertableWait(ctx, base, ctx.r8.u32, [&](bool alertable) {
    return Objects().WaitForObjects({base}, count, objects.data(), ctx.r5.u32, ThreadOf(ctx, base), Timeout(base, ctx.r9.u32, storage),
                                    alertable);
  });
}
ANY360_SYS_EXPORT(NtWaitForMultipleObjectsEx) {
  // (count, handles, wait type, mode, alertable, timeout)
  int64_t storage;
  ctx.r3.u64 = AlertableWait(ctx, base, ctx.r7.u32, [&](bool alertable) {
    return Objects().WaitForHandles({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ThreadOf(ctx, base), Timeout(base, ctx.r8.u32, storage),
                                    alertable);
  });
}

// Batch 7: the scheduler (sys_object_tests).
ANY360_SYS_EXPORT(ExCreateThread) {
  ctx.r3.u64 = Threads().CreateThread({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(ExTerminateThread) { ctx.r3.u64 = Threads().TerminateThread({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(KeDelayExecutionThread) {
  // (mode, alertable, interval)
  ctx.r3.u64 = AlertableWait(ctx, base, ctx.r4.u32, [&](bool alertable) {
    return Threads().Delay({base}, ctx.r13.u32, ctx.r5.u32 ? int64_t(Guest{base}.Load64(ctx.r5.u32)) : 0, alertable);
  });
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
  // (handle, event, APC routine, APC context, I/O status, buffer, length, offset)
  ctx.r3.u64 = IoApc(ctx, base, Files().ReadFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32));
}
ANY360_SYS_EXPORT(NtWriteFile) {
  ctx.r3.u64 = IoApc(ctx, base, Files().WriteFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32));
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
ANY360_SYS_EXPORT(KeInitializeApc) {
  // (apc, thread, kernel routine, rundown routine, normal routine, mode, normal context)
  Threads().KeInitializeApc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(KeInsertQueueApc) { ctx.r3.u64 = Threads().KeInsertQueueApc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeRemoveQueueApc) { ctx.r3.u64 = Threads().KeRemoveQueueApc({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_WSAStartup) { ctx.r3.u64 = XamState().WsaStartup({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_XNetStartup) { ctx.r3.u64 = XamState().XNetStartup({base}, ctx.r4.u32); }
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

// Batch 13: the rest of the offline network Crackdown, The Simpsons Game and TDU import (sys_object_tests).
ANY360_SYS_EXPORT(NetDll_getsockname) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_getpeername) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSAEventSelect) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSACreateEvent) { ctx.r3.u64 = XamState().WsaCreateEvent(); }
ANY360_SYS_EXPORT(NetDll_WSACloseEvent) { ctx.r3.u64 = XamState().WsaCloseEvent(ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_WSAWaitForMultipleEvents) {
  // (count, events, wait all, timeout ms, alertable)
  ctx.r3.u64 = XamState().WsaWaitForMultipleEvents({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ThreadOf(ctx, base));
}
ANY360_SYS_EXPORT(NetDll_XNetRandom) { ctx.r3.u64 = XamState().XNetRandom({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_XNetCreateKey) { ctx.r3.u64 = XamState().XNetCreateKey({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_XNetRegisterKey) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_XNetServerToInAddr) { ctx.r3.u64 = 10050; }  // WSAENETDOWN
ANY360_SYS_EXPORT(NetDll_XNetConnect) { ctx.r3.u64 = 10050; }
ANY360_SYS_EXPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = 3; }  // XNET_CONNECT_STATUS_LOST
ANY360_SYS_EXPORT(NetDll_XNetQosListen) { ctx.r3.u64 = 0x65B; }  // ERROR_FUNCTION_FAILED, as the reference
ANY360_SYS_EXPORT(NetDll_XNetQosLookup) { ctx.r3.u64 = 10050; }
ANY360_SYS_EXPORT(NetDll_XNetQosRelease) { ctx.r3.u64 = XamState().XNetQosRelease(ctx.r4.u32); }
ANY360_SYS_EXPORT(XNetLogonGetTitleID) { ctx.r3.u64 = XamState().TitleId({base}); }

// Batch 14: tasks, voice, the profile's Live answers, guide screens, console keys (sys_object_tests).
ANY360_SYS_EXPORT(XamTaskSchedule) { ctx.r3.u64 = XamState().TaskSchedule({base}, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamTaskCloseHandle) {
  Objects().Close(ctx.r3.u32);
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XamTaskShouldExit) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamVoiceCreate) { ctx.r3.u64 = XamState().VoiceCreate({base}, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamVoiceClose) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamVoiceSubmitPacket) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamUserAreUsersFriends) {
  ctx.r3.u64 = XamState().UserAreUsersFriends({base}, ctx.r3.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamUserCreateAchievementEnumerator) {
  // (title id, user, xuid, flags, offset, count, buffer size out, handle out)
  ctx.r3.u64 = XamState().UserCreateAchievementEnumerator({base}, ctx.r4.u32, ctx.r6.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XamUserCreateStatsEnumerator) { ctx.r3.u64 = kErrorFunctionFailed; }  // leaderboards are Live's
ANY360_SYS_EXPORT(XamUserGetMembershipTierFromXUID) { ctx.r3.u64 = 0; }  // an offline profile has no tier
ANY360_SYS_EXPORT(XamUserGetOnlineCountryFromXUID) { ctx.r3.u64 = Console().country; }
ANY360_SYS_EXPORT(XamParseGamerTileKey) {
  ctx.r3.u64 = XamState().ParseGamerTileKey({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(XamReadTileToTexture) {
  // (unknown, title id, tile id (64-bit), user, buffer, stride, height, overlapped)
  ctx.r3.u64 = XamState().ReadTileToTexture({base}, ctx.r5.u64, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XamShowFriendsUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowGamerCardUIForXUID) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowMarketplaceUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(IoDismountVolumeByFileHandle) { ctx.r3.u64 = 0; }
// The console's key pair is not there to sign with: signatures pass, as the reference answers.
ANY360_SYS_EXPORT(XeKeysConsolePrivateKeySign) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XeKeysConsoleSignatureVerification) { ctx.r3.u64 = 0; }
// The guest's floating-point state is the thread's own: nothing to save around a kernel-mode use.
ANY360_SYS_EXPORT(KeSaveFloatingPointState) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(KeRestoreFloatingPointState) { ctx.r3.u64 = 0; }

// Batch 15: the last of Crackdown's (sys_object_tests).
ANY360_SYS_EXPORT(NtDeviceIoControlFile) {
  // (handle, event, APC, APC context, IO status, code, input, input length, output, output length)
  ctx.r3.u64 = Files().DeviceIoControl({base}, ctx.r7.u32, ctx.r8.u32, StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
}
ANY360_SYS_EXPORT(XexLoadImage) { ctx.r3.u64 = XamState().LoadImage({base}, ctx.r3.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XexUnloadImage) { ctx.r3.u64 = XamState().UnloadImage(ctx.r3.u32); }
// Another executable from a content package (a demo, an offer): no image is loaded besides the title's.
ANY360_SYS_EXPORT(XamContentLaunchImage) { ctx.r3.u64 = kErrorFileNotFound; }

// Batch 16: the rest of Test Drive Unlimited's (sys_object_tests).
ANY360_SYS_EXPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(NetDll_XNetTsAddrToInAddr) { ctx.r3.u64 = 10050; }    // WSAENETDOWN: no title server
ANY360_SYS_EXPORT(NetDll_XNetUnregisterInAddr) { ctx.r3.u64 = 10022; }  // WSAEINVAL: nothing is registered offline
ANY360_SYS_EXPORT(XamLoaderGetDvdTrayState) {
  if (ctx.r3.u32) Guest{base}.Store32(ctx.r3.u32, 1);  // closed, with the title's disc
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XMsgCompleteIORequest) {
  ctx.r3.u64 = XamState().MsgCompleteIORequest({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(XamGetOverlappedResult) {
  ctx.r3.u64 = XamState().GetOverlappedResult({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ThreadOf(ctx, base));
}
ANY360_SYS_EXPORT(XamContentSetThumbnail) {
  ctx.r3.u64 = XamState().ContentSetThumbnail({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamContentGetLicenseMask) { ctx.r3.u64 = XamState().ContentGetLicenseMask({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XNotifyPositionUI) {}  // where notifications appear: nothing is drawn
ANY360_SYS_EXPORT(XNotifyDelayUI) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamShowKeyboardUI) {
  // (user, flags, default text, title, description, buffer, buffer length, overlapped)
  ctx.r3.u64 = XamState().ShowKeyboardUI({base}, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XamWriteGamerTile) { ctx.r3.u64 = XamState().Finish({base}, ctx.r8.u32, kErrorSuccess); }
ANY360_SYS_EXPORT(IoDismountVolume) { ctx.r3.u64 = 0; }  // nothing is cached per volume
ANY360_SYS_EXPORT(IoDismountVolumeByName) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(IoSynchronousDeviceIoControlRequest) {
  // (code, device, input, input length, output, output length, returned length)
  ctx.r3.u64 = Files().SynchronousDeviceIoControl({base}, ctx.r3.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(KeInitializeDpc) { KeInitializeDpc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeInsertQueueDpc) { ctx.r3.u64 = KeInsertQueueDpc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(KeRemoveQueueDpc) { ctx.r3.u64 = KeRemoveQueueDpc({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(MmMapIoSpace) { ctx.r3.u64 = ctx.r4.u32; }  // device registers (XMA contexts) are plain memory here
ANY360_SYS_EXPORT(NtWriteFileGather) {
  ctx.r3.u64 = IoApc(ctx, base, Files().WriteFileGather({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32));
}
ANY360_SYS_EXPORT(ObOpenObjectByName) { ctx.r3.u64 = Objects().OpenObjectByName({base}, ctx.r3.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(ObReferenceObjectByName) { ctx.r3.u64 = Objects().ReferenceObjectByName({base}, ctx.r3.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(XexLoadImageHeaders) { ctx.r3.u64 = XamState().LoadImageHeaders({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(VdSetDisplayModeOverride) { ctx.r3.u64 = 0; }  // the host picks the output mode
ANY360_SYS_EXPORT(XAudioGetSpeakerConfig) { ctx.r3.u64 = Audio().GetSpeakerConfig({base}, ctx.r3.u32); }

// Batch 17: the rest of The Simpsons Game's outside graphics and audio (sys_object_tests).
// Overlapped Winsock on sockets that never exist: SOCKET_ERROR (FALSE for the BOOL ones) and WSAENOTSOCK.
ANY360_SYS_EXPORT(NetDll_WSARecv) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSARecvFrom) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSASend) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSASendTo) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSACancelOverlappedIO) { ctx.r3.u64 = XamState().SocketFailure(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSAGetOverlappedResult) {
  XamState().SocketFailure(ctx.r4.u32);
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(NetDll_WSASetLastError) { XamState().WsaSetLastError(ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_WSASetEvent) { ctx.r3.u64 = XamState().WsaSetEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_WSAResetEvent) { ctx.r3.u64 = XamState().WsaResetEvent({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NetDll_XNetInAddrToServer) { ctx.r3.u64 = 10022; }  // WSAEINVAL: no server address is registered
ANY360_SYS_EXPORT(NetDll_XNetXnAddrToMachineId) {
  if (ctx.r5.u32) Guest{base}.Store64(ctx.r5.u32, 0);
  ctx.r3.u64 = 10022;  // no peer is known offline
}
ANY360_SYS_EXPORT(NetDll_XNetDnsLookup) { ctx.r3.u64 = XamState().XNetDnsLookup({base}, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_XNetDnsRelease) { ctx.r3.u64 = XamState().XNetQosRelease(ctx.r4.u32); }  // the same pool release
ANY360_SYS_EXPORT(NetDll_XNetQosServiceLookup) { ctx.r3.u64 = XamState().XNetQosServiceLookup({base}, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_XNetGetDebugXnAddr) { ctx.r3.u64 = XamState().XNetGetTitleXnAddr({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; }      // no cable
ANY360_SYS_EXPORT(NetDll_XNetGetBroadcastVersionStatus) { ctx.r3.u64 = 0; }  // no newer title version heard
ANY360_SYS_EXPORT(NetDll_XNetQosGetListenStats) { ctx.r3.u64 = 10022; }      // nothing listens (XNetQosListen fails)
ANY360_SYS_EXPORT(NetDll_XNetGetOpt) { ctx.r3.u64 = XamState().XNetGetOpt({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_XNetSetOpt) { ctx.r3.u64 = 0; }  // accepted, no effect without a network
ANY360_SYS_EXPORT(XNetLogonGetMachineID) { ctx.r3.u64 = XamState().LogonGetMachineId({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(Refresh) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XMsgSystemProcessCall) { ctx.r3.u64 = XamState().MsgSystemProcessCall(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamContentCreateDeviceEnumerator) {
  // (content type, flags, count, buffer size, handle)
  ctx.r3.u64 = XamState().ContentCreateDeviceEnumerator({base}, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamContentGetDeviceName) { ctx.r3.u64 = XamState().ContentGetDeviceName({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamContentGetThumbnail) {
  ctx.r3.u64 = XamState().ContentGetThumbnail({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamContentFlush) { ctx.r3.u64 = XamState().Finish({base}, ctx.r4.u32, kErrorSuccess); }  // written through
ANY360_SYS_EXPORT(XamContentResolve) { ctx.r3.u64 = XamState().ContentResolve({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamContentInstall) { ctx.r3.u64 = kErrorFunctionFailed; }  // no package to install offline
ANY360_SYS_EXPORT(XamShowSigninUIp) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowMessagesUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowQuickChatUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowVoiceMailUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowAchievementsUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowPlayerReviewUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowPlayersUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowMessageComposeUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowGameInviteUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(XamShowFriendRequestUI) { ctx.r3.u64 = XamState().ShowLiveUI(); }
ANY360_SYS_EXPORT(NtProtectVirtualMemory) {
  ctx.r3.u64 = Memory().ProtectVirtual({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(NtQueueApcThread) {
  ctx.r3.u64 = Threads().QueueApc({base}, ctx.r3.u32, ThreadOf(ctx, base), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtSignalAndWaitForSingleObjectEx) {
  // (signal, wait, mode, alertable, timeout): the signal always happens, so APCs run only once the wait ends for them.
  int64_t storage;
  ctx.r3.u64 = Objects().SignalAndWait({base}, ctx.r3.u32, ctx.r4.u32, ThreadOf(ctx, base), Timeout(base, ctx.r7.u32, storage),
                                       ctx.r6.u32 != 0);
  if (ctx.r3.u32 == kStatusUserApc) Threads().DeliverApcs({base}, ctx.r13.u32, ctx.r1.u32);
}
ANY360_SYS_EXPORT(ObLookupThreadByThreadId) { ctx.r3.u64 = Threads().LookupThreadById({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(ObOpenObjectByPointer) { ctx.r3.u64 = Objects().OpenObjectByPointer({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XexUnloadImageAndExitThread) {
  XamState().UnloadImage(ctx.r3.u32);
  ctx.r3.u64 = Threads().TerminateThread({base}, ctx.r13.u32, ctx.r4.u32);
}

// Batch 11: what Sonic '06 still needed (sys_io_tests for XeCryptSha, sys_object_tests for the rest).
ANY360_SYS_EXPORT(XexGetModuleSection) { ctx.r3.u64 = XamState().GetModuleSection({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(StfsCreateDevice) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(StfsControlDevice) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(ExAllocatePoolTypeWithTag) { ctx.r3.u64 = XamState().AllocatePool({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExAllocatePoolWithTag) { ctx.r3.u64 = XamState().AllocatePool({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExAllocatePool) { ctx.r3.u64 = XamState().AllocatePool({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExFreePool) { XamState().FreePool(ctx.r3.u32); }
ANY360_SYS_EXPORT(XeCryptSha) {
  XeCryptSha({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XeCryptRotSumSha) { XeCryptRotSumSha({base}, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(XeCryptBnQwBeSigVerify) { ctx.r3.u64 = XeCryptBnQwBeSigVerify(); }
ANY360_SYS_EXPORT(XeKeysGetKey) { ctx.r3.u64 = XeKeysGetKey({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamLoaderSetLaunchData) { ctx.r3.u64 = XamState().LoaderSetLaunchData({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamLoaderGetLaunchDataSize) { ctx.r3.u64 = XamState().LoaderGetLaunchDataSize({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamLoaderGetLaunchData) { ctx.r3.u64 = XamState().LoaderGetLaunchData({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamContentGetDeviceData) { ctx.r3.u64 = XamState().ContentGetDeviceData({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamContentGetCreator) {
  ctx.r3.u64 = XamState().ContentGetCreator({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamShowDirtyDiscErrorUI) { XamState().Stop("disc read error", 0); }
ANY360_SYS_EXPORT(FscSetCacheElementCount) { ctx.r3.u64 = XamState().SetCacheElementCount(ctx.r4.u32); }
ANY360_SYS_EXPORT(FscGetCacheElementCount) { ctx.r3.u64 = XamState().GetCacheElementCount(); }
ANY360_SYS_EXPORT(RtlFreeAnsiString) { RtlFreeAnsiString({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlUnicodeStringToAnsiString) { ctx.r3.u64 = RtlUnicodeStringToAnsiString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(RtlUpcaseUnicodeChar) { ctx.r3.u64 = RtlUpcaseUnicodeChar(ctx.r3.u32); }
ANY360_SYS_EXPORT(NtReadFileScatter) {
  // (handle, event, APC routine, APC context, I/O status, segments, length, offset)
  ctx.r3.u64 = IoApc(ctx, base, Files().ReadFileScatter({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32));
}
ANY360_SYS_EXPORT(XamEnableInactivityProcessing) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamResetInactivity) {}
ANY360_SYS_EXPORT(IoCreateDevice) { ctx.r3.u64 = XamState().CreateDevice({base}, ctx.r8.u32); }
ANY360_SYS_EXPORT(IoDeleteDevice) {}
ANY360_SYS_EXPORT(IoCheckShareAccess) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(IoSetShareAccess) {}
ANY360_SYS_EXPORT(IoRemoveShareAccess) {}
ANY360_SYS_EXPORT(IoCompleteRequest) {}
ANY360_SYS_EXPORT(IoInvalidDeviceRequest) { ctx.r3.u64 = 0xC0000010; }  // STATUS_INVALID_DEVICE_REQUEST
ANY360_SYS_EXPORT(ObIsTitleObject) { ctx.r3.u64 = 1; }
ANY360_SYS_EXPORT(ObReferenceObject) {
  Objects().ReferenceObject(ctx.r3.u32);
  ctx.r3.u64 = 0;
}

// Batch 12: the kernel side of graphics and audio (sys_object_tests). The GPU and the audio output are backends.
ANY360_SYS_EXPORT(VdInitializeEngines) { ctx.r3.u64 = Graphics().InitializeEngines(); }
ANY360_SYS_EXPORT(VdInitializeRingBuffer) { Graphics().InitializeRingBuffer({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(VdEnableRingBufferRPtrWriteBack) { Graphics().EnableReadPointerWriteBack({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(VdSetGraphicsInterruptCallback) { Graphics().SetInterruptCallback({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(VdPersistDisplay) { ctx.r3.u64 = Graphics().PersistDisplay({base}, ctx.r4.u32); }
// (buffer, fetch, system writeback, command buffer, its id, frontbuffer address ptr, format ptr, colour space ptr,
// width ptr, height ptr)
ANY360_SYS_EXPORT(VdSwap) {
  Graphics().Swap({base}, ctx.r3.u32, ctx.r4.u32, ctx.r8.u32, StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XAudioRegisterRenderDriverClient) { ctx.r3.u64 = Audio().RegisterClient({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XAudioUnregisterRenderDriverClient) { ctx.r3.u64 = Audio().UnregisterClient(ctx.r3.u32); }
ANY360_SYS_EXPORT(XAudioSubmitRenderDriverFrame) { ctx.r3.u64 = Audio().SubmitFrame({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
  ctx.r3.u64 = Audio().GetVoiceCategoryVolumeChangeMask({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(XAudioGetVoiceCategoryVolume) { ctx.r3.u64 = Audio().GetVoiceCategoryVolume({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(XMACreateContext) { ctx.r3.u64 = Audio().XmaCreateContext({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XMAReleaseContext) { ctx.r3.u64 = Audio().XmaReleaseContext({base}, ctx.r3.u32); }

// Batch 18: the kernel side of The Simpsons Game's audio and video (sys_object_tests).
ANY360_SYS_EXPORT(XMAInitializeContext) { ctx.r3.u64 = Audio().XmaInitializeContext({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XMASetLoopData) { ctx.r3.u64 = Audio().XmaSetLoopData({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XMASetInputBuffer0) { ctx.r3.u64 = Audio().XmaSetInputBuffer({base}, ctx.r3.u32, 0, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XMASetInputBuffer1) { ctx.r3.u64 = Audio().XmaSetInputBuffer({base}, ctx.r3.u32, 1, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XMAEnableContext) { ctx.r3.u64 = Audio().XmaEnableContext({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XMADisableContext) { ctx.r3.u64 = Audio().XmaDisableContext({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XMABlockWhileInUse) { ctx.r3.u64 = Audio().XmaBlockWhileInUse({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XMAIsInputBuffer0Valid) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kInput0Valid); }
ANY360_SYS_EXPORT(XMAIsInputBuffer1Valid) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kInput1Valid); }
ANY360_SYS_EXPORT(XMAIsOutputBufferValid) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kOutputValid); }
ANY360_SYS_EXPORT(XMAGetOutputBufferReadOffset) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kOutputReadOffset); }
ANY360_SYS_EXPORT(XMAGetOutputBufferWriteOffset) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kOutputWriteOffset); }
ANY360_SYS_EXPORT(XMAGetInputBufferReadOffset) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kInputReadOffset); }
ANY360_SYS_EXPORT(XMAGetPacketMetadata) { ctx.r3.u64 = Audio().XmaGet({base}, ctx.r3.u32, XmaField::kPacketMetadata); }
ANY360_SYS_EXPORT(XMASetInputBuffer0Valid) { Audio().XmaSet({base}, ctx.r3.u32, XmaField::kInput0Valid, 1), ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XMASetInputBuffer1Valid) { Audio().XmaSet({base}, ctx.r3.u32, XmaField::kInput1Valid, 1), ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XMASetOutputBufferValid) { Audio().XmaSet({base}, ctx.r3.u32, XmaField::kOutputValid, 1), ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XMASetOutputBufferReadOffset) { Audio().XmaSet({base}, ctx.r3.u32, XmaField::kOutputReadOffset, ctx.r4.u32), ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XMASetInputBufferReadOffset) { Audio().XmaSet({base}, ctx.r3.u32, XmaField::kInputReadOffset, ctx.r4.u32), ctx.r3.u64 = 0; }
// Undocumented; the reference answers success and writes nothing.
ANY360_SYS_EXPORT(VdQuerySystemCommandBuffer) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(VdSetSystemCommandBuffer) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XAudioQueryDriverPerformance) { ctx.r3.u64 = 0; }

// Batch 19: the XeCrypt / XeKeys cryptography (sys_io_tests against the reference).
ANY360_SYS_EXPORT(XeCryptShaInit) { XeCryptShaInit({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XeCryptShaUpdate) { XeCryptShaUpdate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptShaFinal) { XeCryptShaFinal({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptSha256Init) { XeCryptSha256Init({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XeCryptSha256Update) { XeCryptSha256Update({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptSha256Final) { XeCryptSha256Final({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptHmacSha) {
  XeCryptHmacSha({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
}
ANY360_SYS_EXPORT(XeCryptRc4Key) { XeCryptRc4Key({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptRc4Ecb) { XeCryptRc4Ecb({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptRc4) { XeCryptRc4({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XeCryptAesKey) { XeCryptAesKey({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeCryptAesEcb) { XeCryptAesEcb({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XeCryptAesCbc) { XeCryptAesCbc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(XeCryptDes3Key) { XeCryptDes3Key({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeCryptDes3Ecb) { XeCryptDes3Ecb({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XeCryptDes3Cbc) { XeCryptDes3Cbc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(XeCryptDesParity) { XeCryptDesParity({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptBnQw_SwapDwQwLeBe) { XeCryptBnQwSwapDwQwLeBe({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptRandom) { XeCryptRandom({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeCryptBnDwLePkcs1Verify) { ctx.r3.u64 = XeCryptBnDwLePkcs1Verify(); }
ANY360_SYS_EXPORT(XeKeysHmacSha) {
  ctx.r3.u64 = XeKeysHmacSha({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}
ANY360_SYS_EXPORT(XeKeysObscureKey) { ctx.r3.u64 = XeKeysObscureKey({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeKeysAesCbcUsingKey) { ctx.r3.u64 = XeKeysAesCbcUsingKey({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(XeKeysHmacShaUsingKey) {
  ctx.r3.u64 = XeKeysHmacShaUsingKey({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}

// Batch 20: the Win32 file calls XAM exports, on any360::sys's file system (sys_object_tests).
ANY360_SYS_EXPORT(CreateFileA) { ctx.r3.u64 = Files().Win32CreateFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(ReadFile) { ctx.r3.u64 = Files().Win32Transfer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, false); }
ANY360_SYS_EXPORT(WriteFile) { ctx.r3.u64 = Files().Win32Transfer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, true); }
ANY360_SYS_EXPORT(SetFilePointer) { ctx.r3.u64 = Files().Win32SetFilePointer({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(SetFilePointerEx) { ctx.r3.u64 = Files().Win32SetFilePointerEx({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(GetFileSize) { ctx.r3.u64 = Files().Win32GetFileSize({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(GetFileSizeEx) { ctx.r3.u64 = Files().Win32GetFileSizeEx({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(SetEndOfFile) { ctx.r3.u64 = Files().Win32SetEndOfFile(ctx.r3.u32); }
ANY360_SYS_EXPORT(FlushFileBuffers) { ctx.r3.u64 = Files().Win32FlushFileBuffers(ctx.r3.u32); }
ANY360_SYS_EXPORT(DeleteFileA) { ctx.r3.u64 = Files().Win32DeleteFile({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(CreateDirectoryA) { ctx.r3.u64 = Files().Win32CreateDirectory({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(MoveFileA) { ctx.r3.u64 = Files().Win32MoveFile({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(CopyFileA) { ctx.r3.u64 = Files().Win32CopyFile({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(GetFileAttributesA) { ctx.r3.u64 = Files().Win32GetFileAttributes({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(GetFileAttributesExA) { ctx.r3.u64 = Files().Win32GetFileAttributesEx({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(GetDiskFreeSpaceExA) { ctx.r3.u64 = Files().Win32GetDiskFreeSpace({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(FindFirstFileA) { ctx.r3.u64 = Files().Win32FindFirstFile({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(FindNextFileA) { ctx.r3.u64 = Files().Win32FindNextFile({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(CloseHandle) { ctx.r3.u64 = ctx.r3.u32 && ctx.r3.u32 != 0xFFFFFFFF && Objects().Close(ctx.r3.u32) == 0; }
// Attributes and times are not kept apart from the host's: accepted, as the reference.
ANY360_SYS_EXPORT(SetFileAttributesA) { ctx.r3.u64 = 1; }
ANY360_SYS_EXPORT(SetFileTime) { ctx.r3.u64 = 1; }
ANY360_SYS_EXPORT(CompareFileTime) {
  // FILETIMEs: low word, then high word.
  const Guest g{base};
  const uint64_t a = uint64_t(g.Load32(ctx.r3.u32 + 4)) << 32 | g.Load32(ctx.r3.u32);
  const uint64_t b = uint64_t(g.Load32(ctx.r4.u32 + 4)) << 32 | g.Load32(ctx.r4.u32);
  ctx.r3.u64 = a < b ? 0xFFFFFFFF : a > b ? 1 : 0;
}

// Batch 21: read/write locks and interlocked lists (sys_runtime_tests against the reference, sys_object_tests).
ANY360_SYS_EXPORT(ExInitializeReadWriteLock) { ExInitializeReadWriteLock({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExAcquireReadWriteLockExclusive) { ExAcquireReadWriteLockExclusive({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExTryToAcquireReadWriteLockExclusive) { ctx.r3.u64 = ExTryToAcquireReadWriteLockExclusive({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExAcquireReadWriteLockShared) { ExAcquireReadWriteLockShared({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExTryToAcquireReadWriteLockShared) { ctx.r3.u64 = ExTryToAcquireReadWriteLockShared({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(ExReleaseReadWriteLock) { ExReleaseReadWriteLock({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(InterlockedPushEntrySList) { ctx.r3.u64 = InterlockedPushEntrySList({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(InterlockedPopEntrySList) { ctx.r3.u64 = InterlockedPopEntrySList({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(InterlockedFlushSList) { ctx.r3.u64 = InterlockedFlushSList({base}, ctx.r3.u32); }

// Batch 22: XAM's locales, date and time strings and resource locators (sys_io_tests against the reference).
ANY360_SYS_EXPORT(XamGetLocale) { ctx.r3.u64 = XamGetLocale(); }
ANY360_SYS_EXPORT(XamGetLocaleEx) { ctx.r3.u64 = XamGetLocaleEx(ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamGetOnlineCountryFromLocale) { ctx.r3.u64 = XamGetOnlineCountryFromLocale(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetLocaleFromOnlineCountry) { ctx.r3.u64 = XamGetLocaleFromOnlineCountry(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetLanguageFromOnlineLanguage) { ctx.r3.u64 = XamGetLanguageFromOnlineLanguage(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetCountryFromOnlineCountry) { ctx.r3.u64 = XamGetCountryFromOnlineCountry(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetOnlineCountryString) { ctx.r3.u64 = XamGetOnlineCountryString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetCountryString) { ctx.r3.u64 = XamGetCountryString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetLanguageString) { ctx.r3.u64 = XamGetLanguageString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetLocaleString) { ctx.r3.u64 = XamGetLocaleString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetOnlineLanguageString) { ctx.r3.u64 = XamGetOnlineLanguageString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetLanguageLocaleString) { ctx.r3.u64 = XamGetLanguageLocaleString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamGetOnlineLanguageAndCountryString) { ctx.r3.u64 = XamGetOnlineLanguageAndCountryString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamFormatDateString) { XamFormatDateString({base}, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamFormatTimeString) { XamFormatTimeString({base}, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamBuildResourceLocator) { ctx.r3.u64 = XamBuildResourceLocator({base}, ctx.r3.u64, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(XamBuildGamercardResourceLocator) { ctx.r3.u64 = XamBuildSystemResourceLocator({base}, u"gamercrd", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamBuildSharedSystemResourceLocator) { ctx.r3.u64 = XamBuildSystemResourceLocator({base}, u"shrdres", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamBuildLegacySystemResourceLocator) { ctx.r3.u64 = XamBuildSystemResourceLocator({base}, u"shrdres", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamBuildXamResourceLocator) { ctx.r3.u64 = XamBuildSystemResourceLocator({base}, u"xam", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }

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
    ANY360_SYS_ENTRY(XGetVideoFlags),
    ANY360_SYS_ENTRY(XGetVideoStandard),
    ANY360_SYS_ENTRY(XGetVideoCapabilities),
    ANY360_SYS_ENTRY(VdQueryRealVideoMode),
    ANY360_SYS_ENTRY(VdBlockUntilGUIIdle),
    ANY360_SYS_ENTRY(VdDisplayFatalError),
    ANY360_SYS_ENTRY(VdEnableClosedCaption),
    ANY360_SYS_ENTRY(VdEnableDisablePowerSavingMode),
    ANY360_SYS_ENTRY(VdGenerateGPUCSCCoefficients),
    ANY360_SYS_ENTRY(VdGetClosedCaptionReadyStatus),
    ANY360_SYS_ENTRY(VdGetDisplayModeOverride),
    ANY360_SYS_ENTRY(VdInitializeScaler),
    ANY360_SYS_ENTRY(VdReadDVERegisterUlong),
    ANY360_SYS_ENTRY(VdReadWriteHSIOCalibrationFlag),
    ANY360_SYS_ENTRY(VdRegisterGraphicsNotification),
    ANY360_SYS_ENTRY(VdRegisterXamGraphicsNotification),
    ANY360_SYS_ENTRY(VdSendClosedCaptionData),
    ANY360_SYS_ENTRY(VdSetCGMSOption),
    ANY360_SYS_ENTRY(VdSetColorProfileAdjustment),
    ANY360_SYS_ENTRY(VdSetCscMatricesOverride),
    ANY360_SYS_ENTRY(VdSetHDCPOption),
    ANY360_SYS_ENTRY(VdSetMacrovisionOption),
    ANY360_SYS_ENTRY(VdSetWSSData),
    ANY360_SYS_ENTRY(VdSetWSSOption),
    ANY360_SYS_ENTRY(VdTurnDisplayOff),
    ANY360_SYS_ENTRY(VdTurnDisplayOn),
    ANY360_SYS_ENTRY(VdWriteDVERegisterUlong),
    ANY360_SYS_ENTRY(VdInitializeEDRAM),
    ANY360_SYS_ENTRY(VdReadEEDIDBlock),
    ANY360_SYS_ENTRY(VdEnumerateVideoModes),
    ANY360_SYS_ENTRY(VdEnableHDCP),
    ANY360_SYS_ENTRY(VdRegisterHDCPNotification),
    ANY360_SYS_ENTRY(VdGetDisplayDiscoveryData),
    ANY360_SYS_ENTRY(VdStartDisplayDiscovery),
    ANY360_SYS_ENTRY(VdSetHDCPRevocationList),
    ANY360_SYS_ENTRY(VdEnableWMAProOverHDMI),
    ANY360_SYS_ENTRY(VdSetCGMSState),
    ANY360_SYS_ENTRY(VdSetSCMSState),
    ANY360_SYS_ENTRY(VdGetOption),
    ANY360_SYS_ENTRY(VdSetOption),
    ANY360_SYS_ENTRY(VdQueryVideoCapabilities),
    ANY360_SYS_ENTRY(VdGet3dVideoFormat),
    ANY360_SYS_ENTRY(VdGetWSS2Data),
    ANY360_SYS_ENTRY(VdSet3dVideoFormat),
    ANY360_SYS_ENTRY(VdSetWSS2Data),
    ANY360_SYS_ENTRY(VdSetStudioRGBMode),
    ANY360_SYS_ENTRY(AniBlockOnAnimation),
    ANY360_SYS_ENTRY(AniTerminateAnimation),
    ANY360_SYS_ENTRY(AniSetLogo),
    ANY360_SYS_ENTRY(AniStartBootAnimation),
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
    ANY360_SYS_ENTRY(KeInitializeApc),
    ANY360_SYS_ENTRY(KeInsertQueueApc),
    ANY360_SYS_ENTRY(KeRemoveQueueApc),
    ANY360_SYS_ENTRY(NetDll_WSAStartup),
    ANY360_SYS_ENTRY(NetDll_WSACleanup),
    ANY360_SYS_ENTRY(NetDll_XNetStartup),
    ANY360_SYS_ENTRY(NetDll_XNetGetTitleXnAddr),
    ANY360_SYS_ENTRY(NetDll_WSAGetLastError),
    ANY360_SYS_ENTRY(NetDll_socket),
    ANY360_SYS_ENTRY(NetDll_getsockname),
    ANY360_SYS_ENTRY(NetDll_getpeername),
    ANY360_SYS_ENTRY(NetDll_WSAEventSelect),
    ANY360_SYS_ENTRY(NetDll_WSACreateEvent),
    ANY360_SYS_ENTRY(NetDll_WSACloseEvent),
    ANY360_SYS_ENTRY(NetDll_WSAWaitForMultipleEvents),
    ANY360_SYS_ENTRY(NetDll_XNetRandom),
    ANY360_SYS_ENTRY(NetDll_XNetCreateKey),
    ANY360_SYS_ENTRY(NetDll_XNetRegisterKey),
    ANY360_SYS_ENTRY(NetDll_XNetUnregisterKey),
    ANY360_SYS_ENTRY(NetDll_XNetServerToInAddr),
    ANY360_SYS_ENTRY(NetDll_XNetConnect),
    ANY360_SYS_ENTRY(NetDll_XNetGetConnectStatus),
    ANY360_SYS_ENTRY(NetDll_XNetQosListen),
    ANY360_SYS_ENTRY(NetDll_XNetQosLookup),
    ANY360_SYS_ENTRY(NetDll_XNetQosRelease),
    ANY360_SYS_ENTRY(XNetLogonGetTitleID),
    ANY360_SYS_ENTRY(XamTaskSchedule),
    ANY360_SYS_ENTRY(XamTaskCloseHandle),
    ANY360_SYS_ENTRY(XamTaskShouldExit),
    ANY360_SYS_ENTRY(XamVoiceCreate),
    ANY360_SYS_ENTRY(XamVoiceClose),
    ANY360_SYS_ENTRY(XamVoiceHeadsetPresent),
    ANY360_SYS_ENTRY(XamVoiceSubmitPacket),
    ANY360_SYS_ENTRY(XamUserAreUsersFriends),
    ANY360_SYS_ENTRY(XamUserCreateAchievementEnumerator),
    ANY360_SYS_ENTRY(XamUserCreateStatsEnumerator),
    ANY360_SYS_ENTRY(XamUserGetMembershipTierFromXUID),
    ANY360_SYS_ENTRY(XamUserGetOnlineCountryFromXUID),
    ANY360_SYS_ENTRY(XamParseGamerTileKey),
    ANY360_SYS_ENTRY(XamReadTileToTexture),
    ANY360_SYS_ENTRY(XamShowFriendsUI),
    ANY360_SYS_ENTRY(XamShowGamerCardUIForXUID),
    ANY360_SYS_ENTRY(XamShowMarketplaceUI),
    ANY360_SYS_ENTRY(IoDismountVolumeByFileHandle),
    ANY360_SYS_ENTRY(XeKeysConsolePrivateKeySign),
    ANY360_SYS_ENTRY(XeKeysConsoleSignatureVerification),
    ANY360_SYS_ENTRY(KeSaveFloatingPointState),
    ANY360_SYS_ENTRY(KeRestoreFloatingPointState),
    ANY360_SYS_ENTRY(NtDeviceIoControlFile),
    ANY360_SYS_ENTRY(XexLoadImage),
    ANY360_SYS_ENTRY(XexUnloadImage),
    ANY360_SYS_ENTRY(XamContentLaunchImage),
    ANY360_SYS_ENTRY(NetDll_XNetCleanup),
    ANY360_SYS_ENTRY(NetDll_XNetTsAddrToInAddr),
    ANY360_SYS_ENTRY(NetDll_XNetUnregisterInAddr),
    ANY360_SYS_ENTRY(XamLoaderGetDvdTrayState),
    ANY360_SYS_ENTRY(XMsgCompleteIORequest),
    ANY360_SYS_ENTRY(XamGetOverlappedResult),
    ANY360_SYS_ENTRY(XamContentSetThumbnail),
    ANY360_SYS_ENTRY(XamContentGetLicenseMask),
    ANY360_SYS_ENTRY(XNotifyPositionUI),
    ANY360_SYS_ENTRY(XNotifyDelayUI),
    ANY360_SYS_ENTRY(XamShowKeyboardUI),
    ANY360_SYS_ENTRY(XamWriteGamerTile),
    ANY360_SYS_ENTRY(IoDismountVolume),
    ANY360_SYS_ENTRY(IoDismountVolumeByName),
    ANY360_SYS_ENTRY(IoSynchronousDeviceIoControlRequest),
    ANY360_SYS_ENTRY(KeInitializeDpc),
    ANY360_SYS_ENTRY(KeInsertQueueDpc),
    ANY360_SYS_ENTRY(KeRemoveQueueDpc),
    ANY360_SYS_ENTRY(MmMapIoSpace),
    ANY360_SYS_ENTRY(NtWriteFileGather),
    ANY360_SYS_ENTRY(ObOpenObjectByName),
    ANY360_SYS_ENTRY(ObReferenceObjectByName),
    ANY360_SYS_ENTRY(XexLoadImageHeaders),
    ANY360_SYS_ENTRY(VdSetDisplayModeOverride),
    ANY360_SYS_ENTRY(XAudioGetSpeakerConfig),
    ANY360_SYS_ENTRY(NetDll_WSARecv),
    ANY360_SYS_ENTRY(NetDll_WSARecvFrom),
    ANY360_SYS_ENTRY(NetDll_WSASend),
    ANY360_SYS_ENTRY(NetDll_WSASendTo),
    ANY360_SYS_ENTRY(NetDll_WSACancelOverlappedIO),
    ANY360_SYS_ENTRY(NetDll_WSAGetOverlappedResult),
    ANY360_SYS_ENTRY(NetDll_WSASetLastError),
    ANY360_SYS_ENTRY(NetDll_WSASetEvent),
    ANY360_SYS_ENTRY(NetDll_WSAResetEvent),
    ANY360_SYS_ENTRY(NetDll_XNetInAddrToServer),
    ANY360_SYS_ENTRY(NetDll_XNetXnAddrToMachineId),
    ANY360_SYS_ENTRY(NetDll_XNetDnsLookup),
    ANY360_SYS_ENTRY(NetDll_XNetDnsRelease),
    ANY360_SYS_ENTRY(NetDll_XNetQosServiceLookup),
    ANY360_SYS_ENTRY(NetDll_XNetGetDebugXnAddr),
    ANY360_SYS_ENTRY(NetDll_XNetGetEthernetLinkStatus),
    ANY360_SYS_ENTRY(NetDll_XNetGetBroadcastVersionStatus),
    ANY360_SYS_ENTRY(NetDll_XNetQosGetListenStats),
    ANY360_SYS_ENTRY(NetDll_XNetGetOpt),
    ANY360_SYS_ENTRY(NetDll_XNetSetOpt),
    ANY360_SYS_ENTRY(XNetLogonGetMachineID),
    ANY360_SYS_ENTRY(Refresh),
    ANY360_SYS_ENTRY(XMsgSystemProcessCall),
    ANY360_SYS_ENTRY(XamContentCreateDeviceEnumerator),
    ANY360_SYS_ENTRY(XamContentGetDeviceName),
    ANY360_SYS_ENTRY(XamContentGetThumbnail),
    ANY360_SYS_ENTRY(XamContentFlush),
    ANY360_SYS_ENTRY(XamContentResolve),
    ANY360_SYS_ENTRY(XamContentInstall),
    ANY360_SYS_ENTRY(XamShowSigninUIp),
    ANY360_SYS_ENTRY(XamShowMessagesUI),
    ANY360_SYS_ENTRY(XamShowQuickChatUI),
    ANY360_SYS_ENTRY(XamShowVoiceMailUI),
    ANY360_SYS_ENTRY(XamShowAchievementsUI),
    ANY360_SYS_ENTRY(XamShowPlayerReviewUI),
    ANY360_SYS_ENTRY(XamShowPlayersUI),
    ANY360_SYS_ENTRY(XamShowMessageComposeUI),
    ANY360_SYS_ENTRY(XamShowGameInviteUI),
    ANY360_SYS_ENTRY(XamShowFriendRequestUI),
    ANY360_SYS_ENTRY(NtProtectVirtualMemory),
    ANY360_SYS_ENTRY(NtQueueApcThread),
    ANY360_SYS_ENTRY(NtSignalAndWaitForSingleObjectEx),
    ANY360_SYS_ENTRY(ObLookupThreadByThreadId),
    ANY360_SYS_ENTRY(ObOpenObjectByPointer),
    ANY360_SYS_ENTRY(XexUnloadImageAndExitThread),
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
    // Batch 11: the rest of Sonic '06.
    ANY360_SYS_ENTRY(XexGetModuleSection),
    ANY360_SYS_ENTRY(StfsCreateDevice),
    ANY360_SYS_ENTRY(StfsControlDevice),
    ANY360_SYS_ENTRY(ExAllocatePoolTypeWithTag),
    ANY360_SYS_ENTRY(ExAllocatePoolWithTag),
    ANY360_SYS_ENTRY(ExAllocatePool),
    ANY360_SYS_ENTRY(ExFreePool),
    ANY360_SYS_ENTRY(XeCryptSha),
    ANY360_SYS_ENTRY(XamGetLocale),
    ANY360_SYS_ENTRY(XamGetLocaleEx),
    ANY360_SYS_ENTRY(XamGetOnlineCountryFromLocale),
    ANY360_SYS_ENTRY(XamGetLocaleFromOnlineCountry),
    ANY360_SYS_ENTRY(XamGetLanguageFromOnlineLanguage),
    ANY360_SYS_ENTRY(XamGetCountryFromOnlineCountry),
    ANY360_SYS_ENTRY(XamGetOnlineCountryString),
    ANY360_SYS_ENTRY(XamGetCountryString),
    ANY360_SYS_ENTRY(XamGetLanguageString),
    ANY360_SYS_ENTRY(XamGetLocaleString),
    ANY360_SYS_ENTRY(XamGetOnlineLanguageString),
    ANY360_SYS_ENTRY(XamGetLanguageLocaleString),
    ANY360_SYS_ENTRY(XamGetOnlineLanguageAndCountryString),
    ANY360_SYS_ENTRY(XamFormatDateString),
    ANY360_SYS_ENTRY(XamFormatTimeString),
    ANY360_SYS_ENTRY(XamBuildResourceLocator),
    ANY360_SYS_ENTRY(XamBuildGamercardResourceLocator),
    ANY360_SYS_ENTRY(XamBuildSharedSystemResourceLocator),
    ANY360_SYS_ENTRY(XamBuildLegacySystemResourceLocator),
    ANY360_SYS_ENTRY(XamBuildXamResourceLocator),
    ANY360_SYS_ENTRY(ExInitializeReadWriteLock),
    ANY360_SYS_ENTRY(ExAcquireReadWriteLockExclusive),
    ANY360_SYS_ENTRY(ExTryToAcquireReadWriteLockExclusive),
    ANY360_SYS_ENTRY(ExAcquireReadWriteLockShared),
    ANY360_SYS_ENTRY(ExTryToAcquireReadWriteLockShared),
    ANY360_SYS_ENTRY(ExReleaseReadWriteLock),
    ANY360_SYS_ENTRY(InterlockedPushEntrySList),
    ANY360_SYS_ENTRY(InterlockedPopEntrySList),
    ANY360_SYS_ENTRY(InterlockedFlushSList),
    ANY360_SYS_ENTRY(CreateFileA),
    ANY360_SYS_ENTRY(ReadFile),
    ANY360_SYS_ENTRY(WriteFile),
    ANY360_SYS_ENTRY(SetFilePointer),
    ANY360_SYS_ENTRY(SetFilePointerEx),
    ANY360_SYS_ENTRY(GetFileSize),
    ANY360_SYS_ENTRY(GetFileSizeEx),
    ANY360_SYS_ENTRY(SetEndOfFile),
    ANY360_SYS_ENTRY(FlushFileBuffers),
    ANY360_SYS_ENTRY(DeleteFileA),
    ANY360_SYS_ENTRY(CreateDirectoryA),
    ANY360_SYS_ENTRY(MoveFileA),
    ANY360_SYS_ENTRY(CopyFileA),
    ANY360_SYS_ENTRY(GetFileAttributesA),
    ANY360_SYS_ENTRY(GetFileAttributesExA),
    ANY360_SYS_ENTRY(GetDiskFreeSpaceExA),
    ANY360_SYS_ENTRY(FindFirstFileA),
    ANY360_SYS_ENTRY(FindNextFileA),
    ANY360_SYS_ENTRY(CloseHandle),
    ANY360_SYS_ENTRY(SetFileAttributesA),
    ANY360_SYS_ENTRY(SetFileTime),
    ANY360_SYS_ENTRY(CompareFileTime),
    ANY360_SYS_ENTRY(XeCryptShaInit),
    ANY360_SYS_ENTRY(XeCryptShaUpdate),
    ANY360_SYS_ENTRY(XeCryptShaFinal),
    ANY360_SYS_ENTRY(XeCryptSha256Init),
    ANY360_SYS_ENTRY(XeCryptSha256Update),
    ANY360_SYS_ENTRY(XeCryptSha256Final),
    ANY360_SYS_ENTRY(XeCryptHmacSha),
    ANY360_SYS_ENTRY(XeCryptRc4Key),
    ANY360_SYS_ENTRY(XeCryptRc4Ecb),
    ANY360_SYS_ENTRY(XeCryptRc4),
    ANY360_SYS_ENTRY(XeCryptAesKey),
    ANY360_SYS_ENTRY(XeCryptAesEcb),
    ANY360_SYS_ENTRY(XeCryptAesCbc),
    ANY360_SYS_ENTRY(XeCryptDes3Key),
    ANY360_SYS_ENTRY(XeCryptDes3Ecb),
    ANY360_SYS_ENTRY(XeCryptDes3Cbc),
    ANY360_SYS_ENTRY(XeCryptDesParity),
    ANY360_SYS_ENTRY(XeCryptBnQw_SwapDwQwLeBe),
    ANY360_SYS_ENTRY(XeCryptRandom),
    ANY360_SYS_ENTRY(XeCryptBnDwLePkcs1Verify),
    ANY360_SYS_ENTRY(XeKeysHmacSha),
    ANY360_SYS_ENTRY(XeKeysObscureKey),
    ANY360_SYS_ENTRY(XeKeysAesCbcUsingKey),
    ANY360_SYS_ENTRY(XeKeysHmacShaUsingKey),
    ANY360_SYS_ENTRY(XeCryptRotSumSha),
    ANY360_SYS_ENTRY(XeCryptBnQwBeSigVerify),
    ANY360_SYS_ENTRY(XeKeysGetKey),
    ANY360_SYS_ENTRY(XamLoaderSetLaunchData),
    ANY360_SYS_ENTRY(XamLoaderGetLaunchDataSize),
    ANY360_SYS_ENTRY(XamLoaderGetLaunchData),
    ANY360_SYS_ENTRY(XamContentGetDeviceData),
    ANY360_SYS_ENTRY(XamContentGetCreator),
    ANY360_SYS_ENTRY(XamShowDirtyDiscErrorUI),
    ANY360_SYS_ENTRY(FscSetCacheElementCount),
    ANY360_SYS_ENTRY(FscGetCacheElementCount),
    ANY360_SYS_ENTRY(RtlFreeAnsiString),
    ANY360_SYS_ENTRY(RtlUnicodeStringToAnsiString),
    ANY360_SYS_ENTRY(RtlUpcaseUnicodeChar),
    ANY360_SYS_ENTRY(NtReadFileScatter),
    ANY360_SYS_ENTRY(XamEnableInactivityProcessing),
    ANY360_SYS_ENTRY(XamResetInactivity),
    ANY360_SYS_ENTRY(IoCreateDevice),
    ANY360_SYS_ENTRY(IoDeleteDevice),
    ANY360_SYS_ENTRY(IoCheckShareAccess),
    ANY360_SYS_ENTRY(IoSetShareAccess),
    ANY360_SYS_ENTRY(IoRemoveShareAccess),
    ANY360_SYS_ENTRY(IoCompleteRequest),
    ANY360_SYS_ENTRY(IoInvalidDeviceRequest),
    ANY360_SYS_ENTRY(ObIsTitleObject),
    ANY360_SYS_ENTRY(ObReferenceObject),
    // Batch 12: graphics and audio.
    ANY360_SYS_ENTRY(VdInitializeEngines),
    ANY360_SYS_ENTRY(VdInitializeRingBuffer),
    ANY360_SYS_ENTRY(VdEnableRingBufferRPtrWriteBack),
    ANY360_SYS_ENTRY(VdSetGraphicsInterruptCallback),
    ANY360_SYS_ENTRY(VdPersistDisplay),
    ANY360_SYS_ENTRY(VdSwap),
    ANY360_SYS_ENTRY(XAudioRegisterRenderDriverClient),
    ANY360_SYS_ENTRY(XAudioUnregisterRenderDriverClient),
    ANY360_SYS_ENTRY(XAudioSubmitRenderDriverFrame),
    ANY360_SYS_ENTRY(XAudioGetVoiceCategoryVolumeChangeMask),
    ANY360_SYS_ENTRY(XAudioGetVoiceCategoryVolume),
    ANY360_SYS_ENTRY(XMACreateContext),
    ANY360_SYS_ENTRY(XMAReleaseContext),
    ANY360_SYS_ENTRY(XMAInitializeContext),
    ANY360_SYS_ENTRY(XMASetLoopData),
    ANY360_SYS_ENTRY(XMASetInputBuffer0),
    ANY360_SYS_ENTRY(XMASetInputBuffer1),
    ANY360_SYS_ENTRY(XMAEnableContext),
    ANY360_SYS_ENTRY(XMADisableContext),
    ANY360_SYS_ENTRY(XMABlockWhileInUse),
    ANY360_SYS_ENTRY(XMAIsInputBuffer0Valid),
    ANY360_SYS_ENTRY(XMAIsInputBuffer1Valid),
    ANY360_SYS_ENTRY(XMAIsOutputBufferValid),
    ANY360_SYS_ENTRY(XMAGetOutputBufferReadOffset),
    ANY360_SYS_ENTRY(XMAGetOutputBufferWriteOffset),
    ANY360_SYS_ENTRY(XMAGetInputBufferReadOffset),
    ANY360_SYS_ENTRY(XMAGetPacketMetadata),
    ANY360_SYS_ENTRY(XMASetInputBuffer0Valid),
    ANY360_SYS_ENTRY(XMASetInputBuffer1Valid),
    ANY360_SYS_ENTRY(XMASetOutputBufferValid),
    ANY360_SYS_ENTRY(XMASetOutputBufferReadOffset),
    ANY360_SYS_ENTRY(XMASetInputBufferReadOffset),
    ANY360_SYS_ENTRY(VdQuerySystemCommandBuffer),
    ANY360_SYS_ENTRY(VdSetSystemCommandBuffer),
    ANY360_SYS_ENTRY(XAudioQueryDriverPerformance),
};
const unsigned kExportCount = sizeof(kExports) / sizeof(kExports[0]);

}  // namespace any360::sys
