// PowerPC ABI adapters: one `any360_<Export>(PPCContext&, uint8_t*)` per system call the layer provides, with the same
// calling convention as the kernel import thunks of a static recompilation (arguments in r3..r10, result in r3), so a
// title binds them in place of the reference runtime's `__imp__<Export>`. kExports lists them by name for the binder and
// for tools/build_db.py, which counts every name here as native.
#include <rex/ppc.h>

#include <vector>

#include <cstring>

#include "any360/sys/audio.h"
#include "any360/sys/variables.h"
#include "any360/sys/config.h"
#include "any360/sys/crypt.h"
#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/format.h"
#include "any360/sys/graphics.h"
#include "any360/sys/info.h"
#include "any360/sys/ldi.h"
#include "any360/sys/memory.h"
#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/rtl.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/threading.h"
#include "any360/sys/video.h"
#include "any360/sys/win32.h"
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
ANY360_SYS_EXPORT(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = Net().XNetXnAddrToInAddr({base}, ctx.r4.u32, ctx.r6.u32); }  // (caller, XNADDR, XNKID, IN_ADDR out)
ANY360_SYS_EXPORT(NetDll_XNetInAddrToXnAddr) { ctx.r3.u64 = Net().XNetInAddrToXnAddr({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }  // (caller, IN_ADDR, XNADDR out, XNKID out)
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
// The file's completion port, if it has one, gets a packet too (file handle in r3).
// A transfer answered STATUS_PENDING on an asynchronous file has completed all the same: its APC and packet carry the
// status its I/O status block holds.
static uint32_t IoApc(PPCContext& ctx, uint8_t* base, uint32_t status) {
  const uint32_t completed = status == 0x103 && ctx.r7.u32 ? Guest{base}.Load32(ctx.r7.u32) : status;
  Threads().QueueIoApc(ThreadOf(ctx, base), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, completed);
  Files().Completed({base}, ctx.r3.u32, ctx.r6.u32, completed, ctx.r7.u32);
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
// Step 12, the notification calls Xenia stubs too.
// (notification, data): sent to every listener whose areas take it.
ANY360_SYS_EXPORT(XNotifyBroadcast) {
  XamState().Notify(ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = 0;
}
// (type, user, areas, text, context): a toast the guide would show; there is no guide to show it on: success.
ANY360_SYS_EXPORT(XNotifyQueueUI) { ctx.r3.u64 = 0; }
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
// Step 12, the input calls Xenia stubs too: the non-controller devices' raw reports (a portal of power, devices 5
// and 6). As Xenia: the device and the buffer checked (0x20 bytes at most), then no such device plugged in.
static uint32_t NonControllerRaw(Guest g, uint32_t device, uint32_t buffer, uint32_t length_ptr, uint32_t state_ptr) {
  if (device != 5 && device != 6) return 0x57;  // ERROR_INVALID_PARAMETER
  if (!state_ptr || !length_ptr || !buffer) return 0x57;
  const uint32_t length = g.Load32(length_ptr);
  if (!length || length > 0x20) return 0x57;
  return 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
}
static uint32_t NonControllerSetRaw(uint32_t device, uint32_t buffer, uint32_t length) {
  if (device != 5 && device != 6) return 0x57;
  if (!buffer || !length || length > 0x20) return 0x57;
  return 0x48F;
}
// (device, buffer, length in/out, state out)
ANY360_SYS_EXPORT(XamInputNonControllerGetRawEx) {
  ctx.r3.u64 = NonControllerRaw({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
// (state out, length in/out, buffer): device 5.
ANY360_SYS_EXPORT(XamInputNonControllerGetRaw) {
  ctx.r3.u64 = NonControllerRaw({base}, 5, ctx.r5.u32, ctx.r4.u32, ctx.r3.u32);
}
// (device, buffer, length)
ANY360_SYS_EXPORT(XamInputNonControllerSetRawEx) { ctx.r3.u64 = NonControllerSetRaw(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
// (length, buffer): device 5.
ANY360_SYS_EXPORT(XamInputNonControllerSetRaw) { ctx.r3.u64 = NonControllerSetRaw(5, ctx.r4.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamInputGetState) { ctx.r3.u64 = XamState().InputGetState({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamInputSetState) { ctx.r3.u64 = XamState().InputSetState({base}, ctx.r3.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamInputGetKeystrokeEx) {
  // The user index in and out through r3: the keystroke's user (XINPUT_KEYSTROKE + 6) written back, as the reference.
  const uint32_t user_ptr = ctx.r3.u32, user = user_ptr ? Guest{base}.Load32(user_ptr) : 0;
  ctx.r3.u64 = XamState().InputGetKeystroke({base}, user, ctx.r4.u32, ctx.r5.u32);
  if (ctx.r3.u32 == 0 && user_ptr && ctx.r5.u32) Guest{base}.Store32(user_ptr, Guest{base}.Load8(ctx.r5.u32 + 6));
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
ANY360_SYS_EXPORT(NetDll_XNetGetTitleXnAddr) { ctx.r3.u64 = Net().XNetGetTitleXnAddr({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = XamState().WsaGetLastError(); }
// The sockets (caller first, then the Winsock arguments): any360::sys's network (net.h).
ANY360_SYS_EXPORT(NetDll_socket) { ctx.r3.u64 = Net().Socket(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_closesocket) { ctx.r3.u64 = Net().CloseSocket(ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_shutdown) { ctx.r3.u64 = Net().Shutdown(ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_setsockopt) { ctx.r3.u64 = Net().SetSockOpt({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(NetDll_getsockopt) { ctx.r3.u64 = Net().GetSockOpt({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
ANY360_SYS_EXPORT(NetDll_bind) { ctx.r3.u64 = Net().Bind({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_connect) { ctx.r3.u64 = Net().Connect({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_listen) { ctx.r3.u64 = Net().Listen(ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_accept) { ctx.r3.u64 = Net().Accept({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_select) { ctx.r3.u64 = Net().Select({base}, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }  // (caller, nfds, ...)
ANY360_SYS_EXPORT(NetDll_recv) { ctx.r3.u64 = Net().Recv({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(NetDll_recvfrom) {
  ctx.r3.u64 = Net().RecvFrom({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(NetDll_send) { ctx.r3.u64 = Net().Send({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
ANY360_SYS_EXPORT(NetDll_sendto) {
  ctx.r3.u64 = Net().SendTo({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(NetDll_ioctlsocket) { ctx.r3.u64 = Net().IoctlSocket({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }

// Batch 13: the rest of the offline network Crackdown, The Simpsons Game and TDU import (sys_object_tests).
ANY360_SYS_EXPORT(NetDll_getsockname) { ctx.r3.u64 = Net().Name({base}, ctx.r4.u32, false, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(NetDll_getpeername) { ctx.r3.u64 = Net().Name({base}, ctx.r4.u32, true, ctx.r5.u32, ctx.r6.u32); }
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
ANY360_SYS_EXPORT(NetDll_XNetConnect) { ctx.r3.u64 = Net().XNetConnect(); }
ANY360_SYS_EXPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = Net().XNetGetConnectStatus(); }
ANY360_SYS_EXPORT(NetDll_XNetQosListen) { ctx.r3.u64 = 0x65B; }  // ERROR_FUNCTION_FAILED, as the reference
// (caller, address count, XNADDRs, XNKIDs, XNKEYs, IN_ADDR count, IN_ADDRs, service ids, probes, bits a second, flags,
// event, XNQOS out)
ANY360_SYS_EXPORT(NetDll_XNetQosLookup) {
  ctx.r3.u64 = Net().XNetQosLookup({base}, ctx.r4.u32, ctx.r8.u32, StackArgument(ctx, base, 11), StackArgument(ctx, base, 12));
}
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
// Step 12, the last calls Xenia stubs too.
ANY360_SYS_EXPORT(XamVoiceGetMicArrayStatus) { ctx.r3.u64 = 0; }  // no Kinect microphone array
// (user): an automation controller unbound for users 0-3 (1), none past them (0).
ANY360_SYS_EXPORT(XAutomationpUnbindController) { ctx.r3.u64 = ctx.r3.u32 < 4 ? 1 : 0; }
// (X_MIC_DEVICE: request type 0x00, user 0x02, state 0x04, completion routine 0x10, capabilities from 0x18): no
// microphone, as Xenia without one. STATUS_INVALID_PARAMETER without a device or for a user past 4,
// STATUS_INVALID_DEVICE_REQUEST for a request past 0x0B; the state set to not connected; a status request succeeds;
// the others (capabilities: features 0x100, format 1, colour 0) call the completion routine, when given, with the
// device and STATUS_UNSUCCESSFUL on the calling thread and answer STATUS_PENDING.
ANY360_SYS_EXPORT(MicDeviceRequest) {
  Guest g{base};
  const uint32_t device = ctx.r3.u32;
  if (!device || g.Load16(device + 2) > 4) {
    ctx.r3.u64 = 0xC000000D;
    return;
  }
  const uint16_t request = g.Load16(device);
  if (request > 0x0B) {
    ctx.r3.u64 = 0xC0000010;
    return;
  }
  g.Store32(device + 4, 0);  // MicNotConnected
  if (request == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  if (request == 9) {
    g.Store32(device + 0x18, 0x100);
    g.Store16(device + 0x1C, 1);
    g.Store8(device + 0x28, 0);
  }
  if (const uint32_t completion = g.Load32(device + 0x10)) {
    Threads().Dispatch(completion, {ThreadOf(ctx, base), ctx.r13.u32, (ctx.r1.u32 - 0x400) & ~0xFu}, {device, 0xC0000001});
  }
  ctx.r3.u64 = 0x103;  // STATUS_PENDING
}
// (open): the tray opened or closed (XN_SYS_TRAYSTATECHANGED to the listeners). No result.
ANY360_SYS_EXPORT(HalOpenCloseODDTray) { XamState().SmcSetTray(ctx.r3.u32 ? 0 : 2); }
// (message, response): the SMC's answer. No result.
ANY360_SYS_EXPORT(HalSendSMCMessage) { XamState().SmcMessage({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtCancelIoFile) { ctx.r3.u64 = Files().CancelIo(ctx.r3.u32); }
// (message, detail): to the debug sink; no result.
ANY360_SYS_EXPORT(RtlRip) { RtlRip({base}, ctx.r3.u32, ctx.r4.u32); }
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
// Step 12, the guide screens Xenia stubs too.
ANY360_SYS_EXPORT(XamShowMarketplaceUIEx) { ctx.r3.u64 = XamState().ShowMarketplaceUIEx(ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamShowSigninUIEx) { ctx.r3.u64 = XamState().ShowSigninUIEx({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamShowGoldUpgradeUI) { ctx.r3.u64 = kErrorFunctionFailed; }  // no Gold membership to buy offline
// The Kinect forms of the guide screens: Xenia checks the Kinect HUD first, which is not there without a Kinect:
// ERROR_ACCESS_DENIED, nothing shown.
ANY360_SYS_EXPORT(XamShowNuiDeviceSelectorUI) { ctx.r3.u64 = 5; }
ANY360_SYS_EXPORT(XamShowNuiDirtyDiscErrorUI) { ctx.r3.u64 = 5; }
ANY360_SYS_EXPORT(XamShowNuiGuideUI) { ctx.r3.u64 = 5; }
ANY360_SYS_EXPORT(XamShowNuiMessageBoxUI) { ctx.r3.u64 = 5; }
ANY360_SYS_EXPORT(XamShowNuiSigninUI) { ctx.r3.u64 = 5; }
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
// Step 12, the module calls Xenia stubs too.
// (name, flags, minimum version, handle out): XexLoadImage's answer (Xenia's is XexLoadImage too).
ANY360_SYS_EXPORT(XexLoadExecutable) { ctx.r3.u64 = XamState().LoadImage({base}, ctx.r3.u32, ctx.r6.u32); }
// (media type out, title id out[, a word out]): the title runs from an Xbox 360 game disc (1); its title id (Xenia
// gives 0, having none to give), the third word 0. No result.
static void LoaderGetMediaInfo(uint8_t* base, uint32_t media_type, uint32_t title_id, uint32_t extra) {
  Guest g{base};
  if (media_type) g.Store32(media_type, 1);  // X_DVD_DISC_STATE XBOX_360_GAME_DISC
  if (title_id) g.Store32(title_id, XamState().title().title_id);
  if (extra) g.Store32(extra, 0);
}
ANY360_SYS_EXPORT(XamLoaderGetMediaInfo) { LoaderGetMediaInfo(base, ctx.r3.u32, ctx.r4.u32, 0); }
ANY360_SYS_EXPORT(XamLoaderGetMediaInfoEx) { LoaderGetMediaInfo(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
// The system's versions as build numbers: the first retail kernel's (1888, as Xenia), and as current the build the
// layer reports in XboxKrnlVersion (0xFFFF, newer than any title asks for).
ANY360_SYS_EXPORT(XamUpdateGetBaseSystemVersion) { ctx.r3.u64 = 1888; }
ANY360_SYS_EXPORT(XamUpdateGetCurrentSystemVersion) { ctx.r3.u64 = 0xFFFF; }
ANY360_SYS_EXPORT(XexUnloadImage) { ctx.r3.u64 = XamState().UnloadImage(ctx.r3.u32); }
// Another executable from a content package (a demo, an offer): no image is loaded besides the title's.
ANY360_SYS_EXPORT(XamContentLaunchImage) { ctx.r3.u64 = kErrorFileNotFound; }
// The internal forms (an image path or an XCONTENT_DATA_INTERNAL): the same answer.
ANY360_SYS_EXPORT(XamContentLaunchImageFromFileInternal) { ctx.r3.u64 = kErrorFileNotFound; }
ANY360_SYS_EXPORT(XamContentLaunchImageInternal) { ctx.r3.u64 = kErrorFileNotFound; }
ANY360_SYS_EXPORT(XamContentLaunchImageInternalEx) { ctx.r3.u64 = kErrorFileNotFound; }
// Step 12, the content calls Xenia stubs too.
ANY360_SYS_EXPORT(XamContentGetAttributesInternal) {
  ctx.r3.u64 = XamState().ContentGetAttributesInternal({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(XamContentGetDeviceVolumePath) {
  ctx.r3.u64 = XamState().ContentGetDeviceVolumePath({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(XamContentGetLocalizedDeviceData) {
  ctx.r3.u64 = XamState().ContentGetLocalizedDeviceData({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(XamContentGetLocalizedString) {
  ctx.r3.u64 = XamState().ContentGetLocalizedString({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamContentResolveInternal) {
  ctx.r3.u64 = XamState().ContentResolveInternal({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}
// (tile type, title id, item id, user, buffer, size, overlapped)
ANY360_SYS_EXPORT(XamReadTile) { ctx.r3.u64 = XamState().ReadTile({base}, ctx.r6.u32, ctx.r5.u64, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32); }
// (tile type, title id, item id, user, two unknown words, buffer, size, overlapped: the ninth argument, on the stack)
ANY360_SYS_EXPORT(XamReadTileEx) {
  ctx.r3.u64 = XamState().ReadTile({base}, ctx.r6.u32, ctx.r5.u64, ctx.r9.u32, ctx.r10.u32, StackArgument(ctx, base, 8));
}

// Batch 16: the rest of Test Drive Unlimited's (sys_object_tests).
ANY360_SYS_EXPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
// Step 12, the network calls Xenia stubs too. XHTTP: started (1, last error 0), but no session opens (0, last error
// ERROR_NOT_SUPPORTED), as Xenia.
ANY360_SYS_EXPORT(NetDll_XHttpStartup) {
  SetWin32LastError({base}, ThreadOf(ctx, base), 0);
  ctx.r3.u64 = 1;
}
ANY360_SYS_EXPORT(NetDll_XHttpOpen) {
  SetWin32LastError({base}, ThreadOf(ctx, base), 50);  // ERROR_NOT_SUPPORTED
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XamNetworkStorageHasUserEnabledStorage) {
  ctx.r3.u64 = XamState().NetworkStorageHasUserEnabledStorage(ctx.r3.u32);
}
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
// Step 12: what Xenia Canary implements and the reference stubs (docs/SYS_STUBS.md), with scenario tests against the
// hashes' and ciphers' standard test vectors (sys_object_tests).
ANY360_SYS_EXPORT(XeCryptMd5) {
  XeCryptMd5({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XeCryptSha256) {
  XeCryptSha256({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XeCryptMd5Init) { XeCryptMd5Init({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XeCryptMd5Update) { XeCryptMd5Update({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptMd5Final) { XeCryptMd5Final({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptHmacMd5) {
  XeCryptHmacMd5({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                 StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
}
ANY360_SYS_EXPORT(XeCryptHmacShaInit) { XeCryptHmacShaInit({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptHmacShaUpdate) { XeCryptHmacShaUpdate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptHmacShaFinal) { XeCryptHmacShaFinal({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptHmacShaVerify) {
  ctx.r3.u64 = XeCryptHmacShaVerify({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
}
ANY360_SYS_EXPORT(XeCryptDesKey) { XeCryptDesKey({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeCryptDesEcb) { XeCryptDesEcb({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XeCryptDesCbc) { XeCryptDesCbc({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }

// Step 12: xam.xex's Win32 forms (win32.h), each over a kernel call any360::sys already answers.
static uint32_t Win32Delay(PPCContext& ctx, uint8_t* base, uint32_t milliseconds, uint32_t alertable) {
  // INFINITE: an absolute time no clock reaches, as Xenia passes it.
  const int64_t interval = Win32Timeout(milliseconds).value_or(INT64_MAX);
  return AlertableWait(ctx, base, alertable, [&](bool a) { return Threads().Delay({base}, ctx.r13.u32, interval, a); });
}
ANY360_SYS_EXPORT(Sleep) { Win32Delay(ctx, base, ctx.r3.u32, 0); }
ANY360_SYS_EXPORT(SleepEx) { ctx.r3.u64 = Win32SleepResult(Win32Delay(ctx, base, ctx.r3.u32, ctx.r4.u32)); }
static uint32_t Win32Wait(PPCContext& ctx, uint8_t* base, uint32_t handle, uint32_t milliseconds, uint32_t alertable) {
  const std::optional<int64_t> timeout = Win32Timeout(milliseconds);
  const uint32_t status = AlertableWait(ctx, base, alertable, [&](bool a) {
    return Objects().WaitForHandle({base}, handle, ThreadOf(ctx, base), timeout ? &*timeout : nullptr, a);
  });
  return Win32WaitResult({base}, ThreadOf(ctx, base), status);
}
ANY360_SYS_EXPORT(WaitForSingleObject) { ctx.r3.u64 = Win32Wait(ctx, base, ctx.r3.u32, ctx.r4.u32, 0); }
ANY360_SYS_EXPORT(WaitForSingleObjectEx) { ctx.r3.u64 = Win32Wait(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(CreateThread) {
  // (attributes, stack size, start, parameter, flags, thread id out): CREATE_SUSPENDED (4) is the kernel's suspended
  // flag (1); no processor asked for.
  uint32_t handle = 0;
  const uint32_t status = Threads().CreateThreadHandle({base}, &handle, ctx.r4.u32, ctx.r8.u32, 0, ctx.r5.u32, ctx.r6.u32,
                                                       XapiThreadFlags(ctx.r7.u32, 0xFFFFFFFF));
  if (status != kStatusSuccess) SetWin32LastNtError({base}, ThreadOf(ctx, base), status);
  ctx.r3.u64 = status == kStatusSuccess ? handle : 0;
}
ANY360_SYS_EXPORT(ResumeThread) {
  // The previous suspend count, or -1 with the last error set.
  const uint32_t kthread = Objects().Lookup(ctx.r3.u32);
  uint32_t count = 0;
  const uint32_t status = kthread ? Threads().Resume({base}, kthread, &count) : kStatusInvalidHandle;
  if (status != kStatusSuccess) SetWin32LastNtError({base}, ThreadOf(ctx, base), status);
  ctx.r3.u64 = status == kStatusSuccess ? count : 0xFFFFFFFFu;
}
ANY360_SYS_EXPORT(ExitThread) { Threads().TerminateThread({base}, ctx.r13.u32, ctx.r3.u32); }
ANY360_SYS_EXPORT(GetCurrentThreadId) { ctx.r3.u64 = Guest{base}.Load32(ThreadOf(ctx, base) + kKThreadId); }
ANY360_SYS_EXPORT(GetLastError) { ctx.r3.u64 = Win32LastError({base}, ThreadOf(ctx, base)); }
ANY360_SYS_EXPORT(GetModuleHandleA) {
  const uint32_t handle = XamState().ModuleHandle({base}, ctx.r3.u32);
  if (!handle) SetWin32LastNtError({base}, ThreadOf(ctx, base), 0xC0000225u);  // STATUS_NOT_FOUND
  ctx.r3.u64 = handle;
}
ANY360_SYS_EXPORT(GetTickCount) { ctx.r3.u64 = TickCount(); }
ANY360_SYS_EXPORT(QueryPerformanceFrequency) {
  if (ctx.r3.u32) Guest{base}.Store64(ctx.r3.u32, KeQueryPerformanceFrequency());
  ctx.r3.u64 = 1;
}
ANY360_SYS_EXPORT(GetSystemTimeAsFileTime) {
  // The system time as one 64-bit value, as KeQuerySystemTime writes it (and Xenia).
  if (ctx.r3.u32) Guest{base}.Store64(ctx.r3.u32, SystemTime());
}
ANY360_SYS_EXPORT(lstrlenW) { ctx.r3.u64 = LstrlenW({base}, ctx.r3.u32); }

// Step 12: xam.xex's information calls Xenia implements (info.h, xam.h, win32.h).
ANY360_SYS_EXPORT(XGetAudioFlags) { ctx.r3.u64 = XGetAudioFlags(); }
ANY360_SYS_EXPORT(XGetOverlappedExtendedError) {
  ctx.r3.u64 = XGetOverlappedExtendedError({base}, ThreadOf(ctx, base), ctx.r3.u32);
}
ANY360_SYS_EXPORT(XamGetActiveDashAppInfo) { XamState().GetActiveDashAppInfo({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamSetActiveDashAppInfo) { XamState().SetActiveDashAppInfo({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetCachedTitleName) {
  ctx.r3.u64 = XamState().GetCachedTitleName({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(XamGetDashBackstackData) { ctx.r3.u64 = XamState().GetDashBackstackData(); }
ANY360_SYS_EXPORT(XamGetDashContext) { ctx.r3.u64 = XamState().DashContext(); }
ANY360_SYS_EXPORT(XamGetLanguage) { ctx.r3.u64 = XamGetLanguage(); }
ANY360_SYS_EXPORT(XamGetLanguageLocaleFallbackString) {
  ctx.r3.u64 = XamState().LanguageLocaleFallbackString({base}, ctx.r3.u32);
}
ANY360_SYS_EXPORT(XamGetLanguageTypeface) { ctx.r3.u64 = XamGetLanguageTypeface({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamGetOnlineCountryFeatures) { ctx.r3.u64 = XamGetOnlineCountryFeatures(ctx.r3.u32); }
// (xuid, language out, country out)
ANY360_SYS_EXPORT(XamGetOnlineLanguageAndCountry) { XamGetOnlineLanguageAndCountry({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamIsChildAccountSignedIn) { ctx.r3.u64 = 0; }  // the local profile has no parental controls
ANY360_SYS_EXPORT(XamIsCurrentTitleDash) { ctx.r3.u64 = XamState().title().title_id == kDashboardTitleId; }
ANY360_SYS_EXPORT(XamIsIptvEnabled) { ctx.r3.u64 = 0x80004005; }  // E_FAIL: no IPTV (the console's retail flag clear)
ANY360_SYS_EXPORT(XamIsNuiUIActive) { ctx.r3.u64 = 0; }  // the system UI opens and closes at once, as XamIsUIActive
ANY360_SYS_EXPORT(XamIsSystemExperienceTitleId) { ctx.r3.u64 = XamIsSystemExperienceTitleId(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamIsSystemTitleId) { ctx.r3.u64 = XamIsSystemTitleId(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamIsXbox1TitleId) { ctx.r3.u64 = XamIsXbox1TitleId(ctx.r3.u32); }

// Step 12: the kernel and XAM calls Xenia implements (rtl.h, scheduler.h, variables.h, info.h, win32.h).
ANY360_SYS_EXPORT(RtlGetLastError) { ctx.r3.u64 = Win32LastError({base}, ThreadOf(ctx, base)); }
ANY360_SYS_EXPORT(RtlSetLastNTError) { ctx.r3.u64 = SetWin32LastNtError({base}, ThreadOf(ctx, base), ctx.r3.u32); }
// (milliseconds, alertable): SleepEx's delay as an NTSTATUS (STATUS_SUCCESS, or STATUS_USER_APC when an APC ran).
ANY360_SYS_EXPORT(RtlSleep) { ctx.r3.u64 = Win32SleepResult(Win32Delay(ctx, base, ctx.r3.u32, ctx.r4.u32)); }
ANY360_SYS_EXPORT(RtlRandom) { ctx.r3.u64 = RtlRandom({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlDowncaseUnicodeChar) { ctx.r3.u64 = RtlDowncaseUnicodeChar(ctx.r3.u32); }
// (end out, base out), the PCR in r13.
ANY360_SYS_EXPORT(RtlGetStackLimits) { RtlGetStackLimits({base}, ctx.r13.u32, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlImageDirectoryEntryToData) {
  ctx.r3.u64 = RtlImageDirectoryEntryToData({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(KeQueryInterruptTime) { ctx.r3.u64 = InterruptTime(); }
ANY360_SYS_EXPORT(KeSetPriorityThread) { ctx.r3.u64 = Threads().SetPriority({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(HalGetCurrentAVPack) { ctx.r3.u64 = XGetAVPack(); }
ANY360_SYS_EXPORT(XamGetLocaleDateFormat) { ctx.r3.u64 = XamGetLocaleDateFormat(ctx.r3.u32); }

// Step 12: the guide screens and Kinect calls Xenia implements (xam.h). No Kinect is attached.
ANY360_SYS_EXPORT(XamNuiHudGetEngagedTrackingID) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamNuiHudGetInitializeFlags) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamNuiHudGetVersions) { XamState().NuiHudGetVersions({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamNuiHudIsEnabled) { ctx.r3.u64 = 0; }
// Step 12, the Kinect (NUI) calls Xenia stubs too: no Kinect, as Xenia answers without one (no tilt controller, the
// HUD not enabled).
ANY360_SYS_EXPORT(XamNuiCameraGetTiltControllerType) { ctx.r3.u64 = 0; }  // no tilt controller
ANY360_SYS_EXPORT(XamNuiCameraTiltGetStatus) { ctx.r3.u64 = 0x80004005; }  // E_FAIL: no motor to report
// (elevation angle out, moving out): the tilt status fails, so E_FAIL and nothing written.
ANY360_SYS_EXPORT(XamNuiCameraElevationGetAngle) { ctx.r3.u64 = 0x80004005; }
ANY360_SYS_EXPORT(XamNuiCameraSetFlags) { ctx.r3.u64 = 0x8007048F; }  // E_DEVICE_NOT_CONNECTED: no tilt controller
ANY360_SYS_EXPORT(XamNuiCameraTiltSetCallback) {
  XamState().NuiCameraTiltSetCallback(ctx.r3.u32);
  ctx.r3.u64 = 0;
}
// (X_NUI_DEPTH_CALIBRATION out, 0x30 bytes): cleared, then STATUS_NO_SUCH_FILE with the NT facility bit: none stored.
ANY360_SYS_EXPORT(XamNuiGetDepthCalibration) {
  if (ctx.r3.u32) std::memset(base + ctx.r3.u32, 0, 0x30);
  ctx.r3.u64 = 0xD000000F;
}
ANY360_SYS_EXPORT(XamNuiIdentityAbort) { ctx.r3.u64 = 0x80004005; }  // E_FAIL: the HUD is not enabled
ANY360_SYS_EXPORT(XamNuiIdentityEnrollForSignIn) { ctx.r3.u64 = 0x80004005; }
// (two words, X_NUI_PLAYER_ENGAGEMENT_UPDATE out, 0x1C bytes): cleared; nobody is engaged. No result.
ANY360_SYS_EXPORT(XamNuiPlayerEngagementUpdate) {
  if (ctx.r5.u32) std::memset(base + ctx.r5.u32, 0, 0x1C);
}
ANY360_SYS_EXPORT(XamNuiSkeletonGetBestSkeletonIndex) { ctx.r3.u64 = ~0ull; }  // no skeleton tracked (a 64-bit -1)
ANY360_SYS_EXPORT(XamNuiHudSetEngagedTrackingID) { ctx.r3.u64 = XamState().NuiHudSetEngagedTrackingId(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamNuiIdentityGetSessionId) { ctx.r3.u64 = XamState().NuiIdentitySessionId(); }
ANY360_SYS_EXPORT(XamNuiIsChatMicEnabled) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamNuiIsDeviceReady) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamSetDashContext) { ctx.r3.u64 = XamState().SetDashContext(ctx.r3.u32); }
// (user, flags[, an unknown string])
ANY360_SYS_EXPORT(XamShowCreateProfileUI) { ctx.r3.u64 = XamState().ShowCreateProfileUI(); }
ANY360_SYS_EXPORT(XamShowCreateProfileUIEx) { ctx.r3.u64 = XamState().ShowCreateProfileUI(); }
ANY360_SYS_EXPORT(XamShowEditProfileUI) { ctx.r3.u64 = XamState().ShowProfileUI(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamShowForcedNameChangeUI) { ctx.r3.u64 = XamState().ShowForcedNameChangeUI(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamShowGamerCardUI) { ctx.r3.u64 = XamState().ShowProfileUI(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamShowMultiplayerUpgradeUI) { ctx.r3.u64 = XamState().ShowMultiplayerUpgradeUI(); }
ANY360_SYS_EXPORT(XamShowNuiHardwareRequiredUI) { ctx.r3.u64 = XamState().ShowNuiHardwareRequiredUI(ctx.r3.u32); }

// Step 12: the profile and user calls Xenia implements (xam.h). One local profile, user 0.
// (device, handle out)
ANY360_SYS_EXPORT(XamProfileCreateEnumerator) { ctx.r3.u64 = XamState().ProfileCreateEnumerator({base}, ctx.r4.u32); }
// (handle, flags, buffer, overlapped)
ANY360_SYS_EXPORT(XamProfileEnumerate) { ctx.r3.u64 = XamState().ProfileEnumerate({base}, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32); }
// (offline XUID, account out, device out)
ANY360_SYS_EXPORT(XamProfileFindAccount) {
  ctx.r3.u64 = XamState().ProfileFindAccount({base}, ctx.r3.u64, ctx.r4.u32, ctx.r5.u32);
}
// (XUID, buffer length, buffer): the profile's language and country as "ll-CC", which are the console's here.
ANY360_SYS_EXPORT(XamProfileGetLiveLegalLocale) {
  ctx.r3.u64 = XamGetOnlineLanguageAndCountryString({base}, Console().language, Console().country, ctx.r4.u32, ctx.r5.u32);
}
// (XUID, mount name, flags, content data out)
ANY360_SYS_EXPORT(XamProfileOpen) { ctx.r3.u64 = XamState().ProfileOpen({base}, ctx.r3.u64, ctx.r4.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamUserGetCachedUserFlags) { ctx.r3.u64 = XamState().UserFlags(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamUserGetUserFlags) { ctx.r3.u64 = XamState().UserFlags(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamUserGetUserFlagsFromXUID) { ctx.r3.u64 = XamState().UserFlagsFromXuid(ctx.r3.u64); }
// (XUID, flags, index out)
ANY360_SYS_EXPORT(XamUserGetIndexFromXUID) { ctx.r3.u64 = XamState().UserGetIndexFromXuid({base}, ctx.r3.u64, ctx.r5.u32); }
// The profile's language, which is the console's here.
ANY360_SYS_EXPORT(XamUserGetOnlineLanguageFromXUID) { ctx.r3.u64 = Console().language; }
ANY360_SYS_EXPORT(XamUserGetSubscriptionType) {
  ctx.r3.u64 = XamState().UserGetSubscriptionType({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
// (user, tenure level out, milestone out, milestone date out, overlapped)
ANY360_SYS_EXPORT(XamUserGetUserTenure) { ctx.r3.u64 = XamState().UserGetUserTenure(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamUserIsParentalControlled) { ctx.r3.u64 = 0; }  // no parental controls on the local profile
// (XUIDs, flags, overlapped)
ANY360_SYS_EXPORT(XamUserLogon) { ctx.r3.u64 = XamState().UserLogon({base}, ctx.r3.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserNuiGetUserIndexForSignin) { ctx.r3.u64 = XamState().UserNuiGetUserIndexForSignin({base}, ctx.r3.u32); }
// Step 12, the profile and user calls Xenia stubs too.
ANY360_SYS_EXPORT(XamProfileClose) { ctx.r3.u64 = XamState().ProfileClose({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamProfileCreate) { ctx.r3.u64 = XamState().ProfileCreate({base}, ctx.r4.u32, ctx.r10.u32); }
ANY360_SYS_EXPORT(XamProfileGetCreationStatus) {
  ctx.r3.u64 = XamState().ProfileGetCreationStatus({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(XamProfileLoadAccountInfo) { ctx.r3.u64 = XamState().ProfileLoadAccountInfo({base}, ctx.r4.u64, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserCreateTitlesPlayedEnumerator) {
  ctx.r3.u64 = XamState().UserCreateTitlesPlayedEnumerator({base}, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(XamUserIsGuest) { ctx.r3.u64 = 0; }  // no guest: the one local profile
ANY360_SYS_EXPORT(XamUserIsUnsafeProgrammingAllowed) {
  ctx.r3.u64 = XamState().UserIsUnsafeProgrammingAllowed({base}, ctx.r3.u32, ctx.r5.u32);
}
// (profile, flags, overlapped): the local profile stays signed in, as XamUserLogon; success (through the overlapped).
ANY360_SYS_EXPORT(XamUserLogonEx) { ctx.r3.u64 = XamState().Finish({base}, ctx.r5.u32, 0); }
ANY360_SYS_EXPORT(XamUserNuiEnableBiometric) { ctx.r3.u64 = 0x80070057; }  // E_INVALIDARG, as Xenia: no Kinect
ANY360_SYS_EXPORT(XamUserNuiGetUserIndex) { ctx.r3.u64 = 0x80070525; }  // the NO_SUCH_USER HRESULT: no Kinect user
// (index out): E_INVALIDARG without one, else E_FAIL (no Kinect user to bind).
ANY360_SYS_EXPORT(XamUserNuiGetUserIndexForBind) { ctx.r3.u64 = ctx.r3.u32 ? 0x80004005 : 0x80070057; }
// (user, mask, value out / value, overlapped)
ANY360_SYS_EXPORT(XamUserReadUserPreference) {
  ctx.r3.u64 = XamState().UserReadUserPreference({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(XamUserWriteUserPreference) {
  ctx.r3.u64 = XamState().UserWriteUserPreference({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
// Step 12, seventh batch: SHA-512, the BnQw helpers, the signature formats, the console's identity.
ANY360_SYS_EXPORT(XeCryptSha512Init) { XeCryptSha512Init({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XeCryptSha512Update) { XeCryptSha512Update({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptSha512Final) { XeCryptSha512Final({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptBnQw_Copy) { XeCryptBnQw_Copy({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptBnQwNeModInv) { ctx.r3.u64 = XeCryptBnQwNeModInv(ctx.r3.u64); }
ANY360_SYS_EXPORT(XeCryptBnQwNeModMul) {
  XeCryptBnQwNeModMul({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u64, ctx.r7.u32, ctx.r8.u32);
}
ANY360_SYS_EXPORT(XeCryptBnQwBeSigFormat) { XeCryptBnQwBeSigFormat({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XeCryptBnDwLePkcs1Format) { XeCryptBnDwLePkcs1Format({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XeKeysGetConsoleType) { ctx.r3.u64 = XeKeysGetConsoleType({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XeKeysGetConsoleID) { ctx.r3.u64 = XeKeysGetConsoleID({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XeKeysGetKeyProperties) { ctx.r3.u64 = XeKeysGetKeyProperties(ctx.r3.u32); }
// Step 12, eighth batch: the language and locale, XamAllocEx, the Xapi thread and timeout forms, IPTV's name, LDI.
ANY360_SYS_EXPORT(XTLGetLanguageV2) { ctx.r3.u64 = XTLGetLanguageV2(); }
// (buffer length, buffer)
ANY360_SYS_EXPORT(XapipGetLocale) { ctx.r3.u64 = XapipGetLocale({base}, ctx.r3.u32, ctx.r4.u32); }
// (physical flags, flags, size, address out)
ANY360_SYS_EXPORT(XamAllocEx) { ctx.r3.u64 = Memory().XamAllocEx({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
// (interval out, milliseconds): the interval's address, or 0 for INFINITE.
ANY360_SYS_EXPORT(XapiFormatTimeOut) { ctx.r3.u64 = XapiFormatTimeOut({base}, ctx.r3.u32, ctx.r4.u32); }
// (attributes, stack size, start, parameter, flags, processor, thread id out): the handle, or 0 with the last error set.
ANY360_SYS_EXPORT(XapipCreateThread) {
  uint32_t handle = 0;
  const uint32_t status = Threads().CreateThreadHandle({base}, &handle, ctx.r4.u32, ctx.r9.u32, 0, ctx.r5.u32, ctx.r6.u32,
                                                       XapiThreadFlags(ctx.r7.u32, ctx.r8.u32));
  if (status != kStatusSuccess) SetWin32LastNtError({base}, ThreadOf(ctx, base), status);
  ctx.r3.u64 = status == kStatusSuccess ? handle : 0;
}
// (name out)
ANY360_SYS_EXPORT(XamIptvGetServiceName) { ctx.r3.u64 = XamState().IptvServiceName({base}, ctx.r3.u32); }
// The title's LDI allocator and freer run on the calling thread, below its frame (as an APC does).
struct LdiCaller {
  uint32_t kthread, pcr, stack_top;
};
static uint32_t LdiCall(uint32_t function, uint32_t argument, void* user) {
  const auto& caller = *static_cast<const LdiCaller*>(user);
  return uint32_t(Threads().Dispatch(function, {caller.kthread, caller.pcr, caller.stack_top}, {argument}));
}
static LdiCaller LdiCallerOf(PPCContext& ctx, uint8_t* base) {
  return {ThreadOf(ctx, base), ctx.r13.u32, (ctx.r1.u32 - 0x400) & ~0xFu};
}
// (data block max in, config, alloc, free, scratch, source min out, handle out)
ANY360_SYS_EXPORT(LDICreateDecompression) {
  LdiCaller caller = LdiCallerOf(ctx, base);
  ctx.r3.u64 = Ldi().Create({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                            &LdiCall, &caller);
}
// (handle, source, source size, destination, destination size in/out)
ANY360_SYS_EXPORT(LDIDecompress) {
  ctx.r3.u64 = Ldi().Decompress({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(LDIResetDecompression) { ctx.r3.u64 = Ldi().ResetDecompression({base}, ctx.r3.u32); }
// (category, setting, buffer, size)
ANY360_SYS_EXPORT(ExSetXConfigSetting) {
  ctx.r3.u64 = ExSetXConfigSetting({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
// (category, setting, bits kept, bits set)
ANY360_SYS_EXPORT(ExReadModifyWriteXConfigSettingUlong) {
  ctx.r3.u64 = ExReadModifyWriteXConfigSettingUlong(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
// Step 12, the other calls Xenia stubs too.
ANY360_SYS_EXPORT(XamDoesOmniNeedConfiguration) { ctx.r3.u64 = 0; }     // no wireless network to set up
ANY360_SYS_EXPORT(XamFirstRunExperienceShouldRun) { ctx.r3.u64 = 0; }  // the console's first run is done
// (error code, value): the two localization errors Xenia knows are logged (success), any other E_NOTIMPL.
ANY360_SYS_EXPORT(XamLogLocalizationEtx) {
  ctx.r3.u64 = ctx.r3.u32 == 0x80300034 || ctx.r3.u32 == 0x80300035 ? 0 : 0x80004001;
}
ANY360_SYS_EXPORT(XamLookupCommonStringByIndex) { ctx.r3.u64 = 0; }  // no common string table: none found
// (feature name, value out, its size, overlapped): no Live hive offline. Without an overlapped the extended error is
// the answer (X_ONLINE_E_LOGON_NOT_LOGGED_ON, or E_INVALIDARG without a name, a value or its size); with one, it
// completes with success and that extended error, and E_PENDING (0x8000000A) is returned, as Xenia.
ANY360_SYS_EXPORT(XamQueryLiveHiveA) { ctx.r3.u64 = XamState().QueryLiveHiveA({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamXStudioRequest) { ctx.r3.u64 = 0x80004005; }  // E_FAIL: no xstudio.xex
ANY360_SYS_EXPORT(LDIDestroyDecompression) {
  LdiCaller caller = LdiCallerOf(ctx, base);
  ctx.r3.u64 = Ldi().Destroy({base}, ctx.r3.u32, &LdiCall, &caller);
}
// Step 12, ninth batch: the content "Internal" forms, the callbacks a title registers, XamReadString, the Ex startups,
// network storage's privilege, ObLookupAnyThreadByThreadId.
// (XUID, device, type, title id, flags, items a call, buffer size out, handle out)
ANY360_SYS_EXPORT(XamContentCreateEnumeratorInternal) {
  ctx.r3.u64 = XamState().ContentCreateEnumeratorInternal({base}, ctx.r4.u32, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}
// (content data, metadata out, overlapped)
ANY360_SYS_EXPORT(XamContentGetMetaDataInternal) {
  ctx.r3.u64 = XamState().ContentGetMetaDataInternal({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
// (content data, buffer, size): XamContentSetThumbnail with no user and no overlapped.
ANY360_SYS_EXPORT(XamContentSetThumbnailInternal) {
  ctx.r3.u64 = XamState().ContentSetThumbnail({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, 0);
}
ANY360_SYS_EXPORT(XamContentRegisterChangeCallback) { XamState().ContentRegisterChangeCallback(ctx.r3.u32); }
ANY360_SYS_EXPORT(XamLoaderRegisterLaunchRequestCallback) { XamState().LoaderRegisterLaunchRequestCallback(ctx.r3.u32); }
// (title id, string id, user, buffer, size in/out, overlapped)
ANY360_SYS_EXPORT(XamReadString) { ctx.r3.u64 = XamState().ReadString({base}, ctx.r4.u64, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
// (caller, version, WSADATA, version required) and (caller, params, version required): the plain forms.
ANY360_SYS_EXPORT(NetDll_WSAStartupEx) { ctx.r3.u64 = XamState().WsaStartup({base}, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(NetDll_XNetStartupEx) { ctx.r3.u64 = XamState().XNetStartup({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamNetworkStorageUserHasPrivilege) { ctx.r3.u64 = XamState().NetworkStorageUserHasPrivilege(ctx.r3.u32); }
// (thread id, KTHREAD out): ObLookupThreadByThreadId.
ANY360_SYS_EXPORT(ObLookupAnyThreadByThreadId) { ctx.r3.u64 = Threads().LookupThreadById({base}, ctx.r3.u32, ctx.r4.u32); }
// Step 12, tenth batch: ObCreateObject (the type's allocate procedure run on the calling thread, below its frame, as
// LDI's allocator is) and XexLoadImageFromMemory.
static uint32_t CallAllocate(uint32_t function, uint32_t a, uint32_t b, uint32_t c, void* user) {
  const auto& caller = *static_cast<const LdiCaller*>(user);
  return uint32_t(Threads().Dispatch(function, {caller.kthread, caller.pcr, caller.stack_top}, {a, b, c}));
}
// (object type, OBJECT_ATTRIBUTES, object size, object out): the process type is the calling thread's (KTHREAD +0x73).
ANY360_SYS_EXPORT(ObCreateObject) {
  LdiCaller caller = LdiCallerOf(ctx, base);
  const uint32_t process_type = caller.kthread ? Guest{base}.Load8(caller.kthread + 0x73) : 1;
  ctx.r3.u64 = ObCreateObject({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, process_type, &CallAllocate, &caller);
}
// (image, size, name, flags, minimum version, handle out)
ANY360_SYS_EXPORT(XexLoadImageFromMemory) { ctx.r3.u64 = XamState().LoadImageFromMemory({base}, ctx.r5.u32, ctx.r8.u32); }
// Step 12, the calls Xenia stubs too: avatars (the overlapped is the last argument of each).
ANY360_SYS_EXPORT(XamAvatarBeginEnumAssets) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r8.u32); }
ANY360_SYS_EXPORT(XamAvatarEndEnumAssets) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamAvatarEnumAssets) { ctx.r3.u64 = 0x80070012; }  // E_NO_MORE_FILES: nothing to enumerate
ANY360_SYS_EXPORT(XamAvatarGenerateMipMaps) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r7.u32); }
ANY360_SYS_EXPORT(XamAvatarGetAssetBinary) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r7.u32); }
ANY360_SYS_EXPORT(XamAvatarGetAssetIcon) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r7.u32); }
ANY360_SYS_EXPORT(XamAvatarGetAssets) { ctx.r3.u64 = XamState().AvatarGetAssets({base}, ctx.r8.u32); }
ANY360_SYS_EXPORT(XamAvatarGetAssetsResultSize) {
  ctx.r3.u64 = XamState().AvatarGetAssetsResultSize({base}, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(XamAvatarGetInstalledAssetPackageDescription) {
  XamState().AvatarGetInstalledAssetPackageDescription({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(XamAvatarGetInstrumentation) { ctx.r3.u64 = XamState().AvatarGetInstrumentation({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamAvatarGetManifestLocalUser) {
  ctx.r3.u64 = XamState().AvatarGetManifestLocalUser({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
ANY360_SYS_EXPORT(XamAvatarGetManifestsByXuid) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r8.u32); }
ANY360_SYS_EXPORT(XamAvatarGetMetadataRandom) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamAvatarGetMetadataSignedOutProfile) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamAvatarGetMetadataSignedOutProfileCount) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamAvatarLoadAnimation) { ctx.r3.u64 = XamState().AvatarComplete({base}, ctx.r6.u32); }
ANY360_SYS_EXPORT(XamAvatarManifestGetBodyType) { ctx.r3.u64 = 1; }  // male, as Xenia answers every manifest
ANY360_SYS_EXPORT(XamAvatarReinstallAwardedAsset) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamAvatarSetCustomAsset) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamAvatarSetManifest) { ctx.r3.u64 = XamState().AvatarSetManifest({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamAvatarSetMocks) {}
ANY360_SYS_EXPORT(XamAvatarWearNow) { ctx.r3.u64 = XamState().AvatarWearNow({base}, ctx.r5.u32); }
// Step 12, the calls Xenia stubs too: media and the dashboard's services.
ANY360_SYS_EXPORT(EnumerateMediaObjects) { ctx.r3.u64 = 0x80004001; }  // E_NOTIMPL, as Xenia: no media library
ANY360_SYS_EXPORT(EnumerateMediaObjects_) { ctx.r3.u64 = 0x80004001; }
ANY360_SYS_EXPORT(EnumerateMediaObjects_0) { ctx.r3.u64 = 0x80004001; }
ANY360_SYS_EXPORT(EnumerateMediaObjects_1) { ctx.r3.u64 = 0x80004001; }
ANY360_SYS_EXPORT(XamMediaVerificationCreate) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamMediaVerificationClose) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamMediaVerificationInject) { ctx.r3.u64 = 0; }
// (unknown, overlapped, callback): the disc verifies; a callback is queued as a user APC on the calling thread (its
// low bit cleared, no arguments), as Xenia does.
ANY360_SYS_EXPORT(XamMediaVerificationVerify) {
  if (ctx.r5.u32) Threads().QueueApcTo(ThreadOf(ctx, base), ctx.r5.u32 & ~1u, 0, 0, 0);
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XamMediaVerificationFailedBlocks) { ctx.r3.u64 = XamState().MediaVerificationFailedBlocks({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XampWebInstrumentationSetProfileCounts) {}
ANY360_SYS_EXPORT(XampXAuthStartup) { ctx.r3.u64 = XamState().XAuthStartup({base}, ctx.r3.u32); }
// (count out): no service started, so its count is 0.
ANY360_SYS_EXPORT(XampXAuthShutdown) {
  if (ctx.r3.u32) Guest{base}.Store32(ctx.r3.u32, 0);
}
ANY360_SYS_EXPORT(XampXAuthGetTitleBuffer) { ctx.r3.u64 = 0; }  // no title buffer (Xenia: a non-zero one crashes)
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

// Batch 23: I/O completion ports (sys_object_tests).
ANY360_SYS_EXPORT(NtCreateIoCompletion) { ctx.r3.u64 = Objects().CreateIoCompletion({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(NtSetIoCompletion) {
  ctx.r3.u64 = Objects().SetIoCompletion({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(NtRemoveIoCompletion) {
  // (handle, key out, APC context out, I/O status, timeout)
  int64_t storage;
  ctx.r3.u64 = Objects().RemoveIoCompletion({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, Timeout(base, ctx.r7.u32, storage));
}

// Batch 24: XAM's user and profile calls, parties, avatars, a few system queries (sys_io_tests, sys_object_tests).
ANY360_SYS_EXPORT(XamUserGetGamerTag) { ctx.r3.u64 = XamState().UserGetGamerTag({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamUserIsOnlineEnabled) { ctx.r3.u64 = 0; }  // no Xbox Live (the reference answers 1)
ANY360_SYS_EXPORT(XamUserGetMembershipTier) {
  // The local profile's tier as the reference answers it (6, Gold); ERROR_INVALID_PARAMETER past user 3.
  ctx.r3.u64 = ctx.r3.u32 >= 4 ? 0x57 : ctx.r3.u32 ? kErrorNoSuchUser : 6;
}
ANY360_SYS_EXPORT(XamUserContentRestrictionGetFlags) {
  ctx.r3.u64 = XamState().UserContentRestrictionGetFlags({base}, ctx.r3.u32, ctx.r4.u32);
}
ANY360_SYS_EXPORT(XamUserContentRestrictionGetRating) {
  ctx.r3.u64 = XamState().UserContentRestrictionGetRating({base}, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(XamUserContentRestrictionCheckAccess) {
  // (user, four arguments, allowed out, overlapped)
  ctx.r3.u64 = XamState().UserContentRestrictionCheckAccess({base}, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(XamUserGetDeviceContext) {
  // A signed result, sign-extended into r3 as the reference returns it.
  ctx.r3.u64 = uint64_t(int64_t(int32_t(XamState().UserGetDeviceContext({base}, ctx.r3.u32, ctx.r5.u32))));
}
ANY360_SYS_EXPORT(XamUserReadProfileSettingsEx) {
  // XamUserReadProfileSettings with one more argument (r10) before the buffer.
  ctx.r3.u64 = XamState().UserReadProfileSettings({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                                  ctx.r9.u32, StackArgument(ctx, base, 8), StackArgument(ctx, base, 9));
}
ANY360_SYS_EXPORT(XamNotifyCreateListenerInternal) { ctx.r3.u64 = XamState().NotifyCreateListener({base}, ctx.r3.u64, ctx.r5.u32); }
// Parties need Xbox Live: no party (the code the reference found titles to expect), the rest fails.
ANY360_SYS_EXPORT(XamPartyGetUserList) { ctx.r3.u64 = 0x807D0003; }
ANY360_SYS_EXPORT(XamPartySendGameInvites) { ctx.r3.u64 = kErrorFunctionFailed; }
ANY360_SYS_EXPORT(XamPartySetCustomData) { ctx.r3.u64 = kErrorFunctionFailed; }
ANY360_SYS_EXPORT(XamPartyGetBandwidth) { ctx.r3.u64 = kErrorFunctionFailed; }
ANY360_SYS_EXPORT(XamShowPartyUI) { ctx.r3.u64 = kErrorFunctionFailed; }
ANY360_SYS_EXPORT(XamShowCommunitySessionsUI) { ctx.r3.u64 = kErrorFunctionFailed; }
ANY360_SYS_EXPORT(XamShowNuiTroubleshooterUI) { ctx.r3.u64 = 0; }  // no Kinect to troubleshoot: closed at once
// Avatars are not rendered: initialization fails (negative), so a title shuts them down at once.
ANY360_SYS_EXPORT(XamAvatarInitialize) { ctx.r3.u64 = 0xFFFFFFFF; }
ANY360_SYS_EXPORT(XamAvatarShutdown) {}
ANY360_SYS_EXPORT(XamIsUIActive) { ctx.r3.u64 = 0; }  // the system UI opens and closes at once
ANY360_SYS_EXPORT(XamFeatureEnabled) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamVoiceIsActiveProcess) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamGetCurrentTitleId) { ctx.r3.u64 = XamState().title().title_id; }

// Batch 25: the rest of XAM's content calls (sys_object_tests).
ANY360_SYS_EXPORT(XamContentCreate) {
  // XamContentCreateEx without cache and content sizes: (user, root name, content data, flags, disposition, license, overlapped)
  ctx.r3.u64 = XamState().ContentCreate({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}
ANY360_SYS_EXPORT(XamContentCreateInternal) {
  // The system's form (user 0xFE) on an XCONTENT_AGGREGATE_DATA, whose XCONTENT_DATA comes first:
  // (root name, content data, flags, disposition, license, cache size, content size, overlapped)
  ctx.r3.u64 = XamState().ContentCreate({base}, 0xFE, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32);
}
ANY360_SYS_EXPORT(XamContentDeleteInternal) { ctx.r3.u64 = XamState().ContentDelete({base}, 0xFE, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(XamContentOpenFile) { ctx.r3.u64 = kErrorFileNotFound; }  // arguments unknown; not found, as the reference
ANY360_SYS_EXPORT(XamContentAggregateCreateEnumerator) {
  // (XUID, device, content type, unknown, handle out)
  ctx.r3.u64 = XamState().ContentAggregateCreateEnumerator({base}, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32);
}

// Batch 26: kernel odds and ends (sys_io_tests against the reference where comparable, sys_object_tests).
ANY360_SYS_EXPORT(RtlCopyString) { RtlCopyString({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlCopyUnicodeString) { RtlCopyUnicodeString({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(RtlFreeUnicodeString) { RtlFreeUnicodeString({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(RtlImageNtHeader) { ctx.r3.u64 = RtlImageNtHeader({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(OutputDebugStringA) { DebugOutput({base}, ctx.r3.u32, false); }
ANY360_SYS_EXPORT(OutputDebugStringW) { DebugOutput({base}, ctx.r3.u32, true); }
ANY360_SYS_EXPORT(RtlOutputDebugString) { DebugOutput({base}, ctx.r3.u32, false); }
ANY360_SYS_EXPORT(RtlDebugTrace) { DebugOutput({base}, ctx.r3.u32, false); }
ANY360_SYS_EXPORT(XamDbgPrint) {
  // DbgPrint's arguments and answer.
  RegisterArguments args(ctx, base, 1);
  ctx.r3.u64 = DbgPrint({base}, ctx.r3.u32, args);
}
ANY360_SYS_EXPORT(MmIsAddressValid) { ctx.r3.u64 = Memory().IsAddressValid(ctx.r3.u32); }
ANY360_SYS_EXPORT(MmCreateKernelStack) { ctx.r3.u64 = Memory().CreateKernelStack({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(MmDeleteKernelStack) { ctx.r3.u64 = Memory().DeleteKernelStack(ctx.r4.u32); }  // (stack base, stack end)
ANY360_SYS_EXPORT(KeSuspendThread) {
  uint32_t previous = 0;
  Threads().Suspend({base}, ctx.r3.u32, &previous);
  ctx.r3.u64 = previous;
}
ANY360_SYS_EXPORT(KeSetCurrentStackPointers) {
  // (stack pointer, thread, allocation base, base, limit): the caller continues on the new stack.
  Threads().SetCurrentStackPointers({base}, ctx.r13.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  ctx.r1.u64 = ctx.r3.u32;
}
ANY360_SYS_EXPORT(NtOpenSymbolicLinkObject) { ctx.r3.u64 = Files().OpenSymbolicLink({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NtQuerySymbolicLinkObject) { ctx.r3.u64 = Files().QuerySymbolicLink({base}, ctx.r3.u32, ctx.r4.u32); }
ANY360_SYS_EXPORT(NetDll_XNetSetSystemLinkPort) { ctx.r3.u64 = 1; }  // as the reference: no system link offline anyway

// Batch 28: input and the devices a retail console without them reports (sys_io_tests against the reference).
ANY360_SYS_EXPORT(XamInputGetCapabilitiesEx) {
  // (unknown, user, flags, capabilities): XamInputGetCapabilities with one more argument first.
  ctx.r3.u64 = XamState().InputGetCapabilities({base}, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}
ANY360_SYS_EXPORT(XamInputGetKeystroke) { ctx.r3.u64 = XamState().InputGetKeystroke({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }
ANY360_SYS_EXPORT(XamNuiGetDeviceStatus) {
  // XAM_NUI_DEVICE_STATUS (24 bytes) all zero: no Kinect connected.
  if (ctx.r3.u32) std::memset(Guest{base}.host(ctx.r3.u32), 0, 24);
}
// No keyboard or mouse on the console's HID: STATUS_DEVICE_NOT_CONNECTED for keys (which titles turn into
// ERROR_DEVICE_NOT_CONNECTED), STATUS_UNSUCCESSFUL for the rest, no input ever.
ANY360_SYS_EXPORT(HidReadKeys) { ctx.r3.u64 = 0xC000009D; }
ANY360_SYS_EXPORT(HidGetCapabilities) { ctx.r3.u64 = 0xC0000001; }
ANY360_SYS_EXPORT(HidReadMouseChanges) { ctx.r3.u64 = 0xC0000001; }
ANY360_SYS_EXPORT(HidGetLastInputTime) {
  if (ctx.r3.u32) Guest{base}.Store32(ctx.r3.u32, 0);
  ctx.r3.u64 = 0;
}
// The Xbox Live Vision camera: creating its buffers succeeds (titles stop loading otherwise), and it is not connected.
ANY360_SYS_EXPORT(XUsbcamCreate) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XUsbcamGetState) { ctx.r3.u64 = 0; }

// Batch 29: RSA's public operation (sys_io_tests against the reference).
ANY360_SYS_EXPORT(XeCryptBnQwNeRsaPubCrypt) { ctx.r3.u64 = XeCryptBnQwNeRsaPubCrypt({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }

// Batch 30: the last of XAM's (sys_io_tests against the reference, sys_object_tests).
ANY360_SYS_EXPORT(XamGetOnlineSchema) { ctx.r3.u64 = XamState().GetOnlineSchema({base}); }
// Step 12, the information calls Xenia stubs too.
ANY360_SYS_EXPORT(XamGetCountry) { ctx.r3.u64 = Console().country; }  // the console's (as ExGetXConfigSetting gives it)
// (country): the console's country for the run (ExGetXConfigSetting and the locale calls read it).
ANY360_SYS_EXPORT(XamSetCountry) {
  Console().country = uint8_t(ctx.r3.u32);
  ctx.r3.u64 = 0;
}
// (DVR storage out, used out, the drive's unused space out): no DVR storage set aside (0, 0; the third untouched, as
// Xenia).
ANY360_SYS_EXPORT(XamGetDvrStorage) {
  if (ctx.r3.u32) Guest{base}.Store32(ctx.r3.u32, 0);
  if (ctx.r4.u32) Guest{base}.Store32(ctx.r4.u32, 0);
  ctx.r3.u64 = 0;
}
ANY360_SYS_EXPORT(XamSetDvrStorage) { ctx.r3.u64 = 0; }  // (size, overlapped): accepted, nothing set aside
ANY360_SYS_EXPORT(XamGetLanguageTypefacePatch) { ctx.r3.u64 = XamState().LanguageTypefacePatch({base}, ctx.r3.u32); }
ANY360_SYS_EXPORT(XamGetLiveHiveValueA) {
  ctx.r3.u64 = XamState().LiveHiveValue({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamGetLiveHiveValueW) {
  ctx.r3.u64 = XamState().LiveHiveValue({base}, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32);
}
ANY360_SYS_EXPORT(XamGetStagingMode) { ctx.r3.u64 = 0; }          // a retail console, not a staging one
ANY360_SYS_EXPORT(XamIsNatalPlaybackEnabled) { ctx.r3.u64 = 0; }  // no Kinect recording played back
ANY360_SYS_EXPORT(XamIsNuiAutomationEnabled) { ctx.r3.u64 = 0; }  // no Kinect automation
ANY360_SYS_EXPORT(XamQueryLiveHiveW) { ctx.r3.u64 = 0xC00000EF; }  // STATUS_INVALID_PARAMETER_1: no Live hive offline
ANY360_SYS_EXPORT(XamSwapDisc) { ctx.r3.u64 = 0; }                 // the one disc stays in, as the reference answers
ANY360_SYS_EXPORT(XAudioEnableDucker) { ctx.r3.u64 = 0; }          // no voice chat to duck under
ANY360_SYS_EXPORT(XCustomRegisterDynamicActions) {}               // the guide's custom actions are not shown

// Batch 31: the executable's image range (sys_io_tests against the reference, sys_object_tests).
ANY360_SYS_EXPORT(KeGetImagePageTableEntry) { ctx.r3.u64 = MemoryManager::ImagePageTableEntry(ctx.r3.u32); }
ANY360_SYS_EXPORT(NtAllocateEncryptedMemory) { ctx.r3.u64 = Memory().AllocateEncrypted({base}, ctx.r4.u32, ctx.r5.u32); }  // (?, size, base out)
ANY360_SYS_EXPORT(NtFreeEncryptedMemory) { ctx.r3.u64 = Memory().FreeEncrypted({base}, ctx.r4.u32); }  // (region type, base pointer)

// Batch 32: the calls Call of Duty: Black Ops imports that no title before it did (the reference stubs them all:
// it answers with r3 as it came, its first argument).
// The guide shows no custom action: setting one is accepted and has no effect (r3 as it came, as the reference).
ANY360_SYS_EXPORT(XCustomSetAction) {}
// (user index out, action index out, XUID out): FALSE, no custom action was pressed (the guide shows none, and
// XN_CUSTOM_ACTIONPRESSED is never sent); the outputs untouched. The reference returns its first argument, a pointer:
// TRUE, an action pressed.
ANY360_SYS_EXPORT(XCustomGetLastActionPress) { ctx.r3.u64 = 0; }
ANY360_SYS_EXPORT(XamShowMarketplaceDownloadItemsUI) {
  ctx.r3.u64 = XamState().ShowMarketplaceDownloadItemsUI({base}, ctx.r3.u32, ctx.r8.u32, ctx.r9.u32);
}
// (caller, status block, 0): the Xbox network platform's logon state of an offline console: never logged on. The
// title reads a 32-bit state at +4 and a 64-bit value at +8 of the block, little-endian; both are written 0 (the
// rest of the block's layout is not known). Success.
ANY360_SYS_EXPORT(NetDll_XnpLogonGetStatus) {
  if (const uint32_t status = ctx.r4.u32) {
    const Guest g{base};
    g.Store32(status + 4, 0);
    g.Store64(status + 8, 0);
  }
  ctx.r3.u64 = 0;
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
    ANY360_SYS_ENTRY(XamInputNonControllerGetRaw),
    ANY360_SYS_ENTRY(XamInputNonControllerGetRawEx),
    ANY360_SYS_ENTRY(XamInputNonControllerSetRaw),
    ANY360_SYS_ENTRY(XamInputNonControllerSetRawEx),
    ANY360_SYS_ENTRY(NetDll_XHttpOpen),
    ANY360_SYS_ENTRY(NetDll_XHttpStartup),
    ANY360_SYS_ENTRY(XamNetworkStorageHasUserEnabledStorage),
    ANY360_SYS_ENTRY(XNotifyBroadcast),
    ANY360_SYS_ENTRY(XNotifyQueueUI),
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
    ANY360_SYS_ENTRY(XAutomationpUnbindController),
    ANY360_SYS_ENTRY(XamVoiceGetMicArrayStatus),
    ANY360_SYS_ENTRY(MicDeviceRequest),
    ANY360_SYS_ENTRY(HalOpenCloseODDTray),
    ANY360_SYS_ENTRY(HalSendSMCMessage),
    ANY360_SYS_ENTRY(NtCancelIoFile),
    ANY360_SYS_ENTRY(RtlRip),
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
    ANY360_SYS_ENTRY(XamShowGoldUpgradeUI),
    ANY360_SYS_ENTRY(XamShowMarketplaceUIEx),
    ANY360_SYS_ENTRY(XamShowNuiDeviceSelectorUI),
    ANY360_SYS_ENTRY(XamShowNuiDirtyDiscErrorUI),
    ANY360_SYS_ENTRY(XamShowNuiGuideUI),
    ANY360_SYS_ENTRY(XamShowNuiMessageBoxUI),
    ANY360_SYS_ENTRY(XamShowNuiSigninUI),
    ANY360_SYS_ENTRY(XamShowSigninUIEx),
    ANY360_SYS_ENTRY(IoDismountVolumeByFileHandle),
    ANY360_SYS_ENTRY(XeKeysConsolePrivateKeySign),
    ANY360_SYS_ENTRY(XeKeysConsoleSignatureVerification),
    ANY360_SYS_ENTRY(KeSaveFloatingPointState),
    ANY360_SYS_ENTRY(KeRestoreFloatingPointState),
    ANY360_SYS_ENTRY(NtDeviceIoControlFile),
    ANY360_SYS_ENTRY(XexLoadImage),
    ANY360_SYS_ENTRY(XamLoaderGetMediaInfo),
    ANY360_SYS_ENTRY(XamLoaderGetMediaInfoEx),
    ANY360_SYS_ENTRY(XamUpdateGetBaseSystemVersion),
    ANY360_SYS_ENTRY(XamUpdateGetCurrentSystemVersion),
    ANY360_SYS_ENTRY(XexLoadExecutable),
    ANY360_SYS_ENTRY(XexUnloadImage),
    ANY360_SYS_ENTRY(XamContentLaunchImage),
    ANY360_SYS_ENTRY(XamContentGetAttributesInternal),
    ANY360_SYS_ENTRY(XamContentGetDeviceVolumePath),
    ANY360_SYS_ENTRY(XamContentGetLocalizedDeviceData),
    ANY360_SYS_ENTRY(XamContentGetLocalizedString),
    ANY360_SYS_ENTRY(XamContentLaunchImageFromFileInternal),
    ANY360_SYS_ENTRY(XamContentLaunchImageInternal),
    ANY360_SYS_ENTRY(XamContentLaunchImageInternalEx),
    ANY360_SYS_ENTRY(XamContentResolveInternal),
    ANY360_SYS_ENTRY(XamReadTile),
    ANY360_SYS_ENTRY(XamReadTileEx),
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
    ANY360_SYS_ENTRY(KeGetImagePageTableEntry),
    ANY360_SYS_ENTRY(NtAllocateEncryptedMemory),
    ANY360_SYS_ENTRY(NtFreeEncryptedMemory),
    ANY360_SYS_ENTRY(XamGetOnlineSchema),
    ANY360_SYS_ENTRY(XamQueryLiveHiveW),
    ANY360_SYS_ENTRY(XamGetCountry),
    ANY360_SYS_ENTRY(XamGetDvrStorage),
    ANY360_SYS_ENTRY(XamGetLanguageTypefacePatch),
    ANY360_SYS_ENTRY(XamGetLiveHiveValueA),
    ANY360_SYS_ENTRY(XamGetLiveHiveValueW),
    ANY360_SYS_ENTRY(XamGetStagingMode),
    ANY360_SYS_ENTRY(XamIsNatalPlaybackEnabled),
    ANY360_SYS_ENTRY(XamIsNuiAutomationEnabled),
    ANY360_SYS_ENTRY(XamSetCountry),
    ANY360_SYS_ENTRY(XamSetDvrStorage),
    ANY360_SYS_ENTRY(XamSwapDisc),
    ANY360_SYS_ENTRY(XAudioEnableDucker),
    ANY360_SYS_ENTRY(XCustomRegisterDynamicActions),
    ANY360_SYS_ENTRY(XeCryptBnQwNeRsaPubCrypt),
    ANY360_SYS_ENTRY(XamInputGetCapabilitiesEx),
    ANY360_SYS_ENTRY(XamInputGetKeystroke),
    ANY360_SYS_ENTRY(XamNuiGetDeviceStatus),
    ANY360_SYS_ENTRY(HidReadKeys),
    ANY360_SYS_ENTRY(HidGetCapabilities),
    ANY360_SYS_ENTRY(HidReadMouseChanges),
    ANY360_SYS_ENTRY(HidGetLastInputTime),
    ANY360_SYS_ENTRY(XUsbcamCreate),
    ANY360_SYS_ENTRY(XUsbcamGetState),
    ANY360_SYS_ENTRY(RtlCopyString),
    ANY360_SYS_ENTRY(RtlCopyUnicodeString),
    ANY360_SYS_ENTRY(RtlFreeUnicodeString),
    ANY360_SYS_ENTRY(RtlImageNtHeader),
    ANY360_SYS_ENTRY(OutputDebugStringA),
    ANY360_SYS_ENTRY(OutputDebugStringW),
    ANY360_SYS_ENTRY(RtlOutputDebugString),
    ANY360_SYS_ENTRY(RtlDebugTrace),
    ANY360_SYS_ENTRY(XamDbgPrint),
    ANY360_SYS_ENTRY(MmIsAddressValid),
    ANY360_SYS_ENTRY(MmCreateKernelStack),
    ANY360_SYS_ENTRY(MmDeleteKernelStack),
    ANY360_SYS_ENTRY(KeSuspendThread),
    ANY360_SYS_ENTRY(KeSetCurrentStackPointers),
    ANY360_SYS_ENTRY(NtOpenSymbolicLinkObject),
    ANY360_SYS_ENTRY(NtQuerySymbolicLinkObject),
    ANY360_SYS_ENTRY(NetDll_XNetSetSystemLinkPort),
    ANY360_SYS_ENTRY(XamContentCreate),
    ANY360_SYS_ENTRY(XamContentCreateInternal),
    ANY360_SYS_ENTRY(XamContentDeleteInternal),
    ANY360_SYS_ENTRY(XamContentOpenFile),
    ANY360_SYS_ENTRY(XamContentAggregateCreateEnumerator),
    ANY360_SYS_ENTRY(XamUserGetGamerTag),
    ANY360_SYS_ENTRY(XamUserIsOnlineEnabled),
    ANY360_SYS_ENTRY(XamUserGetMembershipTier),
    ANY360_SYS_ENTRY(XamUserContentRestrictionGetFlags),
    ANY360_SYS_ENTRY(XamUserContentRestrictionGetRating),
    ANY360_SYS_ENTRY(XamUserContentRestrictionCheckAccess),
    ANY360_SYS_ENTRY(XamUserGetDeviceContext),
    ANY360_SYS_ENTRY(XamUserReadProfileSettingsEx),
    ANY360_SYS_ENTRY(XamNotifyCreateListenerInternal),
    ANY360_SYS_ENTRY(XamPartyGetUserList),
    ANY360_SYS_ENTRY(XamPartySendGameInvites),
    ANY360_SYS_ENTRY(XamPartySetCustomData),
    ANY360_SYS_ENTRY(XCustomSetAction),
    ANY360_SYS_ENTRY(XCustomGetLastActionPress),
    ANY360_SYS_ENTRY(XamShowMarketplaceDownloadItemsUI),
    ANY360_SYS_ENTRY(NetDll_XnpLogonGetStatus),
    ANY360_SYS_ENTRY(XamPartyGetBandwidth),
    ANY360_SYS_ENTRY(XamShowPartyUI),
    ANY360_SYS_ENTRY(XamShowCommunitySessionsUI),
    ANY360_SYS_ENTRY(XamShowNuiTroubleshooterUI),
    ANY360_SYS_ENTRY(XamAvatarInitialize),
    ANY360_SYS_ENTRY(XamAvatarShutdown),
    ANY360_SYS_ENTRY(XamIsUIActive),
    ANY360_SYS_ENTRY(XamFeatureEnabled),
    ANY360_SYS_ENTRY(XamVoiceIsActiveProcess),
    ANY360_SYS_ENTRY(XamGetCurrentTitleId),
    ANY360_SYS_ENTRY(NtCreateIoCompletion),
    ANY360_SYS_ENTRY(NtSetIoCompletion),
    ANY360_SYS_ENTRY(NtRemoveIoCompletion),
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
    ANY360_SYS_ENTRY(XeCryptMd5),
    ANY360_SYS_ENTRY(XeCryptSha256),
    ANY360_SYS_ENTRY(XeCryptMd5Init),
    ANY360_SYS_ENTRY(XeCryptMd5Update),
    ANY360_SYS_ENTRY(XeCryptMd5Final),
    ANY360_SYS_ENTRY(XeCryptHmacMd5),
    ANY360_SYS_ENTRY(XeCryptHmacShaInit),
    ANY360_SYS_ENTRY(XeCryptHmacShaUpdate),
    ANY360_SYS_ENTRY(XeCryptHmacShaFinal),
    ANY360_SYS_ENTRY(XeCryptHmacShaVerify),
    ANY360_SYS_ENTRY(XeCryptDesKey),
    ANY360_SYS_ENTRY(XeCryptDesEcb),
    ANY360_SYS_ENTRY(XeCryptDesCbc),
    ANY360_SYS_ENTRY(Sleep),
    ANY360_SYS_ENTRY(SleepEx),
    ANY360_SYS_ENTRY(WaitForSingleObject),
    ANY360_SYS_ENTRY(WaitForSingleObjectEx),
    ANY360_SYS_ENTRY(CreateThread),
    ANY360_SYS_ENTRY(ResumeThread),
    ANY360_SYS_ENTRY(ExitThread),
    ANY360_SYS_ENTRY(GetCurrentThreadId),
    ANY360_SYS_ENTRY(GetLastError),
    ANY360_SYS_ENTRY(GetModuleHandleA),
    ANY360_SYS_ENTRY(GetTickCount),
    ANY360_SYS_ENTRY(QueryPerformanceFrequency),
    ANY360_SYS_ENTRY(GetSystemTimeAsFileTime),
    ANY360_SYS_ENTRY(lstrlenW),
    ANY360_SYS_ENTRY(XGetAudioFlags),
    ANY360_SYS_ENTRY(XGetOverlappedExtendedError),
    ANY360_SYS_ENTRY(XamGetActiveDashAppInfo),
    ANY360_SYS_ENTRY(XamSetActiveDashAppInfo),
    ANY360_SYS_ENTRY(XamGetCachedTitleName),
    ANY360_SYS_ENTRY(XamGetDashBackstackData),
    ANY360_SYS_ENTRY(XamGetDashContext),
    ANY360_SYS_ENTRY(XamGetLanguage),
    ANY360_SYS_ENTRY(XamGetLanguageLocaleFallbackString),
    ANY360_SYS_ENTRY(XamGetLanguageTypeface),
    ANY360_SYS_ENTRY(XamGetOnlineCountryFeatures),
    ANY360_SYS_ENTRY(XamGetOnlineLanguageAndCountry),
    ANY360_SYS_ENTRY(XamIsChildAccountSignedIn),
    ANY360_SYS_ENTRY(XamIsCurrentTitleDash),
    ANY360_SYS_ENTRY(XamIsIptvEnabled),
    ANY360_SYS_ENTRY(XamIsNuiUIActive),
    ANY360_SYS_ENTRY(XamIsSystemExperienceTitleId),
    ANY360_SYS_ENTRY(XamIsSystemTitleId),
    ANY360_SYS_ENTRY(XamIsXbox1TitleId),
    ANY360_SYS_ENTRY(RtlGetLastError),
    ANY360_SYS_ENTRY(RtlSetLastNTError),
    ANY360_SYS_ENTRY(RtlSleep),
    ANY360_SYS_ENTRY(RtlRandom),
    ANY360_SYS_ENTRY(RtlDowncaseUnicodeChar),
    ANY360_SYS_ENTRY(RtlGetStackLimits),
    ANY360_SYS_ENTRY(RtlImageDirectoryEntryToData),
    ANY360_SYS_ENTRY(KeQueryInterruptTime),
    ANY360_SYS_ENTRY(KeSetPriorityThread),
    ANY360_SYS_ENTRY(HalGetCurrentAVPack),
    ANY360_SYS_ENTRY(XamGetLocaleDateFormat),
    ANY360_SYS_ENTRY(XamNuiHudGetEngagedTrackingID),
    ANY360_SYS_ENTRY(XamNuiHudGetInitializeFlags),
    ANY360_SYS_ENTRY(XamNuiHudGetVersions),
    ANY360_SYS_ENTRY(XamNuiHudIsEnabled),
    ANY360_SYS_ENTRY(XamNuiCameraElevationGetAngle),
    ANY360_SYS_ENTRY(XamNuiCameraGetTiltControllerType),
    ANY360_SYS_ENTRY(XamNuiCameraSetFlags),
    ANY360_SYS_ENTRY(XamNuiCameraTiltGetStatus),
    ANY360_SYS_ENTRY(XamNuiCameraTiltSetCallback),
    ANY360_SYS_ENTRY(XamNuiGetDepthCalibration),
    ANY360_SYS_ENTRY(XamNuiIdentityAbort),
    ANY360_SYS_ENTRY(XamNuiIdentityEnrollForSignIn),
    ANY360_SYS_ENTRY(XamNuiPlayerEngagementUpdate),
    ANY360_SYS_ENTRY(XamNuiSkeletonGetBestSkeletonIndex),
    ANY360_SYS_ENTRY(XamNuiHudSetEngagedTrackingID),
    ANY360_SYS_ENTRY(XamNuiIdentityGetSessionId),
    ANY360_SYS_ENTRY(XamNuiIsChatMicEnabled),
    ANY360_SYS_ENTRY(XamNuiIsDeviceReady),
    ANY360_SYS_ENTRY(XamSetDashContext),
    ANY360_SYS_ENTRY(XamShowCreateProfileUI),
    ANY360_SYS_ENTRY(XamShowCreateProfileUIEx),
    ANY360_SYS_ENTRY(XamShowEditProfileUI),
    ANY360_SYS_ENTRY(XamShowForcedNameChangeUI),
    ANY360_SYS_ENTRY(XamShowGamerCardUI),
    ANY360_SYS_ENTRY(XamShowMultiplayerUpgradeUI),
    ANY360_SYS_ENTRY(XamShowNuiHardwareRequiredUI),
    ANY360_SYS_ENTRY(XamProfileCreateEnumerator),
    ANY360_SYS_ENTRY(XamProfileEnumerate),
    ANY360_SYS_ENTRY(XamProfileFindAccount),
    ANY360_SYS_ENTRY(XamProfileGetLiveLegalLocale),
    ANY360_SYS_ENTRY(XamProfileOpen),
    ANY360_SYS_ENTRY(XamUserGetCachedUserFlags),
    ANY360_SYS_ENTRY(XamUserGetIndexFromXUID),
    ANY360_SYS_ENTRY(XamUserGetOnlineLanguageFromXUID),
    ANY360_SYS_ENTRY(XamUserGetSubscriptionType),
    ANY360_SYS_ENTRY(XamUserGetUserFlags),
    ANY360_SYS_ENTRY(XamUserGetUserFlagsFromXUID),
    ANY360_SYS_ENTRY(XamUserGetUserTenure),
    ANY360_SYS_ENTRY(XamUserIsParentalControlled),
    ANY360_SYS_ENTRY(XamUserLogon),
    ANY360_SYS_ENTRY(XamUserNuiGetUserIndexForSignin),
    ANY360_SYS_ENTRY(XamProfileClose),
    ANY360_SYS_ENTRY(XamProfileCreate),
    ANY360_SYS_ENTRY(XamProfileGetCreationStatus),
    ANY360_SYS_ENTRY(XamProfileLoadAccountInfo),
    ANY360_SYS_ENTRY(XamUserCreateTitlesPlayedEnumerator),
    ANY360_SYS_ENTRY(XamUserIsGuest),
    ANY360_SYS_ENTRY(XamUserIsUnsafeProgrammingAllowed),
    ANY360_SYS_ENTRY(XamUserLogonEx),
    ANY360_SYS_ENTRY(XamUserNuiEnableBiometric),
    ANY360_SYS_ENTRY(XamUserNuiGetUserIndex),
    ANY360_SYS_ENTRY(XamUserNuiGetUserIndexForBind),
    ANY360_SYS_ENTRY(XamUserReadUserPreference),
    ANY360_SYS_ENTRY(XamUserWriteUserPreference),
    ANY360_SYS_ENTRY(XeCryptSha512Init),
    ANY360_SYS_ENTRY(XeCryptSha512Update),
    ANY360_SYS_ENTRY(XeCryptSha512Final),
    ANY360_SYS_ENTRY(XeCryptBnQw_Copy),
    ANY360_SYS_ENTRY(XeCryptBnQwNeModInv),
    ANY360_SYS_ENTRY(XeCryptBnQwNeModMul),
    ANY360_SYS_ENTRY(XeCryptBnQwBeSigFormat),
    ANY360_SYS_ENTRY(XeCryptBnDwLePkcs1Format),
    ANY360_SYS_ENTRY(XeKeysGetConsoleType),
    ANY360_SYS_ENTRY(XeKeysGetConsoleID),
    ANY360_SYS_ENTRY(XeKeysGetKeyProperties),
    ANY360_SYS_ENTRY(XTLGetLanguageV2),
    ANY360_SYS_ENTRY(XapipGetLocale),
    ANY360_SYS_ENTRY(XamAllocEx),
    ANY360_SYS_ENTRY(XapiFormatTimeOut),
    ANY360_SYS_ENTRY(XapipCreateThread),
    ANY360_SYS_ENTRY(XamIptvGetServiceName),
    ANY360_SYS_ENTRY(LDICreateDecompression),
    ANY360_SYS_ENTRY(LDIDecompress),
    ANY360_SYS_ENTRY(LDIDestroyDecompression),
    ANY360_SYS_ENTRY(XamDoesOmniNeedConfiguration),
    ANY360_SYS_ENTRY(XamFirstRunExperienceShouldRun),
    ANY360_SYS_ENTRY(XamLogLocalizationEtx),
    ANY360_SYS_ENTRY(XamLookupCommonStringByIndex),
    ANY360_SYS_ENTRY(XamQueryLiveHiveA),
    ANY360_SYS_ENTRY(XamXStudioRequest),
    ANY360_SYS_ENTRY(ExReadModifyWriteXConfigSettingUlong),
    ANY360_SYS_ENTRY(ExSetXConfigSetting),
    ANY360_SYS_ENTRY(LDIResetDecompression),
    ANY360_SYS_ENTRY(XamContentCreateEnumeratorInternal),
    ANY360_SYS_ENTRY(XamContentGetMetaDataInternal),
    ANY360_SYS_ENTRY(XamContentSetThumbnailInternal),
    ANY360_SYS_ENTRY(XamContentRegisterChangeCallback),
    ANY360_SYS_ENTRY(XamLoaderRegisterLaunchRequestCallback),
    ANY360_SYS_ENTRY(XamReadString),
    ANY360_SYS_ENTRY(NetDll_WSAStartupEx),
    ANY360_SYS_ENTRY(NetDll_XNetStartupEx),
    ANY360_SYS_ENTRY(XamNetworkStorageUserHasPrivilege),
    ANY360_SYS_ENTRY(ObLookupAnyThreadByThreadId),
    ANY360_SYS_ENTRY(ObCreateObject),
    ANY360_SYS_ENTRY(XexLoadImageFromMemory),
    ANY360_SYS_ENTRY(XamAvatarBeginEnumAssets),
    ANY360_SYS_ENTRY(XamAvatarEndEnumAssets),
    ANY360_SYS_ENTRY(XamAvatarEnumAssets),
    ANY360_SYS_ENTRY(XamAvatarGenerateMipMaps),
    ANY360_SYS_ENTRY(XamAvatarGetAssetBinary),
    ANY360_SYS_ENTRY(XamAvatarGetAssetIcon),
    ANY360_SYS_ENTRY(XamAvatarGetAssets),
    ANY360_SYS_ENTRY(XamAvatarGetAssetsResultSize),
    ANY360_SYS_ENTRY(XamAvatarGetInstalledAssetPackageDescription),
    ANY360_SYS_ENTRY(XamAvatarGetInstrumentation),
    ANY360_SYS_ENTRY(XamAvatarGetManifestLocalUser),
    ANY360_SYS_ENTRY(XamAvatarGetManifestsByXuid),
    ANY360_SYS_ENTRY(XamAvatarGetMetadataRandom),
    ANY360_SYS_ENTRY(XamAvatarGetMetadataSignedOutProfile),
    ANY360_SYS_ENTRY(XamAvatarGetMetadataSignedOutProfileCount),
    ANY360_SYS_ENTRY(XamAvatarLoadAnimation),
    ANY360_SYS_ENTRY(XamAvatarManifestGetBodyType),
    ANY360_SYS_ENTRY(XamAvatarReinstallAwardedAsset),
    ANY360_SYS_ENTRY(XamAvatarSetCustomAsset),
    ANY360_SYS_ENTRY(XamAvatarSetManifest),
    ANY360_SYS_ENTRY(XamAvatarSetMocks),
    ANY360_SYS_ENTRY(XamAvatarWearNow),
    ANY360_SYS_ENTRY(EnumerateMediaObjects),
    ANY360_SYS_ENTRY(EnumerateMediaObjects_),
    ANY360_SYS_ENTRY(EnumerateMediaObjects_0),
    ANY360_SYS_ENTRY(EnumerateMediaObjects_1),
    ANY360_SYS_ENTRY(XamMediaVerificationClose),
    ANY360_SYS_ENTRY(XamMediaVerificationCreate),
    ANY360_SYS_ENTRY(XamMediaVerificationFailedBlocks),
    ANY360_SYS_ENTRY(XamMediaVerificationInject),
    ANY360_SYS_ENTRY(XamMediaVerificationVerify),
    ANY360_SYS_ENTRY(XampWebInstrumentationSetProfileCounts),
    ANY360_SYS_ENTRY(XampXAuthGetTitleBuffer),
    ANY360_SYS_ENTRY(XampXAuthShutdown),
    ANY360_SYS_ENTRY(XampXAuthStartup),
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

// The kernel's exported variables (sys_object_tests); their data is KernelVariables'.
#define ANY360_SYS_VARIABLE(name) {#name}
const Variable kVariables[] = {
    ANY360_SYS_VARIABLE(ExConsoleGameRegion),
    ANY360_SYS_VARIABLE(ExEventObjectType),
    ANY360_SYS_VARIABLE(ExLoadedCommandLine),
    ANY360_SYS_VARIABLE(ExLoadedImageName),
    ANY360_SYS_VARIABLE(ExMutantObjectType),
    ANY360_SYS_VARIABLE(ExSemaphoreObjectType),
    ANY360_SYS_VARIABLE(ExThreadObjectType),
    ANY360_SYS_VARIABLE(ExTimerObjectType),
    ANY360_SYS_VARIABLE(IoCompletionObjectType),
    ANY360_SYS_VARIABLE(IoDeviceObjectType),
    ANY360_SYS_VARIABLE(IoFileObjectType),
    ANY360_SYS_VARIABLE(KeCertMonitorData),
    ANY360_SYS_VARIABLE(KeDebugMonitorData),
    ANY360_SYS_VARIABLE(KeTimeStampBundle),
    ANY360_SYS_VARIABLE(ObDirectoryObjectType),
    ANY360_SYS_VARIABLE(ObSymbolicLinkObjectType),
    ANY360_SYS_VARIABLE(XboxHardwareInfo),
    ANY360_SYS_VARIABLE(XboxKrnlVersion),
    ANY360_SYS_VARIABLE(XexExecutableModuleHandle),
};
const unsigned kVariableCount = sizeof(kVariables) / sizeof(kVariables[0]);

uint32_t VariableAddress(uint8_t* base, const char* name) {
  for (const Variable& v : kVariables) {
    if (!std::strcmp(v.name, name)) return Variables().Address({base}, name);
  }
  return 0;
}

}  // namespace any360::sys
