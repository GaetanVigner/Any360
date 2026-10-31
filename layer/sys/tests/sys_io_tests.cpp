// Runs each system call of any360::sys against the reference runtime's implementation (ReXGlue's `__imp__<name>`,
// linked from the reference SDK) on the same random inputs: registers r3..r10 and two 16 KB guest blocks seeded with
// random words, strings and structures. Compares r3 and both blocks byte for byte afterwards. A difference names the
// export, the trial and the first differing item.
// Run: sys_io_tests [--trials N]
#include <rex/ppc.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "any360/sys/exports.h"
#include "guest_memory.h"

#define REFERENCE(name) extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base);
REFERENCE(RtlCompareMemoryUlong)
REFERENCE(RtlFillMemoryUlong)
REFERENCE(RtlUpperChar)
REFERENCE(RtlLowerChar)
REFERENCE(RtlCompareString)
REFERENCE(RtlCompareStringN)
REFERENCE(RtlInitAnsiString)
REFERENCE(RtlInitUnicodeString)
REFERENCE(RtlMultiByteToUnicodeN)
REFERENCE(RtlUnicodeToMultiByteN)
REFERENCE(RtlTimeToTimeFields)
REFERENCE(RtlTimeFieldsToTime)
REFERENCE(RtlComputeCrc32)
REFERENCE(XGetAVPack)
REFERENCE(XGetGameRegion)
REFERENCE(XGetLanguage)
REFERENCE(XamGetSystemVersion)
REFERENCE(RtlNtStatusToDosError)
REFERENCE(ExGetXConfigSetting)
REFERENCE(RtlInitializeCriticalSection)
REFERENCE(RtlInitializeCriticalSectionAndSpinCount)
REFERENCE(VdQueryVideoMode)
REFERENCE(XGetVideoMode)
REFERENCE(VdQueryVideoFlags)
REFERENCE(VdGetCurrentDisplayInformation)
REFERENCE(VdGetCurrentDisplayGamma)
REFERENCE(VdSetDisplayMode)
REFERENCE(VdEnableDisableClockGating)
REFERENCE(VdIsHSIOTrainingSucceeded)
REFERENCE(VdRetrainEDRAM)
REFERENCE(VdRetrainEDRAMWorker)
REFERENCE(VdGetGraphicsAsicID)
REFERENCE(VdGetSystemCommandBuffer)
REFERENCE(VdCallGraphicsNotificationRoutines)
REFERENCE(VdInitializeScalerCommandBuffer)
REFERENCE(NetDll_inet_addr)
REFERENCE(NetDll_XNetInAddrToString)
REFERENCE(NetDll_XNetXnAddrToInAddr)
REFERENCE(NetDll_XNetInAddrToXnAddr)
REFERENCE(NetDll___WSAFDIsSet)

namespace {

using Function = void (*)(PPCContext&, uint8_t*);
constexpr uint32_t kBlockA = 0x40000000, kBlockB = 0x40010000, kBlockSize = 0x4000;
constexpr uint32_t kStack = 0x70000000, kStackSize = 0x10000;
// Named places inside the blocks the preparations use.
constexpr uint32_t kString1 = kBlockA + 0x100, kString2 = kBlockB + 0x100, kWide = kBlockB + 0x300;
constexpr uint32_t kStruct = kBlockA + 0x200, kOut = kBlockA + 0x400, kWritten = kBlockA + 0x800;
constexpr uint32_t kTime = kBlockA + 0x500, kFields = kBlockA + 0x600, kTime2 = kBlockA + 0x700;

struct Scene {
  uint8_t* base;
  void Word(uint32_t a, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + a, &v, 4);
  }
  void Half(uint32_t a, uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(base + a, &v, 2);
  }
  void Quad(uint32_t a, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(base + a, &v, 8);
  }
  // A NUL-terminated string of letters, digits and a few symbols, `n` characters long.
  void Text(uint32_t a, std::mt19937& rng, uint32_t n) {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ ";
    for (uint32_t i = 0; i < n; ++i) base[a + i] = kAlphabet[rng() % (sizeof(kAlphabet) - 1)];
    base[a + n] = 0;
  }
  void WideText(uint32_t a, std::mt19937& rng, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) Half(a + 2 * i, uint16_t(rng() % 4 ? 0x20 + rng() % 95 : rng() % 0xFFFF + 1));
    Half(a + 2 * n, 0);
  }
};

using Prepare = std::function<void(Scene&, PPCContext&, std::mt19937&)>;

struct Case {
  const char* name;
  Function native, reference;
  Prepare prepare;
};

