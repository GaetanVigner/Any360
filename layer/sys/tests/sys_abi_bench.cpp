// sys_abi_bench: the original against Any360 for step 12's system calls (docs/SYS_STUBS.md), through the PowerPC
// ABI a title calls them by: the reference runtime's `__imp__<name>` (ReXGlue, linked from the reference SDK) and
// Any360's adapter (any360::sys::kExports), each on the same registers and guest memory. Prints nanoseconds a call
// (fastest of the runs) and the host heap a call (bytes, allocations) of Any360's side; the reference runtime is a
// DLL with its own heap, which the allocation meter (operator new of this executable) does not see.
// The calls that block (Sleep, SleepEx, RtlSleep, the waits, CreateThread, XapipCreateThread, ResumeThread, ExitThread) are
// left out: they are their kernel call (measured with it) plus the Win32 part sys_step12_bench times. Calls that work
// in pairs or streams are timed as such on both sides (XamAllocEx with the XamFree that returns its pages on Any360's
// side; LDI's Create and Destroy, and a whole LZX stream: Create, every chunk, Destroy).
//
//   sys_abi_bench [repeats]   (results kept in docs/perf/archive/<date>-<step>/)
#include <rex/ppc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/crypt.h"
#include "any360/sys/exports.h"
#include "any360/sys/ldi.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"
#include "guest_memory.h"
#if defined(_WIN32)
#include "lzx_cab.h"
#endif

using Function = void (*)(PPCContext& ctx, uint8_t* base);

#define REFERENCE(name) extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base);
#define STEP12_CALLS(X)                                                                                              \
  X(XeCryptMd5) X(XeCryptSha256) X(XeCryptMd5Init) X(XeCryptMd5Update) X(XeCryptMd5Final) X(XeCryptHmacMd5)          \
  X(XeCryptHmacShaInit) X(XeCryptHmacShaUpdate) X(XeCryptHmacShaFinal) X(XeCryptHmacShaVerify) X(XeCryptDesKey)      \
  X(XeCryptDesEcb) X(XeCryptDesCbc) X(GetLastError) X(GetCurrentThreadId) X(GetTickCount) X(QueryPerformanceFrequency) \
  X(GetSystemTimeAsFileTime) X(lstrlenW) X(GetModuleHandleA) X(XGetAudioFlags) X(XGetOverlappedExtendedError)        \
  X(XamGetActiveDashAppInfo) X(XamSetActiveDashAppInfo) X(XamGetCachedTitleName) X(XamGetDashBackstackData)          \
  X(XamGetDashContext) X(XamGetLanguage) X(XamGetLanguageLocaleFallbackString) X(XamGetLanguageTypeface)            \
  X(XamGetOnlineCountryFeatures) X(XamGetOnlineLanguageAndCountry) X(XamIsChildAccountSignedIn)                      \
  X(XamIsCurrentTitleDash) X(XamIsIptvEnabled) X(XamIsNuiUIActive) X(XamIsSystemExperienceTitleId)                   \
  X(XamIsSystemTitleId) X(XamIsXbox1TitleId) X(RtlGetLastError) X(RtlSetLastNTError) X(RtlRandom)                    \
  X(RtlDowncaseUnicodeChar) X(RtlGetStackLimits) X(RtlImageDirectoryEntryToData) X(KeQueryInterruptTime)            \
  X(KeSetPriorityThread) X(HalGetCurrentAVPack) X(XamGetLocaleDateFormat) X(XamNuiHudGetEngagedTrackingID)           X(XamNuiHudGetInitializeFlags) X(XamNuiHudGetVersions) X(XamNuiHudIsEnabled) X(XamNuiHudSetEngagedTrackingID)        X(XamNuiIdentityGetSessionId) X(XamNuiIsChatMicEnabled) X(XamNuiIsDeviceReady) X(XamSetDashContext)                  X(XamShowCreateProfileUI) X(XamShowCreateProfileUIEx) X(XamShowEditProfileUI) X(XamShowForcedNameChangeUI)          X(XamShowGamerCardUI) X(XamShowMultiplayerUpgradeUI) X(XamShowNuiHardwareRequiredUI) X(XamProfileCreateEnumerator) \
  X(XamProfileEnumerate) X(XamProfileFindAccount) X(XamProfileGetLiveLegalLocale) X(XamProfileOpen)                   \
  X(XamUserGetCachedUserFlags) X(XamUserGetIndexFromXUID) X(XamUserGetOnlineLanguageFromXUID)                        \
  X(XamUserGetSubscriptionType) X(XamUserGetUserFlags) X(XamUserGetUserFlagsFromXUID) X(XamUserGetUserTenure)        \
  X(XamUserIsParentalControlled) X(XamUserLogon) X(XamUserNuiGetUserIndexForSignin) X(XamUserReadUserPreference)     \
  X(XamUserWriteUserPreference) X(XeCryptSha512Init) X(XeCryptSha512Update) X(XeCryptSha512Final) X(XeCryptBnQw_Copy)    \
  X(XeCryptBnQwNeModInv) X(XeCryptBnQwNeModMul) X(XeCryptBnQwBeSigFormat) X(XeCryptBnDwLePkcs1Format)                  \
  X(XeKeysGetConsoleType) X(XeKeysGetConsoleID) X(XeKeysGetKeyProperties) X(XTLGetLanguageV2) X(XapipGetLocale)     \
  X(XamAllocEx) X(XapiFormatTimeOut) X(XamIptvGetServiceName) X(LDICreateDecompression) X(LDIDecompress)               \
  X(LDIDestroyDecompression) X(XamContentCreateEnumeratorInternal) X(XamContentGetMetaDataInternal)                     \
  X(XamContentSetThumbnailInternal) X(XamContentRegisterChangeCallback) X(XamLoaderRegisterLaunchRequestCallback)      \
  X(XamReadString) X(NetDll_WSAStartupEx) X(NetDll_XNetStartupEx) X(XamNetworkStorageUserHasPrivilege)                 \
  X(ObLookupAnyThreadByThreadId) X(ObCreateObject) X(XexLoadImageFromMemory)                                          \
  X(XamAvatarBeginEnumAssets) X(XamAvatarEndEnumAssets) X(XamAvatarEnumAssets) X(XamAvatarGenerateMipMaps)             \
  X(XamAvatarGetAssetBinary) X(XamAvatarGetAssetIcon) X(XamAvatarGetAssets) X(XamAvatarGetAssetsResultSize)            \
  X(XamAvatarGetInstalledAssetPackageDescription) X(XamAvatarGetInstrumentation) X(XamAvatarGetManifestLocalUser)      \
  X(XamAvatarGetManifestsByXuid) X(XamAvatarGetMetadataRandom) X(XamAvatarGetMetadataSignedOutProfile)                 \
  X(XamAvatarGetMetadataSignedOutProfileCount) X(XamAvatarLoadAnimation) X(XamAvatarManifestGetBodyType)               \
  X(XamAvatarReinstallAwardedAsset) X(XamAvatarSetCustomAsset) X(XamAvatarSetManifest) X(XamAvatarSetMocks)            \
  X(XamAvatarWearNow) \
  X(EnumerateMediaObjects) X(EnumerateMediaObjects_) X(EnumerateMediaObjects_0) X(EnumerateMediaObjects_1)             \
  X(XamMediaVerificationClose) X(XamMediaVerificationCreate) X(XamMediaVerificationFailedBlocks)                       \
  X(XamMediaVerificationInject) X(XamMediaVerificationVerify) X(XampWebInstrumentationSetProfileCounts)                \
  X(XampXAuthGetTitleBuffer) X(XampXAuthShutdown) X(XampXAuthStartup)                                                  \
  X(XamProfileClose) X(XamProfileCreate) X(XamProfileGetCreationStatus) X(XamProfileLoadAccountInfo)                   \
  X(XamUserCreateTitlesPlayedEnumerator) X(XamUserIsGuest) X(XamUserIsUnsafeProgrammingAllowed) X(XamUserLogonEx)      \
  X(XamUserNuiEnableBiometric) X(XamUserNuiGetUserIndex) X(XamUserNuiGetUserIndexForBind)                              \
  X(XamContentGetAttributesInternal) X(XamContentGetDeviceVolumePath) X(XamContentGetLocalizedDeviceData)              \
  X(XamContentGetLocalizedString) X(XamContentLaunchImageFromFileInternal) X(XamContentLaunchImageInternal)            \
  X(XamContentLaunchImageInternalEx) X(XamContentResolveInternal) X(XamReadTile) X(XamReadTileEx)                      \
  X(XamGetCountry) X(XamGetDvrStorage) X(XamGetLanguageTypefacePatch) X(XamGetLiveHiveValueA)                          \
  X(XamGetLiveHiveValueW) X(XamGetStagingMode) X(XamIsNatalPlaybackEnabled) X(XamIsNuiAutomationEnabled)               \
  X(XamSetCountry) X(XamSetDvrStorage)                                                                                 \
  X(XamNuiCameraElevationGetAngle) X(XamNuiCameraGetTiltControllerType) X(XamNuiCameraSetFlags)                        \
  X(XamNuiCameraTiltGetStatus) X(XamNuiCameraTiltSetCallback) X(XamNuiGetDepthCalibration) X(XamNuiIdentityAbort)      \
  X(XamNuiIdentityEnrollForSignIn) X(XamNuiPlayerEngagementUpdate) X(XamNuiSkeletonGetBestSkeletonIndex)               \
  X(XamDoesOmniNeedConfiguration) X(XamFirstRunExperienceShouldRun) X(XamLogLocalizationEtx)                           \
  X(XamLookupCommonStringByIndex) X(XamQueryLiveHiveA) X(XamXStudioRequest) X(ExReadModifyWriteXConfigSettingUlong)    \
  X(ExSetXConfigSetting) X(LDIResetDecompression)                                                                      \
  X(XamShowGoldUpgradeUI) X(XamShowMarketplaceUIEx) X(XamShowNuiDeviceSelectorUI) X(XamShowNuiDirtyDiscErrorUI)        \
  X(XamShowNuiGuideUI) X(XamShowNuiMessageBoxUI) X(XamShowNuiSigninUI) X(XamShowSigninUIEx)                            \
  X(XamLoaderGetMediaInfo) X(XamLoaderGetMediaInfoEx) X(XamUpdateGetBaseSystemVersion)                                 \
  X(XamUpdateGetCurrentSystemVersion) X(XexLoadExecutable)                                                             \
  X(XamInputNonControllerGetRaw) X(XamInputNonControllerGetRawEx) X(XamInputNonControllerSetRaw)                       \
  X(XamInputNonControllerSetRawEx) X(NetDll_XHttpOpen) X(NetDll_XHttpStartup)                                          \
  X(XamNetworkStorageHasUserEnabledStorage) X(XNotifyBroadcast) X(XNotifyQueueUI)                                      \
  X(XAutomationpUnbindController) X(XamVoiceGetMicArrayStatus) X(MicDeviceRequest) X(HalOpenCloseODDTray)              \
  X(HalSendSMCMessage) X(NtCancelIoFile) X(RtlRip)
