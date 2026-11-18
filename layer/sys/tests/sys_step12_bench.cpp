// sys_step12_bench: step 12's calls (docs/SYS_STUBS.md) timed, nanoseconds and host memory a call. The reference
// stubs them (it returns at once and does nothing), so there is no original to time against; where the host already
// does the same work, its own version is timed on the same data beside Any360's: Windows' CNG (BCrypt) for the hashes,
// HMACs and DES, and the C library's wcslen for lstrlenW (host-order strings: it cannot read a guest's big-endian
// ones, so it is a bound, not a replacement). The Win32 forms that block (Sleep, SleepEx, WaitForSingleObject(Ex),
// CreateThread, ResumeThread, ExitThread) are their kernel call plus the Win32 part timed here (timeout, result, last
// error); the kernel calls keep their own measures.
//
//   sys_step12_bench [repeats]   (results kept in docs/perf/archive/<date>-sys-step12/)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/crypt.h"
#include "any360/sys/objects.h"
#include "any360/sys/threading.h"
#include "any360/sys/win32.h"
#include "any360/sys/xam.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

using namespace any360::sys;

namespace {

std::vector<uint8_t> g_memory(1u << 20);
const Guest g{g_memory.data()};
constexpr uint32_t kIn = 0x10000, kOut = 0x1000, kState = 0x2000, kKey = 0x3000, kFeed = 0x3100, kText = 0x4000;
volatile uint32_t g_sink = 0;
uint32_t g_repeats = 9;

struct Totals {
  uint64_t calls = 0, bytes = 0, count = 0;
} g_totals;

// One version's measure: nanoseconds a call (the fastest run) and host heap a call (bytes, allocations: the mean over
// every run, from any360/sys/alloc_meter.h).
struct Measure {
  double ns = 0, bytes = 0, allocations = 0;
  bool valid() const { return ns > 0; }
};

// `g_repeats` runs of `n` calls. Any360's calls (`ours`) also go into the totals.
template <typename F>
Measure Time(uint32_t n, F&& call, bool ours = true) {
  Measure m{1e30, 0, 0};
  uint64_t calls = 0, bytes = 0, count = 0;
  for (uint32_t r = 0; r < g_repeats; ++r) {
    const AllocCounts before = ThreadAllocations();
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < n; ++i) call();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
    const AllocCounts spent = ThreadAllocations() - before;
    calls += n, bytes += spent.bytes, count += spent.count;
    m.ns = std::min(m.ns, ns);
  }
  m.bytes = double(bytes) / double(calls), m.allocations = double(count) / double(calls);
  if (ours) g_totals.calls += calls, g_totals.bytes += bytes, g_totals.count += count;
  return m;
}

uint32_t CallsFor(uint32_t size) { return std::clamp<uint32_t>((8u << 20) / (size + 64), 64, 200000); }

#if defined(_WIN32)
// One CNG algorithm, opened once; hashes, HMACs and DES timed through its one-call and key APIs.
struct Cng {
  BCRYPT_ALG_HANDLE alg = nullptr;
  Cng(const wchar_t* id, ULONG flags) { BCryptOpenAlgorithmProvider(&alg, id, nullptr, flags); }
  ~Cng() {
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  }
  void Hash(const uint8_t* key, uint32_t key_size, const uint8_t* data, uint32_t size, uint8_t* out, uint32_t out_size) const {
    BCryptHash(alg, const_cast<PUCHAR>(key), key_size, const_cast<PUCHAR>(data), size, out, out_size);
  }
};
#endif

