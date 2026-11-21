// The PowerPC adapters of step 12's calls (any360::sys::kExports) called as a title calls them: the arguments in
// r3..r10 (the 9th and later on the stack), each export looked up by name, its r3 and the guest bytes it writes
// checked. sys_object_tests checks the answers behind the adapters; this checks that each adapter reads its
// arguments from the registers the call's prototype puts them in and returns its answer in r3. The reference runtime
// is not run (it stubs these calls), only linked for the PowerPC context's type.
#include <rex/ppc.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>

#include "any360/sys/config.h"
#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/ldi.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"
#include "guest_memory.h"

namespace {

using namespace any360::sys;
using Function = void (*)(PPCContext& ctx, uint8_t* base);

uint8_t* g_base = nullptr;
int g_failures = 0;

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

// Guest layout: a PCR whose +0x100 is the current KTHREAD, a stack, buffers.
constexpr uint32_t kPcr = 0x40000000, kKThread = 0x40001000, kStack = 0x40100000, kStackSize = 0x10000,
                   kHeap = 0x41000000, kBuf = 0x40020000, kOv = 0x40030000, kData = 0x40031000, kOut = 0x40032000;

Guest G() { return Guest{g_base}; }

Function Export(const char* name) {
  for (unsigned i = 0; i < kExportCount; ++i) {
    if (!std::strcmp(kExports[i].name, name)) return kExports[i].function;
  }
  std::printf("sys_adapter_tests: %s is not in any360::sys::kExports\n", name);
  std::exit(2);
}

// Calls an export with r3..r10 set; returns r3.
uint64_t Call(const char* name, std::initializer_list<uint64_t> registers) {
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  int i = 0;
  for (const uint64_t v : registers) r[i++]->u64 = v;
  ctx.r1.u64 = kStack + kStackSize - 0x1000;
  ctx.r13.u64 = kPcr;
  Export(name)(ctx, g_base);
  return ctx.r3.u64;
}

// An XOVERLAPPED with no event, its fields set to a marker.
void FreshOverlapped() {
  std::memset(g_base + kOv, 0xEE, 0x1C);
  G().Store32(kOv + 0x0C, 0);
}
bool Completed(uint32_t result, uint32_t extended) {
  return G().Load32(kOv) == result && G().Load32(kOv + 4) == 0 && G().Load32(kOv + 0x18) == extended;
}

void AvatarAdapters() {
  // The asynchronous ones: success, or ERROR_IO_PENDING with the overlapped completed (its register checked).
  const std::pair<const char*, int> overlapped_in[] = {
      {"XamAvatarBeginEnumAssets", 5}, {"XamAvatarEndEnumAssets", 0}, {"XamAvatarGenerateMipMaps", 4},
      {"XamAvatarGetAssetBinary", 4}, {"XamAvatarGetAssetIcon", 4}, {"XamAvatarGetManifestsByXuid", 5},
      {"XamAvatarGetMetadataRandom", 3}, {"XamAvatarGetMetadataSignedOutProfile", 2},
      {"XamAvatarGetMetadataSignedOutProfileCount", 1}, {"XamAvatarLoadAnimation", 3}, {"XamAvatarWearNow", 2}};
  for (const auto& [name, slot] : overlapped_in) {
    uint64_t regs[8] = {kData, 1, kData + 0x40, 2, kData + 0x80, 3, 0, 0};
    regs[slot] = 0;
    CHECK(Call(name, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]}) == 0);
    regs[slot] = kOv;
    FreshOverlapped();
    const uint64_t r3 = Call(name, {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]});
    if (r3 != 997 || !Completed(0, 0)) std::printf("  %s: r3 %#llx\n", name, (unsigned long long)r3);
    CHECK(r3 == 997 && Completed(0, 0));
  }
  CHECK(Call("XamAvatarEnumAssets", {kData, kData + 8, kOv}) == 0x80070012);
  // XamAvatarGetAssets: through an overlapped ERROR_FUNCTION_FAILED with E_FAIL.
  CHECK(Call("XamAvatarGetAssets", {kData, 0xFF, 0, kOut, kOut + 4, 0}) == 0);
  FreshOverlapped();
  CHECK(Call("XamAvatarGetAssets", {kData, 0xFF, 0, kOut, kOut + 4, kOv}) == 997 && Completed(0x65B, 0x80004005));
  G().Store32(kOut, 7), G().Store32(kOut + 4, 9);
  CHECK(Call("XamAvatarGetAssetsResultSize", {0xFF, kOut, kOut + 4}) == 0 && G().Load32(kOut) == 0 && G().Load32(kOut + 4) == 0);
  G().Store32(kOut, 7);
  CHECK(Call("XamAvatarGetInstrumentation", {6, kOut}) == 1 && G().Load32(kOut) == 0);
  CHECK(Call("XamAvatarManifestGetBodyType", {kData}) == 1);
  CHECK(Call("XamAvatarReinstallAwardedAsset", {kOut, 16, kOut + 0x20}) == 0);
  CHECK(Call("XamAvatarSetCustomAsset", {16, kData, 0, 0, kData}) == 0);
  Call("XamAvatarSetMocks", {});
  // The package of an asset: X_ASSET_ID (big-endian) to XCONTENT_DATA_AGGREGATE.
  G().Store64(kData, 0x0123456789ABCDEFull), G().Store32(kData + 8, 0x11223344), G().Store32(kData + 12, 0x4D5307E6);
  std::memset(g_base + kOut, 0xAA, 0x160);
  Call("XamAvatarGetInstalledAssetPackageDescription", {kData, kOut});
  CHECK(G().Load32(kOut) == 0 && G().Load32(kOut + 4) == 0x9000 && G().Load32(kOut + 0x140) == 0x4D5307E6);
  CHECK(std::string(reinterpret_cast<const char*>(g_base + kOut + 0x108)) == "0123456789ABCDEF112233444D5307E6");
  CHECK(G().Load64(kOut + 0x138) == 0 && g_base[kOut + 0x148] == 0xAA);
  // The manifest: none kept, then set and read back, through registers and overlapped.
  CHECK(Call("XamAvatarGetManifestLocalUser", {0, kOut, 0}) == 0x80004005);
  CHECK(Call("XamAvatarGetManifestLocalUser", {4, kOut, 0}) == 0x80070057);
  CHECK(Call("XamAvatarGetManifestLocalUser", {0, 0, 0}) == 0x80070057);
  CHECK(Call("XamAvatarGetManifestLocalUser", {1, kOut, 0}) == 0x80070525);
  for (uint32_t i = 0; i < 0x3E8; ++i) g_base[kData + i] = uint8_t(i * 7);
  CHECK(Call("XamAvatarSetManifest", {1, kData, 0}) == 0x80070525);
  FreshOverlapped();
  CHECK(Call("XamAvatarSetManifest", {0, kData, kOv}) == 997 && Completed(0, 0));
  std::memset(g_base + kOut, 0, 0x400);
  CHECK(Call("XamAvatarGetManifestLocalUser", {0, kOut, 0}) == 0 && std::memcmp(g_base + kOut, g_base + kData, 0x3E8) == 0);
  FreshOverlapped();
  CHECK(Call("XamAvatarGetManifestLocalUser", {2, kOut, kOv}) == 997 && Completed(0x65B, 0x80070525));
  // It is the profile setting XPROFILE_GAMERCARD_AVATAR_INFO_1 a title reads too.
  const uint32_t ids = kData + 0x800, size = kData + 0x804, buffer = kData + 0x1000;
  G().Store32(ids, 0x63E80044);
  G().Store32(size, 0x1000);
  CHECK(Call("XamUserReadProfileSettings", {0, 0, 0, 0, 1, ids, size, buffer}) == 0);
  const uint32_t setting = G().Load32(buffer + 4);
  CHECK(G().Load32(setting + 32) == 0x3E8 && std::memcmp(g_base + G().Load32(setting + 36), g_base + kData, 0x3E8) == 0);
}