STEP12_CALLS(REFERENCE)

namespace {

using any360::sys::AllocCounts;
using any360::sys::ThreadAllocations;

// Guest layout: a PCR whose +0x100 is the current KTHREAD, buffers, a stack.
constexpr uint32_t kPcr = 0x40000000, kThread = 0x40001000, kIn = 0x40010000, kOut = 0x40020000, kState = 0x40030000,
                   kKey = 0x40031000, kFeed = 0x40031100, kText = 0x40032000, kStack = 0x40100000, kStackSize = 0x10000,
                   kHeap = 0x41000000, kImage = 0x40300000;
constexpr uint32_t kBytes = 256;  // the buffer size of the hashes and ciphers

uint8_t* g_base = nullptr;
uint32_t g_repeats = 9;
uint64_t g_calls = 0, g_bytes = 0, g_count = 0;

void Store32(uint32_t at, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(g_base + at, &v, 4);
}
void Store16(uint32_t at, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(g_base + at, &v, 2);
}

Function Native(const char* name) {
  for (unsigned i = 0; i < any360::sys::kExportCount; ++i) {
    if (!std::strcmp(any360::sys::kExports[i].name, name)) return any360::sys::kExports[i].function;
  }
  std::printf("sys_abi_bench: %s is not in any360::sys::kExports\n", name);
  std::exit(2);
}

// The registers of one call: r3..r10, then the stack slots of the 9th and 10th arguments.
struct Arguments {
  uint64_t r[8] = {};
  uint64_t stack[2] = {};
};

struct Measure {
  double ns = 0, bytes = 0, allocations = 0;
};

Measure Time(Function f, const Arguments& args, uint32_t n, bool count) {
  Measure m{1e30, 0, 0};
  uint64_t calls = 0, bytes = 0, allocs = 0;
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  for (uint32_t r = 0; r < g_repeats; ++r) {
    const AllocCounts before = ThreadAllocations();
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < n; ++i) {
      PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
      for (int k = 0; k < 8; ++k) regs[k]->u64 = args.r[k];
      ctx.r1.u64 = kStack + kStackSize - 0x1000;
      ctx.r13.u64 = kPcr;
      ctx.lr = 0x82000000;
      f(ctx, g_base);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
    const AllocCounts spent = ThreadAllocations() - before;
    calls += n, bytes += spent.bytes, allocs += spent.count;
    m.ns = std::min(m.ns, ns);
  }
  m.bytes = double(bytes) / double(calls), m.allocations = double(allocs) / double(calls);
  if (count) g_calls += calls, g_bytes += bytes, g_count += allocs;
  return m;
}

struct Row {
  const char* name;
  Function reference;
  Arguments args;
  uint32_t calls = 20000;  // a run's calls (fewer for a call that leaves an object behind each time)
  Function native = nullptr;  // Any360's side when it is not the export of the row's name alone (a pair, a stream)
  int64_t expect = -1;        // when set, the r3 Any360's side must return (checked once after the timing)
};

uint32_t Load32(uint32_t at) {
  uint32_t v;
  std::memcpy(&v, g_base + at, 4);
  return __builtin_bswap32(v);
}

// XamAllocEx on Any360's side with the XamFree that returns its pages (the reference's stub allocates nothing).
Function g_alloc_ex = nullptr, g_free = nullptr;
constexpr uint32_t kAllocOut = 0x40020D00;
void AllocExAndFree(PPCContext& ctx, uint8_t* base) {
  g_alloc_ex(ctx, base);
  ctx.r3.u64 = Load32(kAllocOut);
  g_free(ctx, base);
}

// LDI on scratch memory: Create, then each chunk of a stream (none for the Create + Destroy row), then Destroy, each
// side through its own three entry points.
struct LdiSide {
  Function create, decompress, destroy;
};
LdiSide g_ldi_reference, g_ldi_native;
std::vector<uint32_t> g_chunk_at, g_chunk_size, g_chunk_frame;
size_t g_chunks = 0;  // chunks a call decodes
constexpr uint32_t kLdiIo = 0x40040000, kLdiScratch = 0x40041000, kLdiOut = 0x40050000;
void LdiStream(const LdiSide& side, PPCContext& ctx, uint8_t* base) {
  Store32(kLdiIo + 16, 0);
  ctx.r3.u64 = kLdiIo, ctx.r4.u64 = kLdiIo + 8, ctx.r5.u64 = 0, ctx.r6.u64 = 0, ctx.r7.u64 = kLdiScratch, ctx.r8.u64 = 0;
  ctx.r9.u64 = kLdiIo + 16;
  side.create(ctx, base);
  const uint32_t handle = Load32(kLdiIo + 16);
  for (size_t i = 0; i < g_chunks; ++i) {
    Store32(kLdiIo + 24, g_chunk_frame[i]);
    ctx.r3.u64 = handle, ctx.r4.u64 = g_chunk_at[i], ctx.r5.u64 = g_chunk_size[i], ctx.r6.u64 = kLdiOut;
    ctx.r7.u64 = kLdiIo + 24;
    side.decompress(ctx, base);
  }
  ctx.r3.u64 = handle;
  side.destroy(ctx, base);
}
void LdiReference(PPCContext& ctx, uint8_t* base) { LdiStream(g_ldi_reference, ctx, base); }
void LdiNative(PPCContext& ctx, uint8_t* base) { LdiStream(g_ldi_native, ctx, base); }

// XamContentCreateEnumeratorInternal on Any360's side with the NtClose that releases its enumerator (the reference's
// stub creates none).
Function g_enumerator_internal = nullptr;
constexpr uint32_t kEnumeratorOut = 0x40020E00;
// The guest code ObCreateObject calls (an object type's allocate procedure), played by the host: the same block each
// time (an allocator a title would free into), so the row times the kernel's part.
constexpr uint32_t kObjectBlock = 0x4002D000;
uint64_t AllocateProcedure(uint32_t, uint32_t, uint32_t, const uint64_t*, uint32_t, void*) { return kObjectBlock; }

// XamUserCreateTitlesPlayedEnumerator on Any360's side with the NtClose that releases its enumerator.
Function g_titles_played = nullptr;
void TitlesPlayedAndClose(PPCContext& ctx, uint8_t* base) {
  g_titles_played(ctx, base);
  any360::sys::Objects().Close(Load32(kEnumeratorOut));
}
// XamProfileCreateEnumerator on Any360's side with the NtClose that releases its enumerator (left open, the
// enumerators piled up would be searched by every later enumerator row).
Function g_profile_enumerator = nullptr;
void ProfileEnumeratorAndClose(PPCContext& ctx, uint8_t* base) {
  g_profile_enumerator(ctx, base);
  any360::sys::Objects().Close(Load32(kEnumeratorOut));
}
// XamProfileOpen then XamProfileClose of the same name (the mount made and removed each time), on either side.
Function g_profile_open[2], g_profile_close[2];
uint64_t g_profile_xuid = 0;
constexpr uint32_t kProfileName = 0x4002CA00;
void ProfileOpenClose(int side, PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = g_profile_xuid, ctx.r4.u64 = kProfileName, ctx.r5.u64 = 0, ctx.r6.u64 = 0;
  g_profile_open[side](ctx, base);
  ctx.r3.u64 = kProfileName;
  g_profile_close[side](ctx, base);
}
void ProfileOpenCloseReference(PPCContext& ctx, uint8_t* base) { ProfileOpenClose(0, ctx, base); }
void ProfileOpenCloseNative(PPCContext& ctx, uint8_t* base) { ProfileOpenClose(1, ctx, base); }

void EnumeratorAndClose(PPCContext& ctx, uint8_t* base) {
  g_enumerator_internal(ctx, base);
  any360::sys::Objects().Close(Load32(kEnumeratorOut));
}

Arguments Args(std::initializer_list<uint64_t> regs, std::initializer_list<uint64_t> stack = {}) {
  Arguments a;
  int i = 0;
  for (uint64_t v : regs) a.r[i++] = v;
  i = 0;
  for (uint64_t v : stack) a.stack[i++] = v;
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_repeats = std::max(1, std::atoi(argv[1]));
  g_base = any360::test::GuestMemory::Get().base();
  // Touch every page used, then fill the inputs.
  std::memset(g_base + kPcr, 0, 0x2000);
  std::memset(g_base + kIn, 0, 0x30000);
  std::memset(g_base + kStack, 0, kStackSize);
  std::memset(g_base + kHeap, 0, 0x4000000);
  Store32(kPcr + 0x100, kThread);
  for (uint32_t i = 0; i < 0x10000; ++i) g_base[kIn + i] = uint8_t(i * 131 + 7);
  std::memset(g_base + kKey, 0x0b, 64);
  for (uint32_t i = 0; i < 64; ++i) Store16(kText + 2 * i, uint16_t('a' + i % 26));
  Store16(kText + 128, 0);
  std::memcpy(g_base + kText + 0x200, "xam.xex", 8);
  any360::sys::Objects().SetKernelHeap(kHeap, 0x4000000);
  any360::sys::Objects().SetGuest({g_base});
  any360::sys::Objects().Reset();
  any360::sys::XamState().title().title_name = u"Any360 benchmark title";
  // The PCR's stack, and a small PE32 image (little-endian fields) whose directory entry 2 lies in its second section.
  Store32(kPcr + 0x70, kStack + kStackSize), Store32(kPcr + 0x74, kStack);
  std::memset(g_base + kImage, 0, 0x400);
  auto le = [](uint32_t at, uint32_t v, int bytes) { for (int i = 0; i < bytes; ++i) g_base[at + i] = uint8_t(v >> (8 * i)); };
  g_base[kImage] = 'M', g_base[kImage + 1] = 'Z', le(kImage + 0x3C, 0x80, 4);
  const uint32_t nt = kImage + 0x80, optional = nt + 24;
  g_base[nt] = 'P', g_base[nt + 1] = 'E', le(nt + 6, 2, 2), le(nt + 20, 0x80, 2), le(optional, 0x10B, 2);
  le(optional + 60, 0x200, 4), le(optional + 92, 4, 4), le(optional + 96 + 16, 0x3010, 4), le(optional + 100 + 16, 0x40, 4);
  le(optional + 0x80 + 12, 0x1000, 4), le(optional + 0x80 + 16, 0x1000, 4);
  le(optional + 0x80 + 52, 0x3000, 4), le(optional + 0x80 + 56, 0x800, 4);
  Store32(kOut + 0x200, 12345);
  // The profile rows: an enumerator already at its end, a mount name, the local profile's XUID.
  any360::sys::XamState().title().content_root = std::filesystem::temp_directory_path() / "any360-abi-bench";
  any360::sys::XamState().ProfileCreateEnumerator({g_base}, kOut + 0x300);
  const uint32_t profiles = __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + kOut + 0x300));
  any360::sys::XamState().ProfileEnumerate({g_base}, profiles, kOut + 0x400, 0);
  std::memcpy(g_base + kText + 0x300, "PROFILE", 8);
  const uint64_t xuid = any360::sys::XamState().title().xuid;

  // The eighth batch: guest heaps for XamAllocEx, an LZX stream for LDI (1 MiB of text and x86-like code compressed by
  // the host's FCI with a 2^17 window: 32 chunks; Windows only).
  any360::sys::Memory().Reset({{0x48000000, 0x1000000, 0x1000, false}, {0x49000000, 0x1000000, 0x1000, true}});
  g_alloc_ex = Native("XamAllocEx"), g_free = Native("XamFree");
  g_ldi_reference = {__imp__LDICreateDecompression, __imp__LDIDecompress, __imp__LDIDestroyDecompression};
  g_ldi_native = {Native("LDICreateDecompression"), Native("LDIDecompress"), Native("LDIDestroyDecompression")};
  Store32(kLdiIo, 0x8000);
  Store32(kLdiIo + 8, 0x20000);
  size_t stream_chunks = 0;