void PrintRow(const char* label, uint32_t size, const Measure& ours, const Measure& host) {
  std::printf("  %-36s %8u | %9.1f %9.1f %7.2f |", label, size, ours.ns, ours.bytes, ours.allocations);
  if (host.valid()) {
    std::printf(" %9.1f %9.1f %7.2f | %6.2fx\n", host.ns, host.bytes, host.allocations, host.ns / ours.ns);
  } else {
    std::printf(" %9s %9s %7s |\n", "-", "-", "-");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_repeats = std::max(1, std::atoi(argv[1]));
  for (uint32_t i = 0; i < (1u << 20) - kIn; ++i) g_memory[kIn + i] = uint8_t(i * 131 + 7);
  std::memset(g.host(kKey), 0x0b, 64);
  const std::vector<uint32_t> sizes = {0, 16, 64, 256, 1024, 4096, 65536};
  std::printf("sys_step12_bench: ns a call (fastest of %u runs); host = what the host offers for the same work "
              "(ratio > 1: Any360 faster)\n",
              g_repeats);
  std::printf("  %-36s %8s | %9s %9s %7s | %9s %9s %7s | %7s\n", "call (guest state it uses)", "bytes", "ns", "B/call",
              "allocs", "host ns", "B/call", "allocs", "speed");

#if defined(_WIN32)
  const Cng md5(BCRYPT_MD5_ALGORITHM, 0), sha256(BCRYPT_SHA256_ALGORITHM, 0);
  const Cng hmac_md5(BCRYPT_MD5_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG), hmac_sha1(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
#endif
  uint8_t host_out[64];
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] { XeCryptMd5(g, kIn, size, 0, 0, 0, 0, kOut, 16); });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { md5.Hash(nullptr, 0, g.host(kIn), size, host_out, 16); }, false);
#endif
    PrintRow("XeCryptMd5", size, ours, host);
  }
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] { XeCryptSha256(g, kIn, size, 0, 0, 0, 0, kOut, 32); });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { sha256.Hash(nullptr, 0, g.host(kIn), size, host_out, 32); }, false);
#endif
    PrintRow("XeCryptSha256", size, ours, host);
  }
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] { XeCryptHmacMd5(g, kKey, 16, kIn, size, 0, 0, 0, 0, kOut, 16); });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { hmac_md5.Hash(g.host(kKey), 16, g.host(kIn), size, host_out, 16); }, false);
#endif
    PrintRow("XeCryptHmacMd5", size, ours, host);
  }
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] {
      XeCryptHmacShaInit(g, kState, kKey, 20);
      XeCryptHmacShaUpdate(g, kState, kIn, size);
      XeCryptHmacShaFinal(g, kState, kOut, 20);
    });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { hmac_sha1.Hash(g.host(kKey), 20, g.host(kIn), size, host_out, 20); }, false);
#endif
    PrintRow("HmacSha Init+Update+Final (0xB0)", size, ours, host);
  }
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    XeCryptHmacSha(g, kKey, 20, kIn, size, 0, 0, 0, 0, kOut + 0x80, 20);
    PrintRow("XeCryptHmacShaVerify", size,
             Time(n, [&] { g_sink = g_sink + XeCryptHmacShaVerify(g, kKey, 20, kIn, size, 0, 0, 0, 0, kOut + 0x80, 20); }), Measure{});
  }
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    // Streamed in 64-byte pieces, as a title feeding a file through it would.
    PrintRow("Md5 Init+Update/64+Final (0x54)", size, Time(n, [&] {
               XeCryptMd5Init(g, kState);
               for (uint32_t at = 0; at < size; at += 64) XeCryptMd5Update(g, kState, kIn + at, std::min(64u, size - at));
               XeCryptMd5Final(g, kState, kOut, 16);
             }),
             Measure{});
  }

  // DES: the key schedule, one block, CBC over sizes.
  g.Store64(kKey, 0x0123456789ABCDEFull);
  PrintRow("XeCryptDesKey (0x80)", 8, Time(200000, [&] { XeCryptDesKey(g, kState, kKey); }), Measure{});
  XeCryptDesKey(g, kState, kKey);
  PrintRow("XeCryptDesEcb", 8, Time(200000, [&] { XeCryptDesEcb(g, kState, kIn, kOut, 1); }), Measure{});