void UserAdapters() {
  const uint64_t xuid = XamState().title().xuid;
  // XamProfileClose: a profile XamProfileOpen mounted is unmounted (closing it again fails), an unknown name fails.
  std::memcpy(g_base + kData + 0x100, "UPROF", 6);
  CHECK(Call("XamProfileOpen", {xuid, kData + 0x100, 0, 0}) == 0 && !Files().Resolve("UPROF:\\a").empty());
  CHECK(Call("XamProfileClose", {kData + 0x100}) == 0 && Files().Resolve("UPROF:\\a").empty());
  CHECK(Call("XamProfileClose", {kData + 0x100}) == 0x65B);
  // XamProfileClose closes content opened under the name too (Xenia's CloseContent), as XamContentClose would.
  std::memset(g_base + kData + 0x200, 0, 0x134);
  G().Store32(kData + 0x200, 1), G().Store32(kData + 0x204, 1);
  std::memcpy(g_base + kData + 0x200 + 0x108, "pclose", 7);
  std::memcpy(g_base + kData + 0x180, "PSAVE", 6);
  CHECK(Call("XamContentCreate", {0, kData + 0x180, kData + 0x200, 4, 0, 0, 0}) == 0);
  CHECK(!Files().Resolve("PSAVE:\\a").empty());
  CHECK(Call("XamProfileClose", {kData + 0x180}) == 0 && Files().Resolve("PSAVE:\\a").empty());
  CHECK(Call("XamContentClose", {kData + 0x180, 0}) == 2);  // already closed: ERROR_FILE_NOT_FOUND
  CHECK(Call("XamContentDelete", {0, kData + 0x200, 0}) == 0);
  // XamProfileCreate: no other profile; the creation info out set to 0, the device out 1 (as Xenia's).
  G().Store32(kOut, 0x1234), G().Store32(kOut + 4, 0x5678);
  CHECK(Call("XamProfileCreate", {0, kOut + 4, 0, kData, 0, 0, 0, kOut}) == 87);
  CHECK(G().Load32(kOut) == 0 && G().Load32(kOut + 4) == 1);
  // XamProfileGetCreationStatus: the info's offline XUID (at 0xA78: after the flags, device, account 0x17C, payment
  // info 0x8F0 and a word) out; no info, success and nothing written.
  G().Store64(kBuf + 0xA70, 0xBAD), G().Store64(kBuf + 0xA78, 0xE000000000001234ull);
  CHECK(Call("XamProfileGetCreationStatus", {kBuf, kOut + 8}) == 0 && G().Load64(kOut + 8) == 0xE000000000001234ull);
  G().Store64(kOut + 8, 7);
  CHECK(Call("XamProfileGetCreationStatus", {0, kOut + 8}) == 0 && G().Load64(kOut + 8) == 7);
  // XamProfileLoadAccountInfo: the account XamProfileFindAccount gives, for the local XUID only.
  std::memset(g_base + kOut + 0x100, 0xAA, 0x400);
  CHECK(Call("XamProfileFindAccount", {xuid, kOut + 0x100, 0}) == 0);
  CHECK(Call("XamProfileLoadAccountInfo", {1, xuid, kOut + 0x300}) == 0);
  CHECK(!std::memcmp(g_base + kOut + 0x100, g_base + kOut + 0x300, XamState().kAccountSize));
  CHECK(Call("XamProfileLoadAccountInfo", {1, xuid + 1, kOut + 0x300}) == 87);
  CHECK(Call("XamProfileLoadAccountInfo", {1, 0, kOut + 0x300}) == 87 && Call("XamProfileLoadAccountInfo", {1, xuid, 0}) == 87);
  // XamUserCreateTitlesPlayedEnumerator: an empty enumerator of 0xA8-byte items; its first enumeration is the end.
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 0, 0, 0, 5, kOut, kOut + 4}) == 0 && G().Load32(kOut) == 5 * 0xA8);
  const uint32_t titles = G().Load32(kOut + 4);
  CHECK(titles != 0 && Call("XamEnumerate", {titles, 0, kBuf, 5 * 0xA8, kOut + 8, 0}) == 18);
  CHECK(Call("NtClose", {titles}) == 0);
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 1, 0, 0, 5, kOut, kOut + 4}) == 0x65B);
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 4, 0, 0, 5, kOut, kOut + 4}) == 87);
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 0, 0, 0, 0, kOut, kOut + 4}) == 87);
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 0, 0, 0, 5, 0, kOut + 4}) == 87);
  CHECK(Call("XamUserCreateTitlesPlayedEnumerator", {0, 0, 0, 0, 5, kOut, 0}) == 87);
  CHECK(Call("XamUserIsGuest", {0}) == 0 && Call("XamUserIsGuest", {7}) == 0);
  // XamUserIsUnsafeProgrammingAllowed: allowed for users 0-3 and XUSER_INDEX_ANY.
  for (const uint32_t user : {0u, 3u, 0xFFu}) {
    G().Store32(kOut, 0);
    CHECK(Call("XamUserIsUnsafeProgrammingAllowed", {user, 0, kOut}) == 0 && G().Load32(kOut) == 1);
  }
  CHECK(Call("XamUserIsUnsafeProgrammingAllowed", {4, 0, kOut}) == 87 && Call("XamUserIsUnsafeProgrammingAllowed", {0, 0, 0}) == 87);
  // XamUserLogonEx: success, through the overlapped when given.
  CHECK(Call("XamUserLogonEx", {kData, 0, 0}) == 0);
  FreshOverlapped();
  CHECK(Call("XamUserLogonEx", {kData, 0, kOv}) == 997 && Completed(0, 0));
  CHECK(Call("XamUserNuiEnableBiometric", {0, 1}) == 0x80070057);
  CHECK(Call("XamUserNuiGetUserIndex", {0, kOut}) == 0x80070525);
  CHECK(Call("XamUserNuiGetUserIndexForBind", {kOut}) == 0x80004005 && Call("XamUserNuiGetUserIndexForBind", {0}) == 0x80070057);
}