void Strings(Scene& s, std::mt19937& rng) {
  const uint32_t n = rng() % 40;
  s.Text(kString1, rng, n);
  switch (rng() % 4) {
    case 0: s.Text(kString2, rng, rng() % 40); break;                         // unrelated
    case 1: std::memcpy(s.base + kString2, s.base + kString1, n + 1); break;  // equal
    case 2:                                                                     // same letters, other case
      for (uint32_t i = 0; i <= n; ++i) {
        const char c = char(s.base[kString1 + i]);
        s.base[kString2 + i] = char((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ? c ^ 0x20 : c);
      }
      break;
    default:  // a prefix
      std::memcpy(s.base + kString2, s.base + kString1, n + 1);
      s.base[kString2 + rng() % (n + 1)] = 0;
  }
}

Function Native(const char* name) {
  for (unsigned i = 0; i < any360::sys::kExportCount; ++i) {
    if (!std::strcmp(any360::sys::kExports[i].name, name)) return any360::sys::kExports[i].function;
  }
  std::printf("sys_io_tests: %s is not in any360::sys::kExports\n", name);
  std::exit(2);
}

#define CASE(name, ...) {#name, Native(#name), &__imp__##name, __VA_ARGS__}

const Case kCases[] = {
    CASE(RtlCompareMemoryUlong, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = kBlockA + (rng() % 4 ? 0 : 1);
      ctx.r4.u64 = rng() % 64 * 4 + (rng() % 4 ? 0 : 1);
      ctx.r5.u64 = rng() % 2 ? 0x11223344 : uint32_t(rng());
      for (uint32_t i = 0; i < 64; ++i) if (rng() % 3 == 0) s.Word(kBlockA + 4 * i, 0x11223344);
    }),
    CASE(RtlFillMemoryUlong, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = kBlockA + rng() % 16 * 4;
      ctx.r4.u64 = rng() % 256;
      ctx.r5.u64 = uint32_t(rng());
    }),
    CASE(RtlUpperChar, [](Scene&, PPCContext& ctx, std::mt19937& rng) { ctx.r3.u64 = uint32_t(rng()); }),
    CASE(RtlLowerChar, [](Scene&, PPCContext& ctx, std::mt19937& rng) { ctx.r3.u64 = uint32_t(rng()); }),
    CASE(RtlCompareString, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      Strings(s, rng);
      ctx.r3.u64 = kString1;
      ctx.r4.u64 = kString2;
      ctx.r5.u64 = rng() % 2;
    }),
    CASE(RtlCompareStringN, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      Strings(s, rng);
      ctx.r3.u64 = kString1;
      ctx.r4.u64 = rng() % 2 ? 0xFFFF : rng() % 45;
      ctx.r5.u64 = kString2;
      ctx.r6.u64 = rng() % 2 ? 0xFFFF : rng() % 45;
      ctx.r7.u64 = rng() % 2;
    }),
    CASE(RtlInitAnsiString, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.Text(kString2, rng, rng() % 60);
      ctx.r3.u64 = kStruct;
      ctx.r4.u64 = rng() % 4 ? kString2 : 0;
    }),
    CASE(RtlInitUnicodeString, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.WideText(kWide, rng, rng() % 60);
      ctx.r3.u64 = kStruct;
      ctx.r4.u64 = rng() % 4 ? kWide : 0;
    }),
    CASE(RtlMultiByteToUnicodeN, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.Text(kString2, rng, 60);
      ctx.r3.u64 = kOut;
      ctx.r4.u64 = rng() % 100;
      ctx.r5.u64 = rng() % 3 ? kWritten : 0;
      ctx.r6.u64 = kString2;
      ctx.r7.u64 = rng() % 60;
    }),
    CASE(RtlUnicodeToMultiByteN, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.WideText(kWide, rng, 60);
      ctx.r3.u64 = kOut;
      ctx.r4.u64 = rng() % 70;
      ctx.r5.u64 = rng() % 3 ? kWritten : 0;
      ctx.r6.u64 = kWide;
      ctx.r7.u64 = rng() % 130;
    }),
    CASE(RtlTimeToTimeFields, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      // Up to the year 9999; a quarter of the draws are the first days after 1601 or around the Unix epoch.
      uint64_t time = (uint64_t(rng()) << 32 | rng()) % 2650467743999999999ull;
      if (rng() % 4 == 0) time = rng() % 2 ? rng() % 10000000000ull : 116444736000000000ull + rng() % 1000000000000ull;
      s.Quad(kTime, time);
      ctx.r3.u64 = kTime;
      ctx.r4.u64 = kFields;
    }),
    CASE(RtlTimeFieldsToTime, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      const bool valid = rng() % 3 != 0;
      const uint16_t values[8] = {
          uint16_t(valid ? 1601 + rng() % 2500 : rng() % 3000), uint16_t(valid ? 1 + rng() % 12 : rng() % 14),
          uint16_t(valid ? 1 + rng() % 28 : rng() % 33),         uint16_t(valid ? rng() % 24 : rng() % 26),
          uint16_t(valid ? rng() % 60 : rng() % 62),             uint16_t(valid ? rng() % 60 : rng() % 62),
          uint16_t(valid ? rng() % 1000 : rng() % 1100),         uint16_t(rng() % 7)};
      for (uint32_t i = 0; i < 8; ++i) s.Half(kFields + 2 * i, values[i]);
      ctx.r3.u64 = kFields;
      ctx.r4.u64 = kTime2;
    }),
    CASE(RtlComputeCrc32, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = rng() % 2 ? 0 : uint32_t(rng());
      ctx.r4.u64 = kBlockB + rng() % 64;
      ctx.r5.u64 = rng() % 4 ? rng() % 300 : 0;
    }),
    CASE(XGetAVPack, nullptr),
    CASE(XGetGameRegion, nullptr),
    CASE(XGetLanguage, nullptr),
    CASE(XamGetSystemVersion, nullptr),
    // Batch 2.
    CASE(RtlNtStatusToDosError, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      static const uint32_t kBases[] = {0x00000103, 0x40000002, 0x40020056, 0x400200AF, 0x80000001, 0x80000288, 0x80090300,
                                        0xC0000001, 0xC0000202, 0xC0020001, 0xC0030001, 0xC0030059, 0xC0050003, 0xC0980001};
      switch (rng() % 5) {
        case 0: ctx.r3.u64 = uint32_t(rng()); break;
        case 1: ctx.r3.u64 = 0x80070000 | rng() % 0x10000; break;
        case 2: ctx.r3.u64 = (rng() % 2 ? 0xD0000000 : 0xC0010000) | rng() % 0x400; break;
        default: ctx.r3.u64 = kBases[rng() % 14] + rng() % 300 - (rng() % 8 == 0 ? 3 : 0);
      }
    }),
    CASE(ExGetXConfigSetting, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = rng() % 4 ? 2 + rng() % 2 : rng() % 6;
      ctx.r4.u64 = rng() % 0x1B;
      ctx.r5.u64 = rng() % 4 ? kOut : 0;
      ctx.r6.u64 = rng() % 6;
      ctx.r7.u64 = rng() % 2 ? kWritten : 0;
    }),
    CASE(RtlInitializeCriticalSection, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kStruct; }),
    CASE(RtlInitializeCriticalSectionAndSpinCount, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = kStruct;
      ctx.r4.u64 = rng() % 2 ? rng() % 70000 : rng() % 1024;
    }),
    CASE(VdQueryVideoMode, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kOut; }),
    CASE(XGetVideoMode, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kOut; }),
    CASE(VdQueryVideoFlags, nullptr),
    CASE(VdGetCurrentDisplayInformation, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kOut; }),
    CASE(VdGetCurrentDisplayGamma, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kOut; ctx.r4.u64 = kOut + 8; }),
    CASE(VdSetDisplayMode, [](Scene&, PPCContext& ctx, std::mt19937& rng) { ctx.r3.u64 = uint32_t(rng()); }),
    CASE(VdEnableDisableClockGating, [](Scene&, PPCContext& ctx, std::mt19937& rng) { ctx.r3.u64 = rng() % 2; }),
    CASE(VdIsHSIOTrainingSucceeded, nullptr),
    CASE(VdRetrainEDRAM, nullptr),
    CASE(VdRetrainEDRAMWorker, nullptr),
    CASE(VdGetGraphicsAsicID, nullptr),
    CASE(VdGetSystemCommandBuffer, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = kOut; ctx.r4.u64 = kOut + 0x100; }),
    CASE(VdCallGraphicsNotificationRoutines, [](Scene&, PPCContext& ctx, std::mt19937&) { ctx.r3.u64 = 1; ctx.r4.u64 = kStruct; }),
    CASE(VdInitializeScalerCommandBuffer, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.Word(ctx.r1.u32 + 0x54 + 2 * 8, kOut);
      s.Word(ctx.r1.u32 + 0x54 + 3 * 8, rng() % 64);
    }),
    CASE(NetDll_inet_addr, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      std::string text;
      const int parts = 1 + rng() % 4;
      for (int i = 0; i < parts; ++i) {
        const uint32_t v = rng() % 8 ? rng() % 256 : rng() % 70000;
        char buffer[32];
        switch (rng() % 6) {
          case 0: std::snprintf(buffer, sizeof buffer, "0x%X", v); break;
          case 1: std::snprintf(buffer, sizeof buffer, "0%o", v); break;
          default: std::snprintf(buffer, sizeof buffer, "%u", v);
        }
        text += (i ? "." : "") + std::string(buffer);
      }
      switch (rng() % 10) {
        case 0: text.clear(); break;
        case 1: text += " "; break;
        case 2: text += "x"; break;
        case 3: text += "."; break;
        default: break;
      }
      std::memcpy(s.base + kString1, text.c_str(), text.size() + 1);
      ctx.r3.u64 = rng() % 16 ? kString1 : 0;
    }),
    CASE(NetDll_XNetInAddrToString, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r4.u64 = uint32_t(rng());
      ctx.r5.u64 = kOut;
      ctx.r6.u64 = rng() % 20;
    }),
    CASE(NetDll_XNetXnAddrToInAddr, nullptr),
    CASE(NetDll_XNetInAddrToXnAddr, nullptr),
    CASE(NetDll___WSAFDIsSet, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      const uint32_t socket = 0xF000 + rng() % 16;
      s.Word(kStruct, rng() % 70);
      for (uint32_t i = 0; i < 64; ++i) s.Word(kStruct + 4 + 4 * i, 0xF000 + rng() % 16);
      ctx.r3.u64 = socket;
      ctx.r4.u64 = kStruct;
    }),
};

