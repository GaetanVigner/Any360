// The oracle for the system calls that need a running kernel: the reference runtime (ReXGlue) is set up in tool mode
// (memory, kernel state, no GPU or audio), a guest thread is created and this test runs on it, so the reference
// implementations find their current thread, PCR and process. Each case runs the native and the reference
// implementation from the same state (two seeded guest blocks, the PCR, the KTHREAD and the KPROCESS are snapshotted
// and restored in between) and compares r3 and every one of those regions byte for byte.
// Run: sys_runtime_tests [--trials N]
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "any360/sys/exports.h"

#define REFERENCE(name) extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base);
REFERENCE(KeGetCurrentProcessType)
REFERENCE(KeGetImagePageTableEntry)
REFERENCE(RtlCopyString)
REFERENCE(RtlCopyUnicodeString)
REFERENCE(RtlImageNtHeader)
REFERENCE(ExAcquireReadWriteLockExclusive)
REFERENCE(ExTryToAcquireReadWriteLockExclusive)
REFERENCE(ExAcquireReadWriteLockShared)
REFERENCE(ExTryToAcquireReadWriteLockShared)
REFERENCE(ExReleaseReadWriteLock)
REFERENCE(InterlockedPushEntrySList)
REFERENCE(InterlockedPopEntrySList)
REFERENCE(InterlockedFlushSList)
REFERENCE(KeSetCurrentProcessType)
REFERENCE(KeRaiseIrqlToDpcLevel)
REFERENCE(KfLowerIrql)
REFERENCE(KfRaiseIrql)
REFERENCE(KfAcquireSpinLock)
REFERENCE(KfReleaseSpinLock)
REFERENCE(KeAcquireSpinLockAtRaisedIrql)
REFERENCE(KeTryToAcquireSpinLockAtRaisedIrql)
REFERENCE(KeReleaseSpinLockFromRaisedIrql)
REFERENCE(RtlEnterCriticalSection)
REFERENCE(RtlTryEnterCriticalSection)
REFERENCE(RtlLeaveCriticalSection)
REFERENCE(KeEnterCriticalRegion)
REFERENCE(KeLeaveCriticalRegion)
REFERENCE(KeTlsAlloc)
REFERENCE(KeTlsFree)
REFERENCE(KeTlsGetValue)
REFERENCE(KeTlsSetValue)
REFERENCE(KeQueryPerformanceFrequency)

namespace {

using Function = void (*)(PPCContext&, uint8_t*);
constexpr uint32_t kBlockSize = 0x4000, kPcrSize = 0x2D8, kThreadSize = 0x100, kProcessSize = 0x60;
constexpr uint32_t kEntry = 0x82000000;

struct Scene {
  uint8_t* base;
  uint32_t a, b, pcr, thread, process;  // guest addresses
  void Word(uint32_t at, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + at, &v, 4);
  }
  uint32_t Read(uint32_t at) const {
    uint32_t v;
    std::memcpy(&v, base + at, 4);
    return __builtin_bswap32(v);
  }
};

using Prepare = std::function<void(Scene&, PPCContext&, std::mt19937&)>;

struct Case {
  const char* name;
  Function native, reference;
  Prepare prepare;
};

Function Native(const char* name) {
  for (unsigned i = 0; i < any360::sys::kExportCount; ++i) {
    if (!std::strcmp(any360::sys::kExports[i].name, name)) return any360::sys::kExports[i].function;
  }
  std::printf("sys_runtime_tests: %s is not in any360::sys::kExports\n", name);
  std::exit(2);
}

#define CASE(name, ...) {#name, Native(#name), &__imp__##name, __VA_ARGS__}