// An XCONTENT_DATA_INTERNAL at `at` naming the saved game `name` on `device`.
void ContentData(uint32_t at, const char* name, uint32_t device = 1) {
  std::memset(g_base + at, 0, 0x148);
  G().Store32(at, device), G().Store32(at + 4, 1);
  std::memcpy(g_base + at + 0x108, name, std::strlen(name));
}

void ContentAdapters() {
  // A package made as a title makes it (its folder and header), closed again.
  ContentData(kData + 0x200, "cpkg");
  std::memcpy(g_base + kData + 0x180, "CPKG", 5);
  CHECK(Call("XamContentCreate", {0, kData + 0x180, kData + 0x200, 4, 0, 0, 0}) == 0 && Call("XamContentClose", {kData + 0x180, 0}) == 0);

  // XamContentGetAttributesInternal: attributes 0, the folder's three times (FILETIMEs of this century: 2015-2070),
  // size 0; 36 bytes and no more. A missing package: ERROR_FUNCTION_FAILED, the overlapped untouched.
  std::memset(g_base + kOut, 0xAA, 0x30);
  CHECK(Call("XamContentGetAttributesInternal", {kData + 0x200, kOut, 0}) == 0);
  CHECK(G().Load32(kOut) == 0 && G().Load32(kOut + 28) == 0 && G().Load32(kOut + 32) == 0 && g_base[kOut + 36] == 0xAA);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t high = G().Load32(kOut + 8 + 8 * i);
    CHECK(high > 0x01D00000 && high < 0x02200000);
  }
  FreshOverlapped();
  CHECK(Call("XamContentGetAttributesInternal", {kData + 0x200, kOut, kOv}) == 997 && Completed(0, 0));
  ContentData(kData + 0x400, "missing");
  FreshOverlapped();
  CHECK(Call("XamContentGetAttributesInternal", {kData + 0x400, kOut, kOv}) == 0x65B && G().Load32(kOv) == 0xEEEEEEEE);

  // XamContentGetDeviceVolumePath: "hdd0\", truncated to the size and terminated; another device fails.
  std::memset(g_base + kOut, 0xAA, 16);
  CHECK(Call("XamContentGetDeviceVolumePath", {1, kOut, 16, 1}) == 0 && !std::memcmp(g_base + kOut, "hdd0\\\0", 6));
  std::memset(g_base + kOut, 0xAA, 16);
  CHECK(Call("XamContentGetDeviceVolumePath", {1, kOut, 3, 0}) == 0 && !std::memcmp(g_base + kOut, "hd\0", 3) && g_base[kOut + 3] == 0xAA);
  CHECK(Call("XamContentGetDeviceVolumePath", {2, kOut, 16, 0}) == 0x65B);

  // XamContentGetLocalizedDeviceData: XamContentGetDeviceData's bytes, the disc named "Optical Drive".
  CHECK(Call("XamContentGetDeviceData", {1, kOut}) == 0 && Call("XamContentGetLocalizedDeviceData", {1, kOut + 0x100}) == 0);
  CHECK(!std::memcmp(g_base + kOut, g_base + kOut + 0x100, 0x50));
  CHECK(Call("XamContentGetLocalizedDeviceData", {2, kOut + 0x100}) == 0 && G().Load32(kOut + 0x100) == 2);
  for (uint32_t i = 0; i < 14; ++i) CHECK(G().Load16(kOut + 0x100 + 24 + 2 * i) == uint16_t(u"Optical Drive"[i]));
  CHECK(Call("XamContentGetLocalizedDeviceData", {3, kOut + 0x100}) == 1167);  // ERROR_DEVICE_NOT_CONNECTED

  // XamContentGetLocalizedString: the console language's display name and description, truncated and terminated.
  const uint32_t metadata = kBuf;  // 0x93D6 bytes
  std::memset(g_base + metadata, 0, 0x93D6);
  G().Store32(metadata + 4, 2);
  const uint32_t saved_language = Console().language;
  auto put = [&](uint32_t at, const char16_t* text) {
    for (uint32_t i = 0; text[i]; ++i) G().Store16(at + 2 * i, uint16_t(text[i]));
  };
  Console().language = 3;  // German: the third slot
  put(metadata + 0xCD + 2 * 0x100, u"Spielstand");
  put(metadata + 0x9CD + 2 * 0x100, u"Beschreibung");
  std::memset(g_base + kOut, 0xAA, 0x100);
  CHECK(Call("XamContentGetLocalizedString", {metadata, kOut, 6, kOut + 0x80, 64}) == 0);
  for (uint32_t i = 0; i < 5; ++i) CHECK(G().Load16(kOut + 2 * i) == uint16_t(u"Spiel"[i]));
  CHECK(G().Load16(kOut + 10) == 0 && G().Load16(kOut + 12) == 0xAAAA);
  for (uint32_t i = 0; i < 13; ++i) CHECK(G().Load16(kOut + 0x80 + 2 * i) == uint16_t(u"Beschreibung"[i]));
  // Language 10 (Polish) sits in the version 2 slots; with version 1 it is empty.
  Console().language = 10;
  put(metadata + 0x50D6, u"Zapis");
  CHECK(Call("XamContentGetLocalizedString", {metadata, kOut, 64, 0, 0}) == 0 && G().Load16(kOut) == 'Z' && G().Load16(kOut + 10) == 0);
  G().Store32(metadata + 4, 1);
  CHECK(Call("XamContentGetLocalizedString", {metadata, kOut, 64, 0, 0}) == 0 && G().Load16(kOut) == 0);
  Console().language = saved_language;
  CHECK(Call("XamContentGetLocalizedString", {0, kOut, 64, 0, 0}) == 87);

  // XamContentResolveInternal: the package's device path, mounted on its folder (made when asked); through a root
  // name, the name (taken: ERROR_ALREADY_EXISTS, the path still given); the disc's path as Xenia gives it.
  ContentData(kData + 0x400, "rpkg");
  std::memset(g_base + kOut, 0xAA, 0x100);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 1, 0, 0}) == 0);
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), "\\Device\\Content\\00000001\\rpkg"));
  CHECK(std::filesystem::is_directory(XamState().title().content_root / "00000001" / "rpkg"));
  CHECK(!Files().Resolve("\\Device\\Content\\00000001\\rpkg\\file.bin").empty());
  std::memset(g_base + kOut, 0xAA, 0x100);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 8, 0, 0, 0}) == 0 && !std::memcmp(g_base + kOut, "\\Device\0", 8));
  std::memcpy(g_base + kData + 0x180, "RROOT", 6);
  FreshOverlapped();
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, kData + 0x180, kOv}) == 997 && Completed(0, 0));
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), "RROOT:") && Files().HasLink("RROOT:"));
  CHECK(!Files().Resolve("RROOT:\\file.bin").empty());
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, kData + 0x180, 0}) == 0xB7);
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), "RROOT:"));
  Files().Unlink("RROOT:");
  // Asked again (the package mounted): the same path from the fast path, truncated the same way, through an
  // overlapped too; a name with ':' cleaned as ContentPath cleans it.
  std::memset(g_base + kOut, 0xAA, 0x100);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, 0}) == 0);
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), "\\Device\\Content\\00000001\\rpkg"));
  std::memset(g_base + kOut, 0xAA, 0x100);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 8, 0, 0, 0}) == 0 && !std::memcmp(g_base + kOut, "\\Device\0", 8));
  FreshOverlapped();
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, kOv}) == 997 && Completed(0, 0));
  ContentData(kData + 0x600, "a:b");
  CHECK(Call("XamContentResolveInternal", {kData + 0x600, kOut, 0x100, 1, 0, 0}) == 0);
  CHECK(Call("XamContentResolveInternal", {kData + 0x600, kOut, 0x100, 0, 0, 0}) == 0);
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), "\\Device\\Content\\00000001\\a_b"));
  CHECK(std::filesystem::is_directory(XamState().title().content_root / "00000001" / "a_b"));
  // The content root moved: the mount on the old folder is not taken as resolved; the package mounted again on the
  // new root's folder (made when asked), its files found there.
  const std::filesystem::path old_root = XamState().title().content_root;
  XamState().title().content_root = old_root.parent_path() / "content2";
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 1, 0, 0}) == 0);
  CHECK(Files().Resolve("\\Device\\Content\\00000001\\rpkg\\file.bin") ==
        XamState().title().content_root / "00000001" / "rpkg" / "file.bin");
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, 0}) == 0);
  CHECK(Files().Resolve("\\Device\\Content\\00000001\\rpkg\\file.bin") ==
        XamState().title().content_root / "00000001" / "rpkg" / "file.bin");
  XamState().title().content_root = old_root;
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, 0}) == 0);
  CHECK(Files().Resolve("\\Device\\Content\\00000001\\rpkg\\file.bin") == old_root / "00000001" / "rpkg" / "file.bin");
  ContentData(kData + 0x400, "dpkg", 2);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, 0}) == 0);
  char disc[160];
  std::snprintf(disc, sizeof disc, "D:\\content\\%016llX\\%08X\\00000001\\dpkg",
                static_cast<unsigned long long>(XamState().title().xuid), XamState().title().title_id);
  CHECK(!std::strcmp(reinterpret_cast<const char*>(g_base + kOut), disc));
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, kData + 0x180, 0}) == 87);  // a root off the drive
  ContentData(kData + 0x400, "xpkg", 3);
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 0, 0, 0}) == 87);
  ContentData(kData + 0x400, "..");
  CHECK(Call("XamContentResolveInternal", {kData + 0x400, kOut, 0x100, 1, 0, 0}) == 87);  // names no folder of its own

  // XamReadTile / XamReadTileEx: no tiles in the profile.
  const uint64_t xuid = XamState().title().xuid;
  G().Store32(kOut, 0x1000);
  CHECK(Call("XamReadTile", {1, 0, 7, 0, kBuf, kOut, 0}) == 2 && G().Load32(kOut) == 0);
  CHECK(Call("XamReadTile", {1, 0, 7, 0, 0, kOut, 0}) == 122);
  CHECK(Call("XamReadTile", {1, 0, xuid, 2, kBuf, kOut, 0}) == 2);  // the item id names the profile
  CHECK(Call("XamReadTile", {1, 0, 7, 2, kBuf, kOut, 0}) == 87 && Call("XamReadTile", {1, 0, 7, 0, kBuf, 0, 0}) == 87);
  FreshOverlapped();
  CHECK(Call("XamReadTile", {1, 0, 7, 0, kBuf, kOut, kOv}) == 997 && Completed(2, 0x80070002));
  // XamReadTileEx: the overlapped is its ninth argument, on the caller's stack.
  FreshOverlapped();
  G().Store32(kStack + kStackSize - 0x1000 + 0x54, kOv);
  CHECK(Call("XamReadTileEx", {1, 0, 7, 0, 0, 0, 0, kOut}) == 997 && Completed(122, 0x8007007A));
  G().Store32(kStack + kStackSize - 0x1000 + 0x54, 0);
  CHECK(Call("XamReadTileEx", {1, 0, 7, 0, 0, 0, kBuf, kOut}) == 2);

  // The launch calls: no image besides the title's.
  CHECK(Call("XamContentLaunchImageFromFileInternal", {kData, kData}) == 2);
  CHECK(Call("XamContentLaunchImageInternal", {kData + 0x200, kData}) == 2);
  CHECK(Call("XamContentLaunchImageInternalEx", {kData + 0x200, kData, 0}) == 2);
}