#if defined(_WIN32)
  BCRYPT_ALG_HANDLE des_alg = nullptr;
  BCRYPT_KEY_HANDLE des_key = nullptr;
  BCryptOpenAlgorithmProvider(&des_alg, BCRYPT_DES_ALGORITHM, nullptr, 0);
  BCryptSetProperty(des_alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                    sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  uint8_t des_key_bytes[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  BCryptGenerateSymmetricKey(des_alg, &des_key, nullptr, 0, des_key_bytes, 8, 0);
  std::vector<uint8_t> host_cipher(65536);
#endif
  for (uint32_t size : {8u, 64u, 1024u, 65536u}) {
    const uint32_t n = CallsFor(size) / 4 + 16;
    const Measure ours = Time(n, [&] {
      g.Store64(kFeed, 0x1234567890ABCDEFull);
      XeCryptDesCbc(g, kState, kIn, size, kOut + 0x100, kFeed, 1);
    });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] {
      uint8_t iv[8] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
      ULONG written = 0;
      BCryptEncrypt(des_key, g.host(kIn), size, nullptr, iv, 8, host_cipher.data(), size, &written, 0);
    }, false);
#endif
    PrintRow("XeCryptDesCbc", size, ours, host);
  }
#if defined(_WIN32)
  BCryptDestroyKey(des_key);
  BCryptCloseAlgorithmProvider(des_alg, 0);
#endif

  // The Win32 forms: what they add over their kernel call.
  const uint32_t kthread = kState + 0x400;
  PrintRow("GetTickCount", 0, Time(200000, [&] { g_sink = g_sink + TickCount(); }), Measure{});
  PrintRow("GetLastError", 0, Time(200000, [&] { g_sink = g_sink + Win32LastError(g, kthread); }), Measure{});
  PrintRow("RtlSetLastNTError (failed waits)", 0, Time(200000, [&] { g_sink = g_sink + SetWin32LastNtError(g, kthread, 0xC0000008); }), Measure{});
  PrintRow("Wait result (WaitForSingleObject)", 0, Time(200000, [&] { g_sink = g_sink + Win32WaitResult(g, kthread, 0x102); }), Measure{});
  PrintRow("Timeout (Sleep, waits)", 0, Time(200000, [&] { g_sink = g_sink + uint32_t(Win32Timeout(1500).value_or(0)); }), Measure{});
  PrintRow("GetSystemTimeAsFileTime", 0, Time(200000, [&] { g.Store64(kOut, SystemTime()); }), Measure{});
  PrintRow("QueryPerformanceFrequency", 0, Time(200000, [&] { g.Store64(kOut, KeQueryPerformanceFrequency()); }), Measure{});
  std::memcpy(g.host(kText), "xam.xex", 8);
  PrintRow("GetModuleHandleA", 0, Time(200000, [&] { g_sink = g_sink + XamState().ModuleHandle(g, kText); }), Measure{});
  for (uint32_t length : {0u, 8u, 64u, 256u}) {
    for (uint32_t i = 0; i < length; ++i) g.Store16(kText + 2 * i, uint16_t('a' + i % 26));
    g.Store16(kText + 2 * length, 0);
    std::wstring host_text(length, L'a');
    const Measure ours = Time(200000, [&] { g_sink = g_sink + LstrlenW(g, kText); });
    const Measure host = Time(200000, [&] { g_sink = g_sink + uint32_t(std::wcslen(host_text.c_str())); }, false);
    PrintRow("lstrlenW (host: wcslen)", length, ours, host);
  }
  std::printf("sys_step12_bench: allocated per call %.2f B in %.4f allocations over %llu timed calls\n",
              g_totals.calls ? double(g_totals.bytes) / double(g_totals.calls) : 0.0,
              g_totals.calls ? double(g_totals.count) / double(g_totals.calls) : 0.0,
              static_cast<unsigned long long>(g_totals.calls));
  return 0;
}