// A critical section at block A: free, or owned by this thread (recursion n, lock count n - 1), or owned by another.
void CriticalSection(Scene& s, std::mt19937& rng, int owner) {
  s.Word(s.a + 0, 0x01000000u | (rng() % 4 << 16));
  s.Word(s.a + 4, 0);
  uint32_t recursion = 0, count = 0xFFFFFFFF, thread = 0;
  if (owner) {
    recursion = 1 + rng() % 3;
    count = recursion - 1;
    thread = owner == 1 ? s.thread : s.b + 0x100;
  }
  std::memcpy(s.base + s.a + 0x10, &count, 4);  // host order
  s.Word(s.a + 0x14, recursion);
  s.Word(s.a + 0x18, thread);
}

// An ERWLOCK at block A with random counts and a free spin lock.
void RwLock(Scene& s, std::mt19937& rng) {
  s.Word(s.a, uint32_t(int32_t(rng() % 4) - 1));
  s.Word(s.a + 4, rng() % 3);
  s.Word(s.a + 8, rng() % 3);
  s.Word(s.a + 0xC, rng() % 3);
  s.Word(s.a + 0x34, 0);
}

// An SLIST_HEADER at block A heading a chain of entries in block B (or empty).
void SList(Scene& s, std::mt19937& rng) {
  const uint32_t n = rng() % 5;
  uint32_t next = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t entry = s.b + 0x100 + 8 * i;
    s.Word(entry, next);
    next = entry;
  }
  s.Word(s.a, next);
  s.Word(s.a + 4, n << 16 | (rng() & 0xFFFF));
}

// Two counted strings at block A (destination, source) with buffers in block B.
void CountedStrings(Scene& s, PPCContext& ctx, std::mt19937& rng) {
  s.Word(s.a, uint32_t(rng() % 40) << 16 | uint32_t(rng() % 40));
  s.Word(s.a + 4, s.b + 0x1000);
  s.Word(s.a + 8, uint32_t(rng() % 40) << 16 | uint32_t(rng() % 40));
  s.Word(s.a + 12, s.b + 0x2000);
  ctx.r3.u64 = s.a;
  ctx.r4.u64 = rng() % 6 ? s.a + 8 : 0;
}