void InfoAdapters() {
  // The country: the console's, set for the run (ExGetXConfigSetting reads the same value).
  const uint8_t country = Console().country;
  CHECK(Call("XamGetCountry", {}) == country);
  CHECK(Call("XamSetCountry", {0x22}) == 0 && Call("XamGetCountry", {}) == 0x22 && Console().country == 0x22);
  Console().country = country;
  // DVR storage: none; the third out untouched.
  G().Store32(kOut, 5), G().Store32(kOut + 4, 6), G().Store32(kOut + 8, 7);
  CHECK(Call("XamGetDvrStorage", {kOut, kOut + 4, kOut + 8}) == 0);
  CHECK(G().Load32(kOut) == 0 && G().Load32(kOut + 4) == 0 && G().Load32(kOut + 8) == 7);
  CHECK(Call("XamSetDvrStorage", {0x100000, 0}) == 0);
  // The font patch names: big-endian UTF-16, laid out once.
  auto patch = [&](uint32_t language, const char16_t* expected) {
    const uint32_t at = uint32_t(Call("XamGetLanguageTypefacePatch", {language}));
    bool same = at != 0;
    for (uint32_t i = 0; same; ++i) {
      same = G().Load16(at + 2 * i) == uint16_t(expected[i]);
      if (!expected[i]) break;
    }
    return same ? at : 0u;
  };
  const uint32_t jk = patch(1, u"file://media:/XenonJKLatin.xttp2");
  CHECK(jk != 0 && patch(9, u"file://media:/XenonJKLatin.xttp2") == jk);
  CHECK(patch(17, u"file://media:/XenonSCLatin.xttp2") != 0 && patch(8, u"file://media:/XenonCLatin.xttp2") != 0);
  // The Live hive: not signed in; E_INVALIDARG without a name, a value or its size; through an overlapped, the
  // result as the extended error too, the length ~0.
  for (const char* name : {"XamGetLiveHiveValueA", "XamGetLiveHiveValueW"}) {
    CHECK(Call(name, {kData, kOut, 64, 0, 0}) == 0x80151802);
    CHECK(Call(name, {0, kOut, 64, 0, 0}) == 0x80070057 && Call(name, {kData, 0, 64, 0, 0}) == 0x80070057);
    CHECK(Call(name, {kData, kOut, 0, 0, 0}) == 0x80070057);
    FreshOverlapped();
    CHECK(Call(name, {kData, kOut, 64, 0xFFFFFFFF, kOv}) == 997);
    CHECK(G().Load32(kOv) == 0x80151802 && G().Load32(kOv + 4) == 0xFFFFFFFF && G().Load32(kOv + 0x18) == 0x80151802);
  }
  CHECK(Call("XamGetStagingMode", {}) == 0);
  CHECK(Call("XamIsNatalPlaybackEnabled", {1, 2}) == 0 && Call("XamIsNuiAutomationEnabled", {1, 2}) == 0);
}