#if defined(_WIN32)
  {
    std::vector<uint8_t> data;
    uint32_t x = 12345;
    auto next = [&] { return x = x * 1664525u + 1013904223u; };
    static const char* kWords[] = {"the ", "kernel ", "frame ", "window ", "of ", "LZX ", "decoder ", "block ", "\n",
                                   "tree ", "match ", "offset ", "length ", "and ", "a ", "title "};
    while (data.size() < (1u << 20)) {
      if ((next() >> 20) % 3) {
        const char* w = kWords[(next() >> 16) % 16];
        data.insert(data.end(), w, w + std::strlen(w));
      } else {
        const uint32_t target = (next() >> 8) % 0x60000;
        const uint8_t call[] = {0xE8, uint8_t(target), uint8_t(target >> 8), uint8_t(target >> 16), 0, 0x8B, 0x45, 0xF8};
        data.insert(data.end(), call, call + 8);
      }
    }
    data.resize(1u << 20);
    uint32_t at = 0x40100000 + kStackSize;
    for (const auto& c : any360::test::cab::CabChunks(data, 17)) {
      g_chunk_at.push_back(at), g_chunk_size.push_back(uint32_t(c.data.size())), g_chunk_frame.push_back(c.frame);
      std::memcpy(g_base + at, c.data.data(), c.data.size());
      at += (uint32_t(c.data.size()) + 15) & ~15u;
    }
    stream_chunks = g_chunk_at.size();
  }
