// sys_abi_bench: the original against Any360 for step 12's system calls (docs/SYS_STUBS.md), through the PowerPC
// ABI a title calls them by: the reference runtime's `__imp__<name>` (ReXGlue, linked from the reference SDK) and
// Any360's adapter (any360::sys::kExports), each on the same registers and guest memory. Prints nanoseconds a call
// (fastest of the runs) and the host heap a call (bytes, allocations) of Any360's side; the reference runtime is a
// DLL with its own heap, which the allocation meter (operator new of this executable) does not see.
// The calls that block (Sleep, SleepEx, RtlSleep, the waits, CreateThread, ResumeThread, ExitThread) are left out: they are their
// kernel call (measured with it) plus the Win32 part sys_step12_bench times.
//
//   sys_abi_bench [repeats]   (results kept in docs/perf/archive/<date>-<step>/)
#include <rex/ppc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/exports.h"
#include "any360/sys/objects.h"
#include "any360/sys/xam.h"
#include "guest_memory.h"

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
  X(KeSetPriorityThread) X(HalGetCurrentAVPack) X(XamGetLocaleDateFormat)
STEP12_CALLS(REFERENCE)

namespace {

using any360::sys::AllocCounts;
using any360::sys::ThreadAllocations;

// Guest layout: a PCR whose +0x100 is the current KTHREAD, buffers, a stack.
constexpr uint32_t kPcr = 0x40000000, kThread = 0x40001000, kIn = 0x40010000, kOut = 0x40020000, kState = 0x40030000,
                   kKey = 0x40031000, kFeed = 0x40031100, kText = 0x40032000, kStack = 0x40100000, kStackSize = 0x10000,
                   kHeap = 0x40200000, kImage = 0x40300000;
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
};

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
  std::memset(g_base + kHeap, 0, 0x8000);
  Store32(kPcr + 0x100, kThread);
  for (uint32_t i = 0; i < 0x10000; ++i) g_base[kIn + i] = uint8_t(i * 131 + 7);
  std::memset(g_base + kKey, 0x0b, 64);
  for (uint32_t i = 0; i < 64; ++i) Store16(kText + 2 * i, uint16_t('a' + i % 26));
  Store16(kText + 128, 0);
  std::memcpy(g_base + kText + 0x200, "xam.xex", 8);
  any360::sys::Objects().SetKernelHeap(kHeap, 0x8000);
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
    const uint32_t n = 20000;
    const Measure original = Time(row.reference, row.args, n, false);
    const std::string name(row.name, std::strcspn(row.name, " "));
    const Measure ours = Time(Native(name.c_str()), row.args, n, true);
    std::printf("  %-36s | %10.1f | %10.1f %9.1f %7.2f | %8.1fx\n", row.name, original.ns, ours.ns, ours.bytes,
                ours.allocations, original.ns / ours.ns);
  }
  std::printf("sys_abi_bench: Any360 allocated per call %.2f B in %.4f allocations over %llu timed calls\n",
              g_calls ? double(g_bytes) / double(g_calls) : 0.0, g_calls ? double(g_count) / double(g_calls) : 0.0,
              static_cast<unsigned long long>(g_calls));
  return 0;
}