void NuiAdapters() {
  // No Kinect: no tilt controller, the motor's status and the angle fail with nothing written, the flags find no
  // device; the tilt callback kept.
  CHECK(Call("XamNuiCameraGetTiltControllerType", {}) == 0);
  G().Store32(kOut, 0x50), G().Store32(kOut + 4, 0x58745373);  // a status buffer: its size and "XtSs"
  CHECK(Call("XamNuiCameraTiltGetStatus", {kOut}) == 0x80004005 && Call("XamNuiCameraTiltGetStatus", {0}) == 0x80004005);
  G().Store32(kOut + 0x60, 7), G().Store32(kOut + 0x64, 8);
  CHECK(Call("XamNuiCameraElevationGetAngle", {kOut + 0x60, kOut + 0x64}) == 0x80004005);
  CHECK(G().Load32(kOut + 0x60) == 7 && G().Load32(kOut + 0x64) == 8);
  CHECK(Call("XamNuiCameraSetFlags", {1, 2}) == 0x8007048F);
  CHECK(Call("XamNuiCameraTiltSetCallback", {0x82001000}) == 0 && XamState().nui_tilt_callback() == 0x82001000);
  // The depth calibration cleared (0x30 bytes, no more), none stored.
  std::memset(g_base + kOut, 0xAA, 0x40);
  CHECK(Call("XamNuiGetDepthCalibration", {kOut}) == 0xD000000F);
  for (uint32_t i = 0; i < 0x30; ++i) CHECK(g_base[kOut + i] == 0);
  CHECK(g_base[kOut + 0x30] == 0xAA);
  // Identity: the HUD is not enabled.
  CHECK(Call("XamNuiIdentityAbort", {1}) == 0x80004005 && Call("XamNuiIdentityEnrollForSignIn", {1, 2, 3, 4}) == 0x80004005);
  // The engagement update cleared (0x1C bytes, no more), r3 left as it was.
  std::memset(g_base + kOut, 0xAA, 0x20);
  CHECK(Call("XamNuiPlayerEngagementUpdate", {0x1234, 0, kOut}) == 0x1234);
  for (uint32_t i = 0; i < 0x1C; ++i) CHECK(g_base[kOut + i] == 0);
  CHECK(g_base[kOut + 0x1C] == 0xAA);
  CHECK(Call("XamNuiSkeletonGetBestSkeletonIndex", {0}) == ~0ull);
}