const Case kCases[] = {
    CASE(KeGetCurrentProcessType, [](Scene& s, PPCContext&, std::mt19937& rng) {
      s.Word(s.pcr + 0x150, rng() % 2);
      s.base[s.pcr + 0xC] = uint8_t(rng() % 3);
      s.base[s.thread + 0x73] = uint8_t(rng() % 3);
    }),
    CASE(KeSetCurrentProcessType, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.Word(s.pcr + 0x150, rng() % 2);
      ctx.r3.u64 = rng() % 3;
    }),
    CASE(KeRaiseIrqlToDpcLevel, [](Scene& s, PPCContext&, std::mt19937& rng) { s.base[s.pcr + 0x18] = uint8_t(rng() % 4); }),
    CASE(KfRaiseIrql, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.base[s.pcr + 0x18] = uint8_t(rng() % 4);
      ctx.r3.u64 = rng() % 4;
    }),
    CASE(KfLowerIrql, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.base[s.pcr + 0x18] = uint8_t(rng() % 4);
      ctx.r3.u64 = rng() % 3;
    }),
    CASE(KfAcquireSpinLock, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.base[s.pcr + 0x18] = uint8_t(rng() % 3);
      s.Word(s.a, 0);
      ctx.r3.u64 = s.a;
    }),
    CASE(KfReleaseSpinLock, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.base[s.pcr + 0x18] = 2;
      s.Word(s.a, s.pcr);
      ctx.r3.u64 = s.a;
      ctx.r4.u64 = rng() % 3;
    }),
    CASE(KeAcquireSpinLockAtRaisedIrql, [](Scene& s, PPCContext& ctx, std::mt19937&) {
      s.Word(s.a, 0);
      ctx.r3.u64 = s.a;
    }),
    CASE(KeTryToAcquireSpinLockAtRaisedIrql, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      s.Word(s.a, rng() % 2 ? 0 : s.b);
      ctx.r3.u64 = s.a;
    }),
    CASE(KeReleaseSpinLockFromRaisedIrql, [](Scene& s, PPCContext& ctx, std::mt19937&) {
      s.Word(s.a, s.pcr);
      ctx.r3.u64 = s.a;
    }),
    CASE(RtlEnterCriticalSection, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      CriticalSection(s, rng, rng() % 2);  // free or ours: never another thread's (that would wait)
      ctx.r3.u64 = s.a;
    }),
    CASE(RtlTryEnterCriticalSection, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      CriticalSection(s, rng, int(rng() % 3));
      ctx.r3.u64 = s.a;
    }),
    CASE(RtlLeaveCriticalSection, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      CriticalSection(s, rng, 1);
      ctx.r3.u64 = s.a;
    }),
    CASE(KeEnterCriticalRegion, nullptr),
    CASE(KeLeaveCriticalRegion, [](Scene& s, PPCContext&, std::mt19937& rng) {
      const int32_t count = -int32_t(1 + rng() % 3);
      std::memcpy(s.base + s.thread + 0xB0, &count, 4);
    }),
    CASE(KeTlsAlloc, [](Scene& s, PPCContext&, std::mt19937& rng) {
      for (uint32_t i = 0; i < 8; ++i) s.Word(s.process + 0x30 + 4 * i, rng() % 4 ? uint32_t(rng()) : 0);
    }),
    CASE(KeTlsFree, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      for (uint32_t i = 0; i < 8; ++i) s.Word(s.process + 0x30 + 4 * i, uint32_t(rng()));
      ctx.r3.u64 = rng() % 8 ? rng() % 300 : 0xFFFFFFFF;
    }),
    CASE(KeTlsGetValue, [](Scene&, PPCContext& ctx, std::mt19937& rng) { ctx.r3.u64 = rng() % 1100; }),
    CASE(KeTlsSetValue, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      ctx.r3.u64 = rng() % 1100;
      ctx.r4.u64 = uint32_t(rng());
    }),
    CASE(KeQueryPerformanceFrequency, nullptr),
    // Addresses inside and around the executable's range (the reference looks its heaps up).
    CASE(KeGetImagePageTableEntry, [](Scene&, PPCContext& ctx, std::mt19937& rng) {
      static constexpr uint32_t kBases[] = {0x70000000, 0x80000000, 0x82000000, 0x8C000000, 0x90000000, 0x9FFF0000, 0xA0000000};
      ctx.r3.u64 = kBases[rng() % 7] + rng() % 0x1000000;
    }),
    // Counted strings at block A whose buffers are in block B (the reference translates them through its runtime's
    // memory); a null source sometimes.
    CASE(RtlCopyString, [](Scene& s, PPCContext& ctx, std::mt19937& rng) { CountedStrings(s, ctx, rng); }),
    CASE(RtlCopyUnicodeString, [](Scene& s, PPCContext& ctx, std::mt19937& rng) { CountedStrings(s, ctx, rng); }),
    CASE(RtlImageNtHeader, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      // An image at block B: "MZ", e_lfanew (little-endian) to a "PE" signature, each spoiled now and then.
      if (rng() % 4) {
        s.base[s.b] = 'M';
        s.base[s.b + 1] = 'Z';
      }
      const uint32_t lfanew = 0x40 + 8 * (rng() % 64);
      std::memcpy(s.base + s.b + 0x3C, &lfanew, 4);
      if (rng() % 4) std::memcpy(s.base + s.b + lfanew, "PE\0\0", 4);
      ctx.r3.u64 = rng() % 8 ? s.b : 0;
    }),
    // Read/write locks in the states where no call waits: counts at block A, the spin lock free. (Initializing one is
    // checked in sys_object_tests: the reference keeps its host objects in the event's and semaphore's headers.)
    CASE(ExTryToAcquireReadWriteLockExclusive, [](Scene& s, PPCContext&, std::mt19937& rng) { RwLock(s, rng); }),
    CASE(ExTryToAcquireReadWriteLockShared, [](Scene& s, PPCContext&, std::mt19937& rng) { RwLock(s, rng); }),
    CASE(ExAcquireReadWriteLockExclusive, [](Scene& s, PPCContext&, std::mt19937& rng) {
      RwLock(s, rng);
      s.Word(s.a, 0xFFFFFFFF);  // free
    }),
    CASE(ExAcquireReadWriteLockShared, [](Scene& s, PPCContext&, std::mt19937& rng) {
      RwLock(s, rng);
      if (rng() % 2) {
        s.Word(s.a, 0xFFFFFFFF);  // free
      } else {
        s.Word(s.a + 4, 0);  // readers in, no writer waiting
        s.Word(s.a + 0xC, 1 + rng() % 3);
      }
    }),
    CASE(ExReleaseReadWriteLock, [](Scene& s, PPCContext&, std::mt19937& rng) {
      RwLock(s, rng);
      s.Word(s.a + 8, 0);  // no reader to let in (that releases the semaphore)
      if (rng() % 2) {
        s.Word(s.a, 0);  // the last holder
      } else {
        s.Word(s.a + 0xC, 2 + rng() % 3);  // one reader of several leaves
      }
    }),
    CASE(InterlockedPushEntrySList, [](Scene& s, PPCContext& ctx, std::mt19937& rng) {
      SList(s, rng);
      ctx.r4.u64 = s.b + 0x800 + 8 * (rng() % 16);
    }),
    CASE(InterlockedPopEntrySList, [](Scene& s, PPCContext&, std::mt19937& rng) { SList(s, rng); }),
    CASE(InterlockedFlushSList, [](Scene& s, PPCContext&, std::mt19937& rng) { SList(s, rng); }),
};