struct Result {
  uint64_t r3;
  std::vector<uint8_t> a, b;
};

Result Run(const Case& c, Function f, uint32_t trial, std::vector<uint8_t>& image) {
  // The same seed for both runs: the same scene and registers.
  std::mt19937 rng(trial * 7919u + 17u);
  auto& memory = any360::test::GuestMemory::Get();
  uint8_t* base = memory.base();
  for (uint32_t i = 0; i < kBlockSize; i += 4) {
    const uint32_t v = rng();
    std::memcpy(base + kBlockA + i, &v, 4);
    const uint32_t w = rng();
    std::memcpy(base + kBlockB + i, &w, 4);
  }
  memory.Fill(kStack, kStackSize, 0);
  PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.r1.u64 = kStack + kStackSize - 0x1000;
  ctx.r3.u64 = kBlockA;
  ctx.r4.u64 = kBlockB;
  for (PPCRegister* reg : {&ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10}) reg->u64 = rng() % 64;
  ctx.lr = 0x82000000;
  Scene scene{base};
  if (c.prepare) c.prepare(scene, ctx, rng);
  image.assign(base + kBlockA, base + kBlockA + kBlockSize);
  image.insert(image.end(), base + kBlockB, base + kBlockB + kBlockSize);
  f(ctx, base);
  Result r;
  r.r3 = ctx.r3.u64;
  r.a.assign(base + kBlockA, base + kBlockA + kBlockSize);
  r.b.assign(base + kBlockB, base + kBlockB + kBlockSize);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  int trials = 64;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--trials") && i + 1 < argc) trials = std::atoi(argv[++i]);
  }
  int failures = 0;
  for (const Case& c : kCases) {
    bool ok = true;
    for (int trial = 0; trial < trials && ok; ++trial) {
      std::vector<uint8_t> image_native, image_reference;
      const Result native = Run(c, c.native, uint32_t(trial), image_native);
      const Result reference = Run(c, c.reference, uint32_t(trial), image_reference);
      if (native.r3 != reference.r3) {
        std::printf("%s: trial %d: r3 native %#llx, reference %#llx\n", c.name, trial, (unsigned long long)native.r3,
                    (unsigned long long)reference.r3);
        ok = false;
      }
      for (uint32_t i = 0; i < kBlockSize && ok; ++i) {
        if (native.a[i] != reference.a[i]) {
          std::printf("%s: trial %d: byte %08X native %02X, reference %02X\n", c.name, trial, kBlockA + i, native.a[i], reference.a[i]);
          ok = false;
        } else if (native.b[i] != reference.b[i]) {
          std::printf("%s: trial %d: byte %08X native %02X, reference %02X\n", c.name, trial, kBlockB + i, native.b[i], reference.b[i]);
          ok = false;
        }
      }
    }
    if (ok) std::printf("%s: %d trials equal\n", c.name, trials);
    else ++failures;
  }
  std::printf("sys_io_tests: %zu system calls, %d failing\n", sizeof(kCases) / sizeof(kCases[0]), failures);
  return failures ? 1 : 0;
}