void MediaAdapters() {
  for (const char* name : {"EnumerateMediaObjects", "EnumerateMediaObjects_", "EnumerateMediaObjects_0", "EnumerateMediaObjects_1"}) {
    CHECK(Call(name, {kData, 1, kOut}) == 0x80004001);
  }
  CHECK(Call("XamMediaVerificationCreate", {1}) == 0 && Call("XamMediaVerificationClose", {}) == 0);
  CHECK(Call("XamMediaVerificationInject", {1, 2}) == 0);
  // Verify: a callback becomes a user APC on the calling thread (its low bit cleared); none without one.
  const size_t queued = Threads().PendingApcs(kKThread);
  CHECK(Call("XamMediaVerificationVerify", {0, kOv, 0}) == 0 && Threads().PendingApcs(kKThread) == queued);
  CHECK(Call("XamMediaVerificationVerify", {0, kOv, 0x82001235}) == 0 && Threads().PendingApcs(kKThread) == queued + 1);
  // FailedBlocks: the structure's size first; 0x18 gets zeros and "complete", another size ERROR_NOT_ENOUGH_MEMORY.
  std::memset(g_base + kOut, 0xAA, 0x20);
  G().Store32(kOut, 0x18);
  CHECK(Call("XamMediaVerificationFailedBlocks", {kOut}) == 0);
  CHECK(G().Load32(kOut) == 0x18 && G().Load32(kOut + 4) == 0 && G().Load32(kOut + 16) == 0 && G().Load32(kOut + 20) == 1);
  CHECK(g_base[kOut + 0x18] == 0xAA);
  G().Store32(kOut, 0x14);
  G().Store32(kOut + 4, 0x55);
  CHECK(Call("XamMediaVerificationFailedBlocks", {kOut}) == 8 && G().Load32(kOut + 4) == 0x55);
  CHECK(Call("XamMediaVerificationFailedBlocks", {0}) == 0);
  Call("XampWebInstrumentationSetProfileCounts", {0, 1, 1, 0});
  // XAuth: not there.
  G().Store32(kData, 8);
  CHECK(Call("XampXAuthStartup", {kData}) == 0x80158406);
  G().Store32(kData, 12);
  CHECK(Call("XampXAuthStartup", {kData}) == 0x80158401 && Call("XampXAuthStartup", {0}) == 0x80158401);
  G().Store32(kOut, 5);
  Call("XampXAuthShutdown", {kOut});
  CHECK(G().Load32(kOut) == 0);
  Call("XampXAuthShutdown", {0});
  CHECK(Call("XampXAuthGetTitleBuffer", {}) == 0);
}