struct Snapshot {
  std::vector<uint8_t> a, b, pcr, thread, process;
  uint64_t r3 = 0;
};

void Capture(const Scene& s, Snapshot& out) {
  out.a.assign(s.base + s.a, s.base + s.a + kBlockSize);
  out.b.assign(s.base + s.b, s.base + s.b + kBlockSize);
  out.pcr.assign(s.base + s.pcr, s.base + s.pcr + kPcrSize);
  out.thread.assign(s.base + s.thread, s.base + s.thread + kThreadSize);
  out.process.assign(s.base + s.process, s.base + s.process + kProcessSize);
}

void Restore(const Scene& s, const Snapshot& in) {
  std::memcpy(s.base + s.a, in.a.data(), kBlockSize);
  std::memcpy(s.base + s.b, in.b.data(), kBlockSize);
  std::memcpy(s.base + s.pcr, in.pcr.data(), kPcrSize);
  std::memcpy(s.base + s.thread, in.thread.data(), kThreadSize);
  std::memcpy(s.base + s.process, in.process.data(), kProcessSize);
}

// The TLS block of this thread, seeded like the blocks so a value read back is checkable.
void SeedTls(Scene& s, std::mt19937& rng) {
  const uint32_t tls = s.Read(s.thread + 0x68);
  for (uint32_t i = 0; i < 1024; ++i) s.Word(tls + 4 * i, uint32_t(rng()));
}

bool Compare(const char* name, int trial, const char* region, const std::vector<uint8_t>& n, const std::vector<uint8_t>& r, uint32_t at) {
  for (size_t i = 0; i < n.size(); ++i) {
    if (n[i] != r[i]) {
      std::printf("%s: trial %d: %s byte %08X native %02X, reference %02X\n", name, trial, region, at + uint32_t(i), n[i], r[i]);
      return false;
    }
  }
  return true;
}

int g_trials = 64;
std::atomic<int> g_failures{-1};

