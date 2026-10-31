// PowerPC ABI adapters: one `any360_<Export>(PPCContext&, uint8_t*)` per system call the layer provides, with the same
// calling convention as the kernel import thunks of a static recompilation (arguments in r3..r10, result in r3), so a
// title binds them in place of the reference runtime's `__imp__<Export>`. kExports lists them by name for the binder and
// for tools/build_db.py, which counts every name here as native.
#include <rex/ppc.h>

#include "any360/sys/config.h"
#include "any360/sys/exports.h"
#include "any360/sys/info.h"
#include "any360/sys/net.h"
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
};
const unsigned kExportCount = sizeof(kExports) / sizeof(kExports[0]);

}  // namespace any360::sys