#endif

  // The ninth batch: two content packages of type 1 (saved games), an XCONTENT_DATA_INTERNAL naming the first, a size
  // for XamReadString.
  g_enumerator_internal = Native("XamContentCreateEnumeratorInternal");
  g_titles_played = Native("XamUserCreateTitlesPlayedEnumerator");
  constexpr uint32_t kRawLength = 0x40047200;  // a raw report's length for the non-controller rows
  Store32(kRawLength, 0x20);
  constexpr uint32_t kMicDevice = 0x40047300, kSmcMessage = 0x40047400;  // an X_MIC_DEVICE (a status request), SMC
  std::memset(g_base + kMicDevice, 0, 0x40);
  std::memset(g_base + kSmcMessage, 0, 0x20);
  g_base[kSmcMessage] = 0x07;
  // A video flags value for ExSetXConfigSetting (the default), an LDI context on scratch memory for its reset.
  constexpr uint32_t kConfigValue = 0x40047100, kResetIo = 0x40047000, kResetScratch = 0x40044000;
  Store32(kConfigValue, 0x00040000);
  Store32(kResetIo, 0x8000), Store32(kResetIo + 4, 0x8000);
  any360::sys::Ldi().Create({g_base}, kResetIo, kResetIo + 4, 0, 0, kResetScratch, kResetIo + 8, kResetIo + 12, nullptr,
                            nullptr);
  g_profile_enumerator = Native("XamProfileCreateEnumerator");
  g_profile_open[0] = __imp__XamProfileOpen, g_profile_open[1] = Native("XamProfileOpen");
  g_profile_close[0] = __imp__XamProfileClose, g_profile_close[1] = Native("XamProfileClose");
  g_profile_xuid = xuid;
  std::memcpy(g_base + kProfileName, "BENCHPROF", 10);
  std::memcpy(g_base + kProfileName + 0x20, "NOTMOUNTED", 11);
  constexpr uint32_t kContentData = 0x4002C000, kMetadata = 0x40021000, kWsaData = 0x4002B000;  // 0x134, 0x93D6, 0x190 bytes
  {
    // Two packages made as a title makes them (XamContentCreate: the folder and its header), closed again.
    std::error_code error;
    std::filesystem::remove_all(any360::sys::XamState().title().content_root / "00000001", error);
    std::filesystem::remove_all(any360::sys::XamState().title().content_root / "Headers" / "00000001", error);
    std::memcpy(g_base + kContentData + 0x180, "bench", 6);
    for (const char* name : {"slotB", "slotA"}) {
      std::memset(g_base + kContentData, 0, 0x180);
      Store32(kContentData, 1), Store32(kContentData + 4, 1);
      for (uint32_t i = 0; i < 6; ++i) Store16(kContentData + 8 + 2 * i, uint16_t(u"Slot A"[i]));
      std::memcpy(g_base + kContentData + 0x108, name, 6);
      any360::sys::XamState().ContentCreate({g_base}, 0, kContentData + 0x180, kContentData, 2, 0, 0, 0);
      any360::sys::XamState().ContentClose({g_base}, kContentData + 0x180, 0);
    }
  }
  Store32(kOut + 0xD90, 16);

  // The tenth batch: an object type whose allocate procedure the dispatcher plays, attributes naming an object, a
  // module name no title loaded.
  constexpr uint32_t kObjectType = 0x4002C800, kObjectAttributes = 0x4002C840, kObjectName = 0x4002C860;
  std::memset(g_base + kObjectType, 0, 0x100);
  Store32(kObjectType, 0x82001000), Store32(kObjectType + 0x18, 0x54657374);
  Store32(kObjectAttributes + 4, kObjectName);
  Store16(kObjectName, 13), Store16(kObjectName + 2, 13), Store32(kObjectName + 4, kObjectName + 8);
  std::memcpy(g_base + kObjectName + 8, "\\Title\\Event7", 13);
  std::memcpy(g_base + kObjectName + 0x40, "dlc.xex", 8);
  any360::sys::Threads().SetDispatcher(&AllocateProcedure, nullptr);
  // Media: an X_SECURITY_FAILURE_INFORMATION (its size first) and XAUTH_SETTINGS of size 8.
  constexpr uint32_t kMediaInfo = 0x4002C900;
  std::memset(g_base + kMediaInfo, 0, 0x40);
  Store32(kMediaInfo, 0x18), Store32(kMediaInfo + 0x20, 8);

  const uint32_t stack_args = kStack + kStackSize - 0x1000 + 0x54;
  const Row rows[] = {
      {"XeCryptMd5", __imp__XeCryptMd5, Args({kIn, kBytes, 0, 0, 0, 0, kOut, 16})},
      {"XeCryptSha256", __imp__XeCryptSha256, Args({kIn, kBytes, 0, 0, 0, 0, kOut, 32})},
      {"XeCryptMd5Init", __imp__XeCryptMd5Init, Args({kState})},
      {"XeCryptMd5Update", __imp__XeCryptMd5Update, Args({kState, kIn, kBytes})},
      {"XeCryptMd5Final", __imp__XeCryptMd5Final, Args({kState, kOut, 16})},
      {"XeCryptHmacMd5", __imp__XeCryptHmacMd5, Args({kKey, 16, kIn, kBytes, 0, 0, 0, 0}, {kOut, 16})},
      {"XeCryptHmacShaInit", __imp__XeCryptHmacShaInit, Args({kState, kKey, 20})},
      {"XeCryptHmacShaUpdate", __imp__XeCryptHmacShaUpdate, Args({kState, kIn, kBytes})},
      {"XeCryptHmacShaFinal", __imp__XeCryptHmacShaFinal, Args({kState, kOut, 20})},
      {"XeCryptHmacShaVerify", __imp__XeCryptHmacShaVerify, Args({kKey, 20, kIn, kBytes, 0, 0, 0, 0}, {kOut + 0x80, 20})},
      {"XeCryptDesKey", __imp__XeCryptDesKey, Args({kState, kKey})},
      {"XeCryptDesEcb", __imp__XeCryptDesEcb, Args({kState, kIn, kOut, 1})},
      {"XeCryptDesCbc", __imp__XeCryptDesCbc, Args({kState, kIn, kBytes, kOut, kFeed, 1})},
      {"GetLastError", __imp__GetLastError, Args({})},
      {"GetCurrentThreadId", __imp__GetCurrentThreadId, Args({})},
      {"GetTickCount", __imp__GetTickCount, Args({})},
      {"QueryPerformanceFrequency", __imp__QueryPerformanceFrequency, Args({kOut})},
      {"GetSystemTimeAsFileTime", __imp__GetSystemTimeAsFileTime, Args({kOut})},
      {"lstrlenW", __imp__lstrlenW, Args({kText})},
      {"GetModuleHandleA", __imp__GetModuleHandleA, Args({kText + 0x200})},
      {"XGetAudioFlags", __imp__XGetAudioFlags, Args({})},
      {"XGetOverlappedExtendedError", __imp__XGetOverlappedExtendedError, Args({kFeed})},
      {"XamGetActiveDashAppInfo", __imp__XamGetActiveDashAppInfo, Args({kOut})},
      {"XamSetActiveDashAppInfo", __imp__XamSetActiveDashAppInfo, Args({kIn})},
      {"XamGetCachedTitleName", __imp__XamGetCachedTitleName, Args({0x4D5308BC, kOut, kOut + 0x100})},
      {"XamGetDashBackstackData", __imp__XamGetDashBackstackData, Args({kOut, kOut + 0x400})},
      {"XamGetDashContext", __imp__XamGetDashContext, Args({})},
      {"XamGetLanguage", __imp__XamGetLanguage, Args({})},
      {"XamGetLanguageLocaleFallbackString", __imp__XamGetLanguageLocaleFallbackString, Args({2})},
      {"XamGetLanguageTypeface", __imp__XamGetLanguageTypeface, Args({1, 0x100, kOut})},
      {"XamGetOnlineCountryFeatures", __imp__XamGetOnlineCountryFeatures, Args({103})},
      {"XamGetOnlineLanguageAndCountry", __imp__XamGetOnlineLanguageAndCountry, Args({0xE000000000000001ull, kOut, kOut + 1})},
      {"XamIsChildAccountSignedIn", __imp__XamIsChildAccountSignedIn, Args({})},
      {"XamIsCurrentTitleDash", __imp__XamIsCurrentTitleDash, Args({})},
      {"XamIsIptvEnabled", __imp__XamIsIptvEnabled, Args({})},
      {"XamIsNuiUIActive", __imp__XamIsNuiUIActive, Args({})},
      {"XamIsSystemExperienceTitleId", __imp__XamIsSystemExperienceTitleId, Args({0x58480001})},
      {"XamIsSystemTitleId", __imp__XamIsSystemTitleId, Args({0x4D5308BC})},
      {"XamIsXbox1TitleId", __imp__XamIsXbox1TitleId, Args({0x41430006})},
      {"RtlGetLastError", __imp__RtlGetLastError, Args({})},
      {"RtlSetLastNTError", __imp__RtlSetLastNTError, Args({0xC0000008})},
      {"RtlRandom", __imp__RtlRandom, Args({kOut + 0x200})},
      {"RtlDowncaseUnicodeChar", __imp__RtlDowncaseUnicodeChar, Args({0xC9})},
      {"RtlGetStackLimits", __imp__RtlGetStackLimits, Args({kOut, kOut + 4})},
      {"RtlImageDirectoryEntryToData (file)", __imp__RtlImageDirectoryEntryToData, Args({kImage, 0, 2, kOut})},
      {"KeQueryInterruptTime", __imp__KeQueryInterruptTime, Args({})},
      {"KeSetPriorityThread", __imp__KeSetPriorityThread, Args({kThread, 8})},
      {"HalGetCurrentAVPack", __imp__HalGetCurrentAVPack, Args({})},
      {"XamGetLocaleDateFormat", __imp__XamGetLocaleDateFormat, Args({36})},
      {"XamNuiHudGetEngagedTrackingID", __imp__XamNuiHudGetEngagedTrackingID, Args({})},
      {"XamNuiHudGetInitializeFlags", __imp__XamNuiHudGetInitializeFlags, Args({})},
      {"XamNuiHudGetVersions", __imp__XamNuiHudGetVersions, Args({kOut, kOut + 8})},
      {"XamNuiHudIsEnabled", __imp__XamNuiHudIsEnabled, Args({})},
      {"XamNuiHudSetEngagedTrackingID", __imp__XamNuiHudSetEngagedTrackingID, Args({0})},
      {"XamNuiIdentityGetSessionId", __imp__XamNuiIdentityGetSessionId, Args({})},
      {"XamNuiIsChatMicEnabled", __imp__XamNuiIsChatMicEnabled, Args({})},
      {"XamNuiIsDeviceReady", __imp__XamNuiIsDeviceReady, Args({})},
      {"XamSetDashContext", __imp__XamSetDashContext, Args({7})},
      {"XamShowCreateProfileUI (no listener)", __imp__XamShowCreateProfileUI, Args({0, 0})},
      {"XamShowCreateProfileUIEx (no listener)", __imp__XamShowCreateProfileUIEx, Args({0, 0, 0})},
      {"XamShowEditProfileUI (no listener)", __imp__XamShowEditProfileUI, Args({0})},
      {"XamShowForcedNameChangeUI (no listener)", __imp__XamShowForcedNameChangeUI, Args({0})},
      {"XamShowGamerCardUI (no listener)", __imp__XamShowGamerCardUI, Args({0})},
      {"XamShowMultiplayerUpgradeUI", __imp__XamShowMultiplayerUpgradeUI, Args({0})},
      {"XamShowNuiHardwareRequiredUI", __imp__XamShowNuiHardwareRequiredUI, Args({0})},
      {"XamProfileCreateEnumerator (+ NtClose)", __imp__XamProfileCreateEnumerator, Args({1, kEnumeratorOut}), 2000,
       ProfileEnumeratorAndClose, 0},
      {"XamProfileEnumerate (at the end)", __imp__XamProfileEnumerate, Args({profiles, 0, kOut + 0x400, 0})},
      {"XamProfileFindAccount", __imp__XamProfileFindAccount, Args({xuid, kOut + 0x600, kOut + 0x7F0})},
      {"XamProfileGetLiveLegalLocale", __imp__XamProfileGetLiveLegalLocale, Args({xuid, 16, kOut + 0x800})},
      {"XamProfileOpen (mounted again)", __imp__XamProfileOpen, Args({xuid, kText + 0x300, 0, kOut + 0x900})},
      {"XamUserGetCachedUserFlags", __imp__XamUserGetCachedUserFlags, Args({0})},
      {"XamUserGetIndexFromXUID", __imp__XamUserGetIndexFromXUID, Args({xuid, 0, kOut})},
      {"XamUserGetOnlineLanguageFromXUID", __imp__XamUserGetOnlineLanguageFromXUID, Args({xuid})},
      {"XamUserGetSubscriptionType", __imp__XamUserGetSubscriptionType, Args({0, kOut, kOut + 4})},
      {"XamUserGetUserFlags", __imp__XamUserGetUserFlags, Args({0})},
      {"XamUserGetUserFlagsFromXUID", __imp__XamUserGetUserFlagsFromXUID, Args({xuid})},
      {"XamUserGetUserTenure", __imp__XamUserGetUserTenure, Args({0, kOut, kOut + 4, kOut + 8, 0})},
      {"XamUserIsParentalControlled", __imp__XamUserIsParentalControlled, Args({0})},
      {"XamUserLogon", __imp__XamUserLogon, Args({kOut + 0x300, 1, 0})},
      {"XamUserNuiGetUserIndexForSignin", __imp__XamUserNuiGetUserIndexForSignin, Args({kOut})},
      {"XamUserReadUserPreference", __imp__XamUserReadUserPreference, Args({0, 0x020, kOut, 0})},
      {"XamUserWriteUserPreference", __imp__XamUserWriteUserPreference, Args({0, 0x020, 0x020, 0})},
      // The seventh batch: SHA-512 (a state, 256 bytes in), 1024-bit numbers (16 words), a 256-byte signature.
      {"XeCryptSha512Init", __imp__XeCryptSha512Init, Args({kOut + 0xA00})},
      {"XeCryptSha512Update (256 B)", __imp__XeCryptSha512Update, Args({kOut + 0xA00, kText, 256})},
      {"XeCryptSha512Final", __imp__XeCryptSha512Final, Args({kOut + 0xA00, kOut + 0xB00, 64})},
      {"XeCryptBnQw_Copy (16 words)", __imp__XeCryptBnQw_Copy, Args({kText, kOut + 0xC00, 16})},
      {"XeCryptBnQwNeModInv", __imp__XeCryptBnQwNeModInv, Args({0x123456789ABCDEF1ull})},
      {"XeCryptBnQwNeModMul (16 words)", __imp__XeCryptBnQwNeModMul,
       Args({kText, kText + 0x80, kOut + 0xC00, any360::sys::XeCryptBnQwNeModInv(0xFF51AFD7ED558CCDull), kText + 0x100, 16})},
      {"XeCryptBnQwBeSigFormat", __imp__XeCryptBnQwBeSigFormat, Args({kOut + 0xC00, kText, kText + 0x40})},
      {"XeCryptBnDwLePkcs1Format (256 B)", __imp__XeCryptBnDwLePkcs1Format, Args({kText, 0, kOut + 0xC00, 256})},
      {"XeKeysGetConsoleType", __imp__XeKeysGetConsoleType, Args({kOut})},
      {"XeKeysGetConsoleID", __imp__XeKeysGetConsoleID, Args({kOut, kOut + 16})},
      {"XeKeysGetKeyProperties", __imp__XeKeysGetKeyProperties, Args({0x1B})},
      // The eighth batch.
      {"XTLGetLanguageV2", __imp__XTLGetLanguageV2, Args({})},
      {"XapipGetLocale", __imp__XapipGetLocale, Args({16, kOut + 0xD80})},
      {"XapiFormatTimeOut", __imp__XapiFormatTimeOut, Args({kOut + 0xD80, 1500})},
      {"XamIptvGetServiceName", __imp__XamIptvGetServiceName, Args({kOut + 0xD80})},
      {"XamAllocEx (physical 9 KB, + XamFree)", __imp__XamAllocEx, Args({0x20000000, 0x40000000, 0x2345, kAllocOut}), 20000,
       AllocExAndFree},
      {"XamAllocEx (virtual 9 KB, + XamFree)", __imp__XamAllocEx, Args({0, 0, 0x2345, kAllocOut}), 20000, AllocExAndFree},
      {"LDICreateDecompression + Destroy", LdiReference, Args({}), 20000, LdiNative},
      {"LDIDecompress (stream: 32 chunks, 1 MiB)", LdiReference, Args({}), 20, LdiNative},
      // The ninth batch.
      {"XamContentCreateEnumeratorInternal (2, + NtClose)", __imp__XamContentCreateEnumeratorInternal,
       Args({0, 1, 1, 0, 0, 2, kEnumeratorOut + 4, kEnumeratorOut}), 2000, EnumeratorAndClose, 0},
      {"XamContentGetMetaDataInternal", __imp__XamContentGetMetaDataInternal, Args({kContentData, kMetadata, 0}), 2000,
       nullptr, 0},
      {"XamContentSetThumbnailInternal (4 B)", __imp__XamContentSetThumbnailInternal, Args({kContentData, kText, 4}), 2000,
       nullptr, 0},
      {"XamContentRegisterChangeCallback", __imp__XamContentRegisterChangeCallback, Args({0x82001234})},
      {"XamLoaderRegisterLaunchRequestCallback", __imp__XamLoaderRegisterLaunchRequestCallback, Args({0x82005678})},
      {"XamReadString", __imp__XamReadString, Args({0x4D5307E6, 5, 0, kOut + 0xDA0, kOut + 0xD90, 0}), 20000, nullptr, 0},
      {"NetDll_WSAStartupEx", __imp__NetDll_WSAStartupEx, Args({0, 0x0202, kWsaData, 0x0202}), 20000, nullptr, 0},
      {"NetDll_XNetStartupEx", __imp__NetDll_XNetStartupEx, Args({0, kText, 0x20501400}), 20000, nullptr, 0},
      {"XamNetworkStorageUserHasPrivilege", __imp__XamNetworkStorageUserHasPrivilege, Args({0}), 20000, nullptr, 0},
      {"ObLookupAnyThreadByThreadId (unknown id)", __imp__ObLookupAnyThreadByThreadId, Args({0x7777, kOut + 0xDB0}), 20000,
       nullptr, 0xC0000225},
      // The tenth batch.
      {"ObCreateObject (unnamed, 0x40 B)", __imp__ObCreateObject, Args({kObjectType, 0, 0x40, kOut + 0xDC0}), 20000, nullptr, 0},
      {"ObCreateObject (named, 0x40 B)", __imp__ObCreateObject, Args({kObjectType, kObjectAttributes, 0x40, kOut + 0xDC0}),
       20000, nullptr, 0},
      {"XexLoadImageFromMemory (not loaded)", __imp__XexLoadImageFromMemory,
       Args({kIn, 0x1000, kObjectName + 0x40, 0, 0, kOut + 0xDC4}), 20000, nullptr, 0xC000000F},
      // The avatar calls Xenia stubs too (no overlapped: their synchronous answer).
      {"XamAvatarBeginEnumAssets", __imp__XamAvatarBeginEnumAssets, Args({0, 0, 0, 0, 0, 0}), 20000, nullptr, 0},
      {"XamAvatarEndEnumAssets", __imp__XamAvatarEndEnumAssets, Args({0}), 20000, nullptr, 0},
      {"XamAvatarEnumAssets", __imp__XamAvatarEnumAssets, Args({kIn, kIn + 8, 0}), 20000, nullptr, 0x80070012},
      {"XamAvatarGenerateMipMaps", __imp__XamAvatarGenerateMipMaps, Args({kIn, 0, 0, kIn, 0}), 20000, nullptr, 0},
      {"XamAvatarGetAssetBinary", __imp__XamAvatarGetAssetBinary, Args({kIn, 0, 0, 0, 0}), 20000, nullptr, 0},
      {"XamAvatarGetAssetIcon", __imp__XamAvatarGetAssetIcon, Args({kIn, 0, kIn, kIn, 0}), 20000, nullptr, 0},
      {"XamAvatarGetAssets", __imp__XamAvatarGetAssets, Args({kIn, 0xFF, 0, kOut + 0xDD0, kOut + 0xDD4, 0}), 20000,
       nullptr, 0},
      {"XamAvatarGetAssetsResultSize", __imp__XamAvatarGetAssetsResultSize, Args({0xFF, kOut + 0xDD0, kOut + 0xDD4}),
       20000, nullptr, 0},
      {"XamAvatarGetInstalledAssetPackageDescription", __imp__XamAvatarGetInstalledAssetPackageDescription,
       Args({kIn, kOut + 0x1E00}), 20000, nullptr, -1},
      {"XamAvatarGetInstrumentation", __imp__XamAvatarGetInstrumentation, Args({6, kOut + 0xDD0}), 20000, nullptr, 1},
      {"XamAvatarSetManifest (in memory)", __imp__XamAvatarSetManifest, Args({0, kIn, 0}), 2000, nullptr, 0},
      {"XamAvatarGetManifestLocalUser", __imp__XamAvatarGetManifestLocalUser, Args({0, kOut + 0x1A00, 0}), 20000,
       nullptr, 0},
      {"XamAvatarGetManifestsByXuid", __imp__XamAvatarGetManifestsByXuid, Args({0, 1, kIn, 0, kOut + 0x1A00, 0}), 20000,
       nullptr, 0},
      {"XamAvatarGetMetadataRandom", __imp__XamAvatarGetMetadataRandom, Args({1, 1, kOut + 0x1A00, 0}), 20000, nullptr, 0},
      {"XamAvatarGetMetadataSignedOutProfile", __imp__XamAvatarGetMetadataSignedOutProfile, Args({0, kOut + 0x1A00, 0}),
       20000, nullptr, 0},
      {"XamAvatarGetMetadataSignedOutProfileCount", __imp__XamAvatarGetMetadataSignedOutProfileCount,
       Args({kOut + 0xDD0, 0}), 20000, nullptr, 0},
      {"XamAvatarLoadAnimation", __imp__XamAvatarLoadAnimation, Args({kIn, 0, kOut + 0x1A00, 0}), 20000, nullptr, 0},
      {"XamAvatarManifestGetBodyType", __imp__XamAvatarManifestGetBodyType, Args({kIn}), 20000, nullptr, 1},
      {"XamAvatarReinstallAwardedAsset", __imp__XamAvatarReinstallAwardedAsset, Args({kOut + 0x1A00, 16, kOut + 0xDD0}),
       20000, nullptr, 0},
      {"XamAvatarSetCustomAsset", __imp__XamAvatarSetCustomAsset, Args({16, kIn, 0, 0, kIn}), 20000, nullptr, 0},
      {"XamAvatarSetMocks", __imp__XamAvatarSetMocks, Args({}), 20000, nullptr, -1},
      {"XamAvatarWearNow", __imp__XamAvatarWearNow, Args({0, kIn, 0}), 20000, nullptr, 0},
      // Media and the dashboard's services.
      {"EnumerateMediaObjects", __imp__EnumerateMediaObjects, Args({kIn, 1, kOut + 0xDD0}), 20000, nullptr, 0x80004001},
      {"EnumerateMediaObjects_", __imp__EnumerateMediaObjects_, Args({kIn, 1, kOut + 0xDD0}), 20000, nullptr, 0x80004001},
      {"EnumerateMediaObjects_0", __imp__EnumerateMediaObjects_0, Args({kIn, 1, kOut + 0xDD0}), 20000, nullptr, 0x80004001},
      {"EnumerateMediaObjects_1", __imp__EnumerateMediaObjects_1, Args({kIn, 1, kOut + 0xDD0}), 20000, nullptr, 0x80004001},
      {"XamMediaVerificationCreate", __imp__XamMediaVerificationCreate, Args({1}), 20000, nullptr, 0},
      {"XamMediaVerificationClose", __imp__XamMediaVerificationClose, Args({}), 20000, nullptr, 0},
      {"XamMediaVerificationInject", __imp__XamMediaVerificationInject, Args({1, 2}), 20000, nullptr, 0},
      {"XamMediaVerificationVerify (no callback)", __imp__XamMediaVerificationVerify, Args({0, 0, 0}), 20000, nullptr, 0},
      {"XamMediaVerificationFailedBlocks", __imp__XamMediaVerificationFailedBlocks, Args({kMediaInfo}), 20000, nullptr, 0},
      {"XampWebInstrumentationSetProfileCounts", __imp__XampWebInstrumentationSetProfileCounts, Args({0, 1, 1, 0}), 20000,
       nullptr, -1},
      {"XampXAuthStartup", __imp__XampXAuthStartup, Args({kMediaInfo + 0x20}), 20000, nullptr, 0x80158406},
      {"XampXAuthShutdown", __imp__XampXAuthShutdown, Args({kOut + 0xDD0}), 20000, nullptr, -1},
      {"XampXAuthGetTitleBuffer", __imp__XampXAuthGetTitleBuffer, Args({}), 20000, nullptr, 0},
      // The profile and user calls.
      {"XamProfileClose (not mounted)", __imp__XamProfileClose, Args({kProfileName + 0x20}), 20000, nullptr, 0x65B},
      {"XamProfileOpen + XamProfileClose (mounted, unmounted)", ProfileOpenCloseReference, Args({}), 2000,
       ProfileOpenCloseNative, 0},
      {"XamProfileCreate", __imp__XamProfileCreate, Args({0, kOut + 0xDD0, 0, kIn, 0, 0, 0, kOut + 0xDD4}), 20000, nullptr,
       87},
      {"XamProfileGetCreationStatus", __imp__XamProfileGetCreationStatus, Args({kIn, kOut + 0xDD8}), 20000, nullptr, 0},
      {"XamProfileLoadAccountInfo", __imp__XamProfileLoadAccountInfo, Args({1, g_profile_xuid, kOut + 0x1A00}), 20000,
       nullptr, 0},
      {"XamUserCreateTitlesPlayedEnumerator (+ NtClose)", __imp__XamUserCreateTitlesPlayedEnumerator,
       Args({0, 0, 0, 0, 5, kEnumeratorOut + 4, kEnumeratorOut}), 2000, TitlesPlayedAndClose, 0},
      {"XamUserIsGuest", __imp__XamUserIsGuest, Args({0}), 20000, nullptr, 0},
      {"XamUserIsUnsafeProgrammingAllowed", __imp__XamUserIsUnsafeProgrammingAllowed, Args({0, 0, kOut + 0xDD0}), 20000,
       nullptr, 0},
      {"XamUserLogonEx", __imp__XamUserLogonEx, Args({kIn, 0, 0}), 20000, nullptr, 0},
      {"XamUserNuiEnableBiometric", __imp__XamUserNuiEnableBiometric, Args({0, 1}), 20000, nullptr, 0x80070057},
      {"XamUserNuiGetUserIndex", __imp__XamUserNuiGetUserIndex, Args({0, kOut + 0xDD0}), 20000, nullptr, 0x80070525},
      {"XamUserNuiGetUserIndexForBind", __imp__XamUserNuiGetUserIndexForBind, Args({kOut + 0xDD0}), 20000, nullptr,
       0x80004005},
      // The content calls (the ninth batch's package and metadata).
      {"XamContentGetAttributesInternal", __imp__XamContentGetAttributesInternal, Args({kContentData, kOut + 0x1A00, 0}), 2000,
       nullptr, 0},
      {"XamContentGetDeviceVolumePath", __imp__XamContentGetDeviceVolumePath, Args({1, kOut + 0x1A00, 16, 1}), 20000, nullptr,
       0},
      {"XamContentGetLocalizedDeviceData (drive)", __imp__XamContentGetLocalizedDeviceData, Args({1, kOut + 0x1A00}), 2000,
       nullptr, 0},
      {"XamContentGetLocalizedDeviceData (disc)", __imp__XamContentGetLocalizedDeviceData, Args({2, kOut + 0x1A00}), 20000,
       nullptr, 0},
      {"XamContentGetLocalizedString", __imp__XamContentGetLocalizedString,
       Args({kMetadata, kOut + 0x1A00, 64, kOut + 0x1B00, 64}), 20000, nullptr, 0},
      {"XamContentLaunchImageFromFileInternal", __imp__XamContentLaunchImageFromFileInternal, Args({kIn, kIn}), 20000,
       nullptr, 2},
      {"XamContentLaunchImageInternal", __imp__XamContentLaunchImageInternal, Args({kContentData, kIn}), 20000, nullptr, 2},
      {"XamContentLaunchImageInternalEx", __imp__XamContentLaunchImageInternalEx, Args({kContentData, kIn, 0}), 20000,
       nullptr, 2},
      {"XamContentResolveInternal (mounted again)", __imp__XamContentResolveInternal,
       Args({kContentData, kOut + 0x1A00, 0x100, 0, 0, 0}), 2000, nullptr, 0},
      {"XamReadTile", __imp__XamReadTile, Args({1, 0, 7, 0, kIn, kOut + 0xDD0, 0}), 20000, nullptr, 2},
      {"XamReadTileEx", __imp__XamReadTileEx, Args({1, 0, 7, 0, 0, 0, kIn, kOut + 0xDD0}, {0}), 20000, nullptr, 2},
      // The information calls.
      {"XamGetCountry", __imp__XamGetCountry, Args({}), 20000, nullptr, -1},
      {"XamSetCountry (the same)", __imp__XamSetCountry, Args({103}), 20000, nullptr, 0},
      {"XamGetDvrStorage", __imp__XamGetDvrStorage, Args({kOut + 0xDD0, kOut + 0xDD4, kOut + 0xDD8}), 20000, nullptr, 0},
      {"XamSetDvrStorage", __imp__XamSetDvrStorage, Args({0x100000, 0}), 20000, nullptr, 0},
      {"XamGetLanguageTypefacePatch", __imp__XamGetLanguageTypefacePatch, Args({1}), 20000, nullptr, -1},
      {"XamGetLiveHiveValueA", __imp__XamGetLiveHiveValueA, Args({kIn, kOut + 0x1A00, 64, 0, 0}), 20000, nullptr,
       0x80151802},
      {"XamGetLiveHiveValueW", __imp__XamGetLiveHiveValueW, Args({kIn, kOut + 0x1A00, 64, 0, 0}), 20000, nullptr,
       0x80151802},
      {"XamGetStagingMode", __imp__XamGetStagingMode, Args({}), 20000, nullptr, 0},
      {"XamIsNatalPlaybackEnabled", __imp__XamIsNatalPlaybackEnabled, Args({0, 0}), 20000, nullptr, 0},
      {"XamIsNuiAutomationEnabled", __imp__XamIsNuiAutomationEnabled, Args({0, 0}), 20000, nullptr, 0},
      // The Kinect calls.
      {"XamNuiCameraElevationGetAngle", __imp__XamNuiCameraElevationGetAngle, Args({kOut + 0xDD0, kOut + 0xDD4}), 20000,
       nullptr, 0x80004005},
      {"XamNuiCameraGetTiltControllerType", __imp__XamNuiCameraGetTiltControllerType, Args({}), 20000, nullptr, 0},
      {"XamNuiCameraSetFlags", __imp__XamNuiCameraSetFlags, Args({1, 2}), 20000, nullptr, 0x8007048F},
      {"XamNuiCameraTiltGetStatus", __imp__XamNuiCameraTiltGetStatus, Args({kOut + 0x1A00}), 20000, nullptr, 0x80004005},
      {"XamNuiCameraTiltSetCallback", __imp__XamNuiCameraTiltSetCallback, Args({0}), 20000, nullptr, 0},
      {"XamNuiGetDepthCalibration", __imp__XamNuiGetDepthCalibration, Args({kOut + 0x1A00}), 20000, nullptr, 0xD000000F},
      {"XamNuiIdentityAbort", __imp__XamNuiIdentityAbort, Args({1}), 20000, nullptr, 0x80004005},
      {"XamNuiIdentityEnrollForSignIn", __imp__XamNuiIdentityEnrollForSignIn, Args({1, 2, 3, 4}), 20000, nullptr,
       0x80004005},
      {"XamNuiPlayerEngagementUpdate", __imp__XamNuiPlayerEngagementUpdate, Args({0, 0, kOut + 0x1A00}), 20000, nullptr, -1},
      {"XamNuiSkeletonGetBestSkeletonIndex", __imp__XamNuiSkeletonGetBestSkeletonIndex, Args({0}), 20000, nullptr, -1},
      // The other calls: settings set to what they were, a context made on scratch memory for the reset.
      {"ExSetXConfigSetting (video flags)", __imp__ExSetXConfigSetting, Args({3, 0x0A, kConfigValue, 4}), 20000, nullptr, 0},
      {"ExReadModifyWriteXConfigSettingUlong (retail flags)", __imp__ExReadModifyWriteXConfigSettingUlong,
       Args({3, 0x0C, 0xFFFFFFFF, 0x40}), 20000, nullptr, 0},
      {"LDIResetDecompression", __imp__LDIResetDecompression, Args({kResetScratch}), 20000, nullptr, 0},
      {"XamDoesOmniNeedConfiguration", __imp__XamDoesOmniNeedConfiguration, Args({}), 20000, nullptr, 0},
      {"XamFirstRunExperienceShouldRun", __imp__XamFirstRunExperienceShouldRun, Args({}), 20000, nullptr, 0},
      {"XamLogLocalizationEtx", __imp__XamLogLocalizationEtx, Args({0x80300034, 0}), 20000, nullptr, 0},
      {"XamLookupCommonStringByIndex", __imp__XamLookupCommonStringByIndex, Args({3}), 20000, nullptr, 0},
      {"XamQueryLiveHiveA", __imp__XamQueryLiveHiveA, Args({kIn, kOut + 0x1A00, 64, 0}), 20000, nullptr, 0x80151802},
      {"XamXStudioRequest", __imp__XamXStudioRequest, Args({1, kOut + 0xDD0}), 20000, nullptr, 0x80004005},
      // The guide screens (no listener: the two notifications go nowhere).
      {"XamShowGoldUpgradeUI", __imp__XamShowGoldUpgradeUI, Args({0, 0, 0}), 20000, nullptr, 0x65B},
      {"XamShowMarketplaceUIEx", __imp__XamShowMarketplaceUIEx, Args({0, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0}), 20000,
       nullptr, 0},
      {"XamShowNuiDeviceSelectorUI", __imp__XamShowNuiDeviceSelectorUI, Args({1, 0, 1, 0, 0, kOut + 0xDD0, 0}), 20000,
       nullptr, 5},
      {"XamShowNuiDirtyDiscErrorUI", __imp__XamShowNuiDirtyDiscErrorUI, Args({1}), 20000, nullptr, 5},
      {"XamShowNuiGuideUI", __imp__XamShowNuiGuideUI, Args({1, 0}), 20000, nullptr, 5},
      {"XamShowNuiMessageBoxUI", __imp__XamShowNuiMessageBoxUI, Args({1, 0, kIn, kIn, 1, kIn, 0, 0}), 20000, nullptr, 5},
      {"XamShowNuiSigninUI", __imp__XamShowNuiSigninUI, Args({1, 0, 0}), 20000, nullptr, 5},
      {"XamShowSigninUIEx", __imp__XamShowSigninUIEx, Args({1, 0, 0}), 20000, nullptr, 0},
      // The module calls.
      {"XamLoaderGetMediaInfo", __imp__XamLoaderGetMediaInfo, Args({kOut + 0xDD0, kOut + 0xDD4}), 20000, nullptr, -1},
      {"XamLoaderGetMediaInfoEx", __imp__XamLoaderGetMediaInfoEx, Args({kOut + 0xDD0, kOut + 0xDD4, kOut + 0xDD8}), 20000,
       nullptr, -1},
      {"XamUpdateGetBaseSystemVersion", __imp__XamUpdateGetBaseSystemVersion, Args({}), 20000, nullptr, 1888},
      {"XamUpdateGetCurrentSystemVersion", __imp__XamUpdateGetCurrentSystemVersion, Args({}), 20000, nullptr, 0xFFFF},
      {"XexLoadExecutable (not loaded)", __imp__XexLoadExecutable, Args({kObjectName + 0x40, 0, 0, kOut + 0xDC4}), 20000,
       nullptr, 0xC000000F},
      // The input, network and notification calls (the raw length 0x20 at kRawLength).
      {"XamInputNonControllerGetRaw", __imp__XamInputNonControllerGetRaw, Args({kOut + 0xDD0, kRawLength, kOut + 0x1A00}),
       20000, nullptr, 0x48F},
      {"XamInputNonControllerGetRawEx", __imp__XamInputNonControllerGetRawEx,
       Args({5, kOut + 0x1A00, kRawLength, kOut + 0xDD0}), 20000, nullptr, 0x48F},
      {"XamInputNonControllerSetRaw", __imp__XamInputNonControllerSetRaw, Args({0x20, kIn}), 20000, nullptr, 0x48F},
      {"XamInputNonControllerSetRawEx", __imp__XamInputNonControllerSetRawEx, Args({5, kIn, 0x20}), 20000, nullptr, 0x48F},
      {"NetDll_XHttpStartup", __imp__NetDll_XHttpStartup, Args({1, 0, 0}), 20000, nullptr, 1},
      {"NetDll_XHttpOpen", __imp__NetDll_XHttpOpen, Args({1, kIn, 0, 0, 0, 0}), 20000, nullptr, 0},
      // The XamUserWriteUserPreference row above set user 0's network storage preference (0x20): on.
      {"XamNetworkStorageHasUserEnabledStorage (preference on)", __imp__XamNetworkStorageHasUserEnabledStorage, Args({0}),
       20000, nullptr, 1},
      {"XNotifyBroadcast (no listener)", __imp__XNotifyBroadcast, Args({0x11, 7}), 20000, nullptr, 0},
      {"XNotifyQueueUI", __imp__XNotifyQueueUI, Args({0, 0, 0, kIn, 0}), 20000, nullptr, 0},
      // The last calls: an SMC temperature query, a microphone status request (no completion), the tray closed
      // again (its notification to no listener), RtlRip with no debug sink.
      {"XAutomationpUnbindController", __imp__XAutomationpUnbindController, Args({0}), 20000, nullptr, 1},
      {"XamVoiceGetMicArrayStatus", __imp__XamVoiceGetMicArrayStatus, Args({}), 20000, nullptr, 0},
      {"MicDeviceRequest (status)", __imp__MicDeviceRequest, Args({kMicDevice}), 20000, nullptr, 0},
      {"HalSendSMCMessage (temperatures)", __imp__HalSendSMCMessage, Args({kSmcMessage, kSmcMessage + 0x10}), 20000,
       nullptr, -1},
      {"HalOpenCloseODDTray (close)", __imp__HalOpenCloseODDTray, Args({0}), 20000, nullptr, -1},
      {"NtCancelIoFile (not a file)", __imp__NtCancelIoFile, Args({0x12345678}), 20000, nullptr, 0xC0000008},
      {"RtlRip", __imp__RtlRip, Args({kIn, kIn}), 20000, nullptr, -1},
  };
  std::printf("sys_abi_bench: ns a call through the PowerPC ABI (fastest of %u runs), hashes and ciphers on %u bytes; "
              "original = the reference runtime's __imp__ (it stubs these: returns at once); its heap is not seen by "
              "the allocation meter; speed = original / Any360 (> 1: Any360 faster)\n",
              g_repeats, kBytes);
  std::printf("  %-36s | %10s | %10s %9s %7s | %9s\n", "call", "original", "Any360", "B/call", "allocs", "speed");
  for (const Row& row : rows) {
    // The 9th and 10th arguments on the stack, as the ABI passes them.
    Store32(stack_args + 4, uint32_t(row.args.stack[0]));
    Store32(stack_args + 12, uint32_t(row.args.stack[1]));
    const uint32_t n = row.calls;
    // The LDI rows: Create + Destroy decode no chunk, the stream row every chunk (none off Windows: skipped).
    const bool stream = row.native == LdiNative && std::strstr(row.name, "stream");
    if (stream && !stream_chunks) continue;
    g_chunks = stream ? stream_chunks : 0;
    const Measure original = Time(row.reference, row.args, n, false);
    const std::string name(row.name, std::strcspn(row.name, " "));
    const Measure ours = Time(row.native ? row.native : Native(name.c_str()), row.args, n, true);
    std::printf("  %-36s | %10.1f | %10.1f %9.1f %7.2f | %8.1fx\n", row.name, original.ns, ours.ns, ours.bytes,
                ours.allocations, original.ns / ours.ns);
    if (row.expect >= 0) {
      // The timed path is the one the row names: Any360's result once more, on the same registers.
      PPCContext ctx{};
      ctx.fpscr.InitHost();
      PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
      for (int k = 0; k < 8; ++k) regs[k]->u64 = row.args.r[k];
      ctx.r1.u64 = kStack + kStackSize - 0x1000;
      ctx.r13.u64 = kPcr;
      (row.native ? row.native : Native(name.c_str()))(ctx, g_base);
      if (ctx.r3.u32 != uint32_t(row.expect)) {
        std::printf("    !! %s returned 0x%X, not 0x%X\n", row.name, ctx.r3.u32, uint32_t(row.expect));
      }
    }
  }
  std::printf("sys_abi_bench: Any360 allocated per call %.2f B in %.4f allocations over %llu timed calls\n",
              g_calls ? double(g_bytes) / double(g_calls) : 0.0, g_calls ? double(g_count) / double(g_calls) : 0.0,
              static_cast<unsigned long long>(g_calls));
  return 0;
}