// Runs on the guest thread the reference runtime created for us.
void Entry(PPCContext& ctx, uint8_t* base) {
  auto* runtime = rex::Runtime::instance();
  Scene scene{base, 0, 0, 0, 0, 0};
  scene.a = runtime->memory()->SystemHeapAlloc(kBlockSize, 0x1000);
  scene.b = runtime->memory()->SystemHeapAlloc(kBlockSize, 0x1000);
  scene.pcr = ctx.r13.u32;
  scene.thread = scene.Read(scene.pcr + 0x100);
  scene.process = scene.Read(scene.thread + 0x84);
  std::printf("sys_runtime_tests: blocks %08X %08X, pcr %08X, thread %08X, process %08X\n", scene.a, scene.b, scene.pcr,
              scene.thread, scene.process);
  const uint64_t saved[] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  int failures = 0;
  for (const Case& c : kCases) {
    bool ok = true;
    for (int trial = 0; trial < g_trials && ok; ++trial) {
      std::mt19937 rng(trial * 7919u + 23u);
      for (uint32_t i = 0; i < kBlockSize; i += 4) {
        scene.Word(scene.a + i, rng());
        scene.Word(scene.b + i, rng());
      }
      SeedTls(scene, rng);
      ctx.r3.u64 = scene.a;
      ctx.r4.u64 = scene.b;
      for (PPCRegister* reg : {&ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10}) reg->u64 = rng() % 64;
      if (c.prepare) c.prepare(scene, ctx, rng);
      Snapshot initial;
      Capture(scene, initial);
      const uint64_t registers[] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64};

      c.native(ctx, base);
      Snapshot native;
      Capture(scene, native);
      native.r3 = ctx.r3.u64;

      Restore(scene, initial);
      ctx.r3.u64 = registers[0];
      ctx.r4.u64 = registers[1];
      ctx.r5.u64 = registers[2];
      ctx.r6.u64 = registers[3];
      ctx.r7.u64 = registers[4];
      ctx.r8.u64 = registers[5];
      c.reference(ctx, base);
      Snapshot reference;
      Capture(scene, reference);
      reference.r3 = ctx.r3.u64;

      if (native.r3 != reference.r3) {
        std::printf("%s: trial %d: r3 native %#llx, reference %#llx\n", c.name, trial, (unsigned long long)native.r3,
                    (unsigned long long)reference.r3);
        ok = false;
      }
      ok = ok && Compare(c.name, trial, "block A", native.a, reference.a, scene.a) &&
           Compare(c.name, trial, "block B", native.b, reference.b, scene.b) &&
           Compare(c.name, trial, "PCR", native.pcr, reference.pcr, scene.pcr) &&
           Compare(c.name, trial, "KTHREAD", native.thread, reference.thread, scene.thread) &&
           Compare(c.name, trial, "KPROCESS", native.process, reference.process, scene.process);
      Restore(scene, initial);  // leave the thread as it was
    }
    if (ok) std::printf("%s: %d trials equal\n", c.name, g_trials);
    else ++failures;
  }
  std::printf("sys_runtime_tests: %zu system calls, %d failing\n", sizeof(kCases) / sizeof(kCases[0]), failures);
  std::fflush(stdout);
  ctx.r3.u64 = saved[0];
  g_failures = failures;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--trials") && i + 1 < argc) g_trials = std::atoi(argv[++i]);
  }
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "any360-sys-runtime-tests";
  std::filesystem::create_directories(root);
  rex::Runtime runtime(root);
  rex::RuntimeConfig config;
  config.tool_mode = true;
  if (runtime.Setup(std::move(config)) != 0) {
    std::printf("sys_runtime_tests: the reference runtime did not set up\n");
    return 2;
  }
  auto* dispatcher = runtime.function_dispatcher();
  if (!dispatcher->InitializeFunctionTable(kEntry, 0x10000, kEntry, 0x20000, true) || !dispatcher->SetFunction(kEntry, &Entry)) {
    std::printf("sys_runtime_tests: cannot register the entry function\n");
    return 2;
  }
  auto thread = rex::system::make_object<rex::system::XThread>(runtime.kernel_state(), 256 * 1024, 0, kEntry, 0, 0, true, true);
  if (thread->Create() != 0) {
    std::printf("sys_runtime_tests: cannot create the guest thread\n");
    return 2;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (g_failures.load() < 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const int failures = g_failures.load();
  std::fflush(stdout);
  // The runtime's teardown is not needed for the verdict; leave the process without it.
  std::_Exit(failures < 0 ? 3 : failures ? 1 : 0);
}