// Step 12's eighth to tenth batches: the registers of the adapters sys_object_tests checks only behind them.
uint64_t AllocateProcedure(uint32_t, uint32_t, uint32_t, const uint64_t* args, uint32_t count, void*) {
  return count == 3 && args[1] == 0x54657374 ? 0x40038000 : 0;
}

void EarlierBatchAdapters() {
  // Eighth batch.
  CHECK(Call("XTLGetLanguageV2", {}) == 1);
  std::memset(g_base + kOut, 0xAA, 16);
  CHECK(Call("XapipGetLocale", {3, kOut}) == 0 && std::memcmp(g_base + kOut, "en\0", 3) == 0 && g_base[kOut + 3] == 0xAA);
  CHECK(Call("XapiFormatTimeOut", {kOut, 1500}) == kOut && int64_t(G().Load64(kOut)) == -15000000);
  CHECK(Call("XapiFormatTimeOut", {kOut, 0xFFFFFFFF}) == 0);
  CHECK(Call("XamIptvGetServiceName", {kOut}) == 0 && G().Load32(kOut) != 0);
  CHECK(Call("XamAllocEx", {0x20000000, 0x40000000, 0x2345, kOut}) == 0 && G().Load32(kOut) >= 0x49000000);
  CHECK(Memory().QueryProtect(G().Load32(kOut)) == 4);
  // LDI on scratch memory: one uncompressed chunk of 6 bytes.
  const uint32_t io = kData + 0x100, scratch = kData + 0x3000, chunk = kData + 0x200;
  G().Store32(io, 0x8000), G().Store32(io + 4, 0x8000);
  CHECK(Call("LDICreateDecompression", {io, io + 4, 0, 0, scratch, io + 8, io + 12}) == 0);
  CHECK(G().Load32(io + 8) == 0x9800 && G().Load32(io + 12) == scratch);
  const uint8_t stream[] = {0x00, 0x30, 0x60, 0x00, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
                            'L', 'Z', 'X', 'o', 'k', '!'};
  // E8 bit 0, type 3, length 6 (0 011 then 24 bits): the 16-bit words 0x3000 and 0x0060, little-endian; then R0-R2.
  std::memcpy(g_base + chunk, stream, sizeof stream);
  G().Store32(io + 16, 6);
  CHECK(Call("LDIDecompress", {scratch, chunk, sizeof stream, kOut, io + 16}) == 0);
  CHECK(G().Load32(io + 16) == 6 && std::memcmp(g_base + kOut, "LZXok!", 6) == 0);
  CHECK(Call("LDIDestroyDecompression", {scratch}) == 0 && Call("LDIDestroyDecompression", {scratch}) == 2);
  // Ninth batch.
  CHECK(Call("XamContentCreateEnumeratorInternal", {0, 3, 1, 0, 0, 1, kOut + 4, kOut}) == 0x80070057 && G().Load32(kOut + 4) == 0);
  CHECK(Call("XamContentCreateEnumeratorInternal", {0, 1, 1, 0, 0, 4, kOut + 4, kOut}) == 0 && G().Load32(kOut + 4) == 4 * 0x134);
  std::memset(g_base + kData, 0, 0x200);
  std::memcpy(g_base + kData + 0x108, "missing", 8);
  FreshOverlapped();
  CHECK(Call("XamContentGetMetaDataInternal", {kData, kOut, kOv}) == 0x65B && G().Load32(kOv) == 0xEEEEEEEE);
  CHECK(Call("XamContentSetThumbnailInternal", {kData, kBuf, 4}) == 2);  // ERROR_FILE_NOT_FOUND: no such package
  Call("XamContentRegisterChangeCallback", {0x82001234});
  Call("XamLoaderRegisterLaunchRequestCallback", {0x82005678});
  CHECK(XamState().content_change_callback() == 0x82001234 && XamState().launch_request_callback() == 0x82005678);
  G().Store32(kOut + 8, 4);
  G().Store32(kOut + 16, 0xAAAAAAAA);
  CHECK(Call("XamReadString", {0x4D5307E6, 5, 0, kOut + 16, kOut + 8, 0}) == 0 && G().Load16(kOut + 16) == 0);
  CHECK(Call("XamReadString", {0x4D5307E6, 0xFFFF, 0, kOut + 16, kOut + 8, 0}) == 0x80070057);
  CHECK(Call("NetDll_WSAStartupEx", {0, 0x0202, kOut + 0x100, 0x0202}) == 0 && G().Load16(kOut + 0x100) == 0x0202);
  CHECK(Call("NetDll_XNetStartupEx", {0, 0, 0x20501400}) == 0);
  CHECK(Call("XamNetworkStorageUserHasPrivilege", {0}) == 0);
  CHECK(Call("ObLookupAnyThreadByThreadId", {0x7777, kOut}) == 0xC0000225);
  // Tenth batch: the allocate procedure through the scheduler's dispatcher, on the calling thread.
  const uint32_t type = kData + 0x400;
  std::memset(g_base + type, 0, 0x20);
  G().Store32(type, 0x82001000), G().Store32(type + 0x18, 0x54657374);
  Threads().SetDispatcher(&AllocateProcedure, nullptr);
  CHECK(Call("ObCreateObject", {type, 0, 0x40, kOut}) == 0 && G().Load32(kOut) == 0x40038010);
  CHECK(G().Load32(0x40038000) == 1 && G().Load32(0x40038008) == type);
  std::memcpy(g_base + kOut + 0x40, "xam.xex", 8);
  CHECK(Call("XexLoadImageFromMemory", {kBuf, 0x100, kOut + 0x40, 0, 0, kOut + 0x80}) == 0xC0000035);
  std::memcpy(g_base + kOut + 0x40, "dlc.xex", 8);
  CHECK(Call("XexLoadImageFromMemory", {kBuf, 0x100, kOut + 0x40, 0, 0, kOut + 0x80}) == 0xC000000F);
}

}  // namespace

int main() {
  g_base = any360::test::GuestMemory::Get().base();
  std::memset(g_base + kPcr, 0, 0x2000);
  std::memset(g_base + kHeap, 0, 0x100000);
  G().Store32(kPcr + 0x100, kKThread);
  Objects().SetKernelHeap(kHeap, 0x100000);
  Objects().SetGuest(G());
  Objects().Reset();
  Memory().Reset({{0x48000000, 0x1000000, 0x1000, false}, {0x49000000, 0x1000000, 0x1000, true}});
  XamState().Reset();
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "any360-adapter-tests";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  XamState().title().content_root = root / "content";
  XamState().title().profile_root = root / "profile";
  AvatarAdapters();
  MediaAdapters();
  UserAdapters();
  ContentAdapters();
  InfoAdapters();
  NuiAdapters();
  EarlierBatchAdapters();
  Threads().SetDispatcher(nullptr, nullptr);
  std::filesystem::remove_all(root, error);
  if (g_failures) {
    std::printf("sys_adapter_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sys_adapter_tests: all passed\n");
  return 0;
}
