// sys_step12_bench: step 12's calls (docs/SYS_STUBS.md) timed, nanoseconds and host memory a call. The reference
// stubs them (it returns at once and does nothing), so there is no original to time against; where the host already
// does the same work, its own version is timed on the same data beside Any360's: Windows' CNG (BCrypt) for the hashes,
// HMACs and DES, and the C library's wcslen on a host string for lstrlenW (which runs on the host's own 16-bit scan:
// a zero unit is zero in either byte order). The SHA rows run on each SHA path the machine has (crypt.h: ShaPath):
// the default, then [SHA-NI], [host] (CNG) and [C++]. The allocation meter counts operator new only: the host
// library's own heap use (CNG's columns, the [host] rows) is not measured. The Win32 forms that block (Sleep, SleepEx, WaitForSingleObject(Ex),
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
#include "any360/sys/files.h"
#include <filesystem>
#include <fstream>
#include <cwctype>
#include "any360/sys/variables.h"
#include "any360/sys/rtl.h"
#include "any360/sys/info.h"
#include "any360/sys/ldi.h"
#include "any360/sys/memory.h"
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
#undef CreateEvent  // windows.h's macros would rename the object manager's and the file system's methods
#undef CreateFile
#include "lzx_cab.h"
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
  std::printf("sys_step12_bench: SHA paths: processor (SHA-NI) %s, host (CNG) %s; default %s. Rows marked [SHA-NI], [host] "
              "or [C++] force that path (SetShaPath)\n",
              ShaPathAvailable(ShaPath::kProcessor) ? "yes" : "no", ShaPathAvailable(ShaPath::kHost) ? "yes" : "no",
              DefaultShaPath() == ShaPath::kProcessor ? "processor" : DefaultShaPath() == ShaPath::kHost ? "host" : "C++");
  std::printf("  %-36s %8s | %9s %9s %7s | %9s %9s %7s | %7s\n", "call (guest state it uses)", "bytes", "ns", "B/call",
              "allocs", "host ns", "B/call", "allocs", "speed");
  // The SHA rows on the default path first, then on every other path the machine has.
  std::vector<ShaPath> sha_paths = {DefaultShaPath()};
  for (const ShaPath path : {ShaPath::kProcessor, ShaPath::kHost, ShaPath::kPortable}) {
    if (path != DefaultShaPath() && ShaPathAvailable(path)) sha_paths.push_back(path);
  }
  auto label = [](const char* name, ShaPath path) {
    const char* tag = path == DefaultShaPath()     ? ""
                      : path == ShaPath::kHost     ? " [host]"
                      : path == ShaPath::kPortable ? " [C++]"
                                                   : " [SHA-NI]";
    return std::string(name) + tag;
  };

#if defined(_WIN32)
  const Cng md5(BCRYPT_MD5_ALGORITHM, 0), sha1(BCRYPT_SHA1_ALGORITHM, 0), sha256(BCRYPT_SHA256_ALGORITHM, 0);
  const Cng hmac_md5(BCRYPT_MD5_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG), hmac_sha1(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
#endif
  [[maybe_unused]] uint8_t host_out[64];
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] { XeCryptMd5(g, kIn, size, 0, 0, 0, 0, kOut, 16); });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { md5.Hash(nullptr, 0, g.host(kIn), size, host_out, 16); }, false);
#endif
    PrintRow("XeCryptMd5", size, ours, host);
  }
  for (const ShaPath sha : sha_paths) {
    SetShaPath(sha);
    for (uint32_t size : sizes) {
      const uint32_t n = CallsFor(size);
      const Measure ours = Time(n, [&] { XeCryptSha(g, kIn, size, 0, 0, 0, 0, kOut, 20); });
      Measure host;
#if defined(_WIN32)
      host = Time(n, [&] { sha1.Hash(nullptr, 0, g.host(kIn), size, host_out, 20); }, false);
#endif
      PrintRow(label("XeCryptSha", sha).c_str(), size, ours, host);
    }
    for (uint32_t size : sizes) {
      const uint32_t n = CallsFor(size);
      const Measure ours = Time(n, [&] { XeCryptSha256(g, kIn, size, 0, 0, 0, 0, kOut, 32); });
      Measure host;
#if defined(_WIN32)
      host = Time(n, [&] { sha256.Hash(nullptr, 0, g.host(kIn), size, host_out, 32); }, false);
#endif
      PrintRow(label("XeCryptSha256", sha).c_str(), size, ours, host);
    }
  }
  SetShaPath(DefaultShaPath());
  for (uint32_t size : sizes) {
    const uint32_t n = CallsFor(size);
    const Measure ours = Time(n, [&] { XeCryptHmacMd5(g, kKey, 16, kIn, size, 0, 0, 0, 0, kOut, 16); });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] { hmac_md5.Hash(g.host(kKey), 16, g.host(kIn), size, host_out, 16); }, false);
#endif
    PrintRow("XeCryptHmacMd5", size, ours, host);
  }
  // A new key every call (the pad states kept for a repeated key do not help): the cost of a first use.
  for (uint32_t size : {0u, 256u}) {
    uint8_t flip = 0;
    PrintRow("XeCryptHmacMd5 (new key each call)", size, Time(200000, [&] {
               g.Store8(kKey, ++flip);
               XeCryptHmacMd5(g, kKey, 16, kIn, size, 0, 0, 0, 0, kOut, 16);
             }),
             Measure{});
    PrintRow("XeCryptHmacSha (new key each call)", size, Time(200000, [&] {
               g.Store8(kKey, ++flip);
               XeCryptHmacSha(g, kKey, 20, kIn, size, 0, 0, 0, 0, kOut, 20);
             }),
             Measure{});
  }
  std::memset(g.host(kKey), 0x0b, 64);
  for (const ShaPath sha : sha_paths) {
    SetShaPath(sha);
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
      PrintRow(label("HmacSha Init+Update+Final (0xB0)", sha).c_str(), size, ours, host);
    }
  }
  SetShaPath(DefaultShaPath());
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
  for (uint32_t size : {8u, 1024u}) {
    const uint32_t n = CallsFor(size) / 4 + 16;
    PrintRow("XeCryptDesCbc decrypt", size, Time(n, [&] {
               g.Store64(kFeed, 0x1234567890ABCDEFull);
               XeCryptDesCbc(g, kState, kIn, size, kOut + 0x100, kFeed, 0);
             }),
             Measure{});
  }
#if defined(_WIN32)
  BCryptDestroyKey(des_key);
  BCryptCloseAlgorithmProvider(des_alg, 0);
#endif
  for (int i = 0; i < 3; ++i) g.Store64(kKey + 8 * i, 0x0123456789ABCDEFull + 0x1111111111111111ull * i);
  PrintRow("XeCryptDes3Key (0x180)", 24, Time(100000, [&] { XeCryptDes3Key(g, kState, kKey); }), Measure{});
  XeCryptDes3Key(g, kState, kKey);
#if defined(_WIN32)
  BCryptOpenAlgorithmProvider(&des_alg, BCRYPT_3DES_ALGORITHM, nullptr, 0);
  BCryptSetProperty(des_alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                    sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  BCryptGenerateSymmetricKey(des_alg, &des_key, nullptr, 0, g.host(kKey), 24, 0);
#endif
  for (uint32_t size : {8u, 64u, 1024u, 65536u}) {
    const uint32_t n = CallsFor(size) / 8 + 16;
    const Measure ours = Time(n, [&] {
      g.Store64(kFeed, 0x1234567890ABCDEFull);
      XeCryptDes3Cbc(g, kState, kIn, size, kOut + 0x100, kFeed, 1);
    });
    Measure host;
#if defined(_WIN32)
    host = Time(n, [&] {
      uint8_t iv[8] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
      ULONG written = 0;
      BCryptEncrypt(des_key, g.host(kIn), size, nullptr, iv, 8, host_cipher.data(), size, &written, 0);
    }, false);
#endif
    PrintRow("XeCryptDes3Cbc", size, ours, host);
  }
  for (uint32_t size : {8u, 1024u}) {
    const uint32_t n = CallsFor(size) / 8 + 16;
    PrintRow("XeCryptDes3Cbc decrypt", size, Time(n, [&] {
               g.Store64(kFeed, 0x1234567890ABCDEFull);
               XeCryptDes3Cbc(g, kState, kIn, size, kOut + 0x100, kFeed, 0);
             }),
             Measure{});
  }
#if defined(_WIN32)
  BCryptDestroyKey(des_key);
  BCryptCloseAlgorithmProvider(des_alg, 0);
#endif

  // The Win32 forms: what they add over their kernel call.
  const uint32_t kthread = kState + 0x400;
  {
    // The host's GetTickCount64 reads a counter the system updates every 10-16 ms: cheaper, but not the console's
    // millisecond tick, so a bound only.
    Measure host;
#if defined(_WIN32)
    host = Time(200000, [&] { g_sink = g_sink + uint32_t(GetTickCount64()); }, false);
#endif
    PrintRow("GetTickCount (host: GetTickCount64)", 0, Time(200000, [&] { g_sink = g_sink + TickCount(); }), host);
  }
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
    PrintRow("lstrlenW (host: wcslen, own string)", length, ours, host);
  }
  // The information calls (third batch): constants, table lookups and the XAM state they read or write.
  PrintRow("XGetAudioFlags", 0, Time(200000, [&] { g_sink = g_sink + XGetAudioFlags(); }), Measure{});
  PrintRow("XamGetLanguage", 0, Time(200000, [&] { g_sink = g_sink + XamGetLanguage(); }), Measure{});
  PrintRow("XamGetOnlineCountryFeatures (US)", 0, Time(200000, [&] { g_sink = g_sink + XamGetOnlineCountryFeatures(103); }), Measure{});
  PrintRow("XamGetOnlineCountryFeatures (unlisted)", 0, Time(200000, [&] { g_sink = g_sink + XamGetOnlineCountryFeatures(2); }), Measure{});
  PrintRow("XamGetOnlineLanguageAndCountry", 0, Time(200000, [&] { XamGetOnlineLanguageAndCountry(g, kOut, kOut + 1); }), Measure{});
  PrintRow("XamIsSystemTitleId", 0, Time(200000, [&] { g_sink = g_sink + XamIsSystemTitleId(g_sink | 0x4D530000); }), Measure{});
  PrintRow("XamIsSystemExperienceTitleId", 0, Time(200000, [&] { g_sink = g_sink + XamIsSystemExperienceTitleId(g_sink | 0x58480000); }), Measure{});
  PrintRow("XamIsXbox1TitleId", 0, Time(200000, [&] { g_sink = g_sink + XamIsXbox1TitleId(g_sink | 0x41430000); }), Measure{});
  PrintRow("XamGetLanguageTypeface", 0, Time(200000, [&] { g_sink = g_sink + XamGetLanguageTypeface(g, 1, 0x100, kOut); }), Measure{});
  for (uint32_t i = 0; i < 12; ++i) g.Store8(kText + i, uint8_t(i));
  PrintRow("XamSetActiveDashAppInfo (12 B)", 0, Time(200000, [&] { XamState().SetActiveDashAppInfo(g, kText); }), Measure{});
  PrintRow("XamGetActiveDashAppInfo (12 B)", 0, Time(200000, [&] { XamState().GetActiveDashAppInfo(g, kOut); }), Measure{});
  XamState().title().title_name = u"Any360 benchmark title";
  PrintRow("XamGetCachedTitleName (22 chars)", 0, Time(200000, [&] { g_sink = g_sink + XamState().GetCachedTitleName(g, 0x4D5308BC, kOut, kOut + 0x100); }), Measure{});
  PrintRow("XGetOverlappedExtendedError", 0, Time(200000, [&] { g_sink = g_sink + XGetOverlappedExtendedError(g, kthread, kFeed); }), Measure{});
  // The fallback names are laid out in the kernel heap at the first call (200 bytes, untimed), then each call is a lookup.
  Objects().SetKernelHeap(0x80000, 0x8000);
  Objects().SetGuest(g);
  Objects().Reset();
  PrintRow("XamGetLanguageLocaleFallbackString", 0, Time(200000, [&] { g_sink = g_sink + XamState().LanguageLocaleFallbackString(g, 2); }), Measure{});
  // The kernel calls (fourth batch), beside the host's own code where it does the same work.
  {
    uint32_t c = 0;
    const Measure ours = Time(200000, [&] { g_sink = g_sink + RtlDowncaseUnicodeChar(0xC0 + (++c & 0x1F)); });
    const Measure host = Time(200000, [&] { g_sink = g_sink + uint32_t(std::towlower(wint_t(0xC0 + (++c & 0x1F)))); }, false);
    PrintRow("RtlDowncaseUnicodeChar (host: towlower)", 0, ours, host);
    const Measure time = Time(200000, [&] { g_sink = g_sink + uint32_t(InterruptTime()); });
    const Measure clock = Time(200000, [&] { g_sink = g_sink + uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()); }, false);
    PrintRow("KeQueryInterruptTime (host: steady_clock)", 0, time, clock);
#if defined(_WIN32)
    const Measure counter = Time(200000, [&] {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      g_sink = g_sink + uint32_t(now.QuadPart);
    }, false);
    PrintRow("KeQueryInterruptTime (host: QPC alone)", 0, time, counter);
#endif
  }
  g.Store32(kOut + 0x200, 12345);
  PrintRow("RtlRandom", 0, Time(200000, [&] { g_sink = g_sink + RtlRandom(g, kOut + 0x200); }), Measure{});
  PrintRow("RtlGetStackLimits", 0, Time(200000, [&] { RtlGetStackLimits(g, kState, kOut, kOut + 4); }), Measure{});
  PrintRow("XamGetLocaleDateFormat", 0, Time(200000, [&] { g_sink = g_sink + XamGetLocaleDateFormat(36); }), Measure{});
  {
    // A guide screen with a system listener (a title has one): the guide's open and close notifications queued and
    // read back each call, as the title's XNotifyGetNext does.
    const uint32_t listener = XamState().NotifyCreateListener(g, 1, 0);
    PrintRow("XamShowGamerCardUI + 2 XNotifyGetNext", 0, Time(200000, [&] {
               g_sink = g_sink + XamState().ShowProfileUI(0);
               g_sink = g_sink + XamState().NotifyGetNext(g, listener, 0, kOut, kOut + 4);
               g_sink = g_sink + XamState().NotifyGetNext(g, listener, 0, kOut, kOut + 4);
             }),
             Measure{});
    Objects().Close(listener);
  }
  // The profile enumerator as a title uses it: create, read the one profile, read the end, close.
  PrintRow("XamProfile Create+Enumerate x2+Close", 0, Time(20000, [&] {
             XamState().ProfileCreateEnumerator(g, kOut + 0x300);
             const uint32_t handle = g.Load32(kOut + 0x300);
             g_sink = g_sink + XamState().ProfileEnumerate(g, handle, kOut + 0x400, 0);
             g_sink = g_sink + XamState().ProfileEnumerate(g, handle, kOut + 0x400, 0);
             Objects().Close(handle);
           }),
           Measure{});
  // The object manager's own bookkeeping, as every Nt-created object pays it: a create and its close (the block at
  // the top of the heap), with an Ob reference taken and dropped, and two objects closed lowest first (the freed
  // block listed apart from the heap's top until its neighbour joins it).
  PrintRow("Objects NtCreateEvent+NtClose", 0, Time(200000, [&] {
             Objects().CreateEvent(g, kOut + 0x500, 0, 0, 0);
             Objects().Close(g.Load32(kOut + 0x500));
           }),
           Measure{});
  PrintRow("Objects Create+ObRef+ObDeref+Close", 0, Time(200000, [&] {
             Objects().CreateEvent(g, kOut + 0x500, 0, 0, 0);
             const uint32_t handle = g.Load32(kOut + 0x500);
             Objects().ReferenceObjectByHandle(g, handle, 0, kOut + 0x504);
             Objects().DereferenceObject(g, g.Load32(kOut + 0x504));
             Objects().Close(handle);
           }),
           Measure{});
  PrintRow("Objects 2 creates, lower closed first", 0, Time(200000, [&] {
             Objects().CreateEvent(g, kOut + 0x500, 0, 0, 0);
             Objects().CreateEvent(g, kOut + 0x508, 0, 0, 0);
             Objects().Close(g.Load32(kOut + 0x500));
             Objects().Close(g.Load32(kOut + 0x508));
           }),
           Measure{});
  {
    // 64 handles open (a title's usual table), then the same cycle: the close's scan of the table is what grows.
    std::vector<uint32_t> held;
    for (int i = 0; i < 64; ++i) {
      Objects().CreateEvent(g, kOut + 0x500, 0, 0, 0);
      held.push_back(g.Load32(kOut + 0x500));
    }
    PrintRow("Objects NtCreateEvent+NtClose, 64 open", 0, Time(200000, [&] {
               Objects().CreateEvent(g, kOut + 0x500, 0, 0, 0);
               Objects().Close(g.Load32(kOut + 0x500));
             }),
             Measure{});
    for (uint32_t h : held) Objects().Close(h);
  }
  {
    // A file opened, read and closed on a mounted device with 16 devices mounted: the device look-up, the open file's
    // object and its handle (the host file opened again each cycle).
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "any360-step12-bench-files";
    std::error_code ec;
    fs::create_directories(root, ec);
    { std::ofstream(root / "data.bin", std::ios::binary) << "0123456789ABCDEF"; }
    Files().Reset();
    for (int i = 0; i < 16; ++i) Files().Mount("\\Device\\Content\\" + std::to_string(i), root, true);
    Files().Mount("\\Device\\Cdrom0", root, true);
    Files().Link("game:", "\\Device\\Cdrom0");
    const char* name = "game:\\data.bin";
    const uint32_t attrs = kOut + 0x600, string = kOut + 0x620, text = kOut + 0x640, iosb = kOut + 0x6C0, buffer = kOut + 0x700;
    g.Store32(attrs, 0);
    g.Store32(attrs + 4, string);
    g.Store16(string, uint16_t(std::strlen(name)));
    g.Store32(string + 4, text);
    std::memcpy(g.host(text), name, std::strlen(name) + 1);
    PrintRow("Files NtCreateFile+NtReadFile 16+NtClose", 0, Time(20000, [&] {
               Files().CreateFile(g, kOut + 0x680, 0x80000000, attrs, iosb, 0, kOpen, kFileNonDirectoryFile | kFileSynchronousIo);
               const uint32_t handle = g.Load32(kOut + 0x680);
               g_sink = g_sink + Files().ReadFile(g, handle, 0, iosb, buffer, 16, 0);
               Objects().Close(handle);
             }),
             Measure{});
    // The path alone through the link and the mount list (no host file): what the locks around them cost.
    PrintRow("Files Knows (link + 17 mounts)", 0, Time(200000, [&] { g_sink = g_sink + Files().Knows("game:\\data.bin"); }), Measure{});
    Files().Reset();
    fs::remove_all(root, ec);
  }
  // Step 12, seventh batch. SHA-512 streamed as a title uses it (Init, one Update, Final) against CNG's one call on the
  // same bytes (the digests compared), then the BnQw helpers, the signature formats, the console's identity.
  {
#if defined(_WIN32)
    const Cng sha512(BCRYPT_SHA512_ALGORITHM, 0);
#endif
    for (uint32_t size : sizes) {
      const uint32_t n = CallsFor(size);
      const Measure ours = Time(n, [&] {
        XeCryptSha512Init(g, kState);
        XeCryptSha512Update(g, kState, kIn, size);
        XeCryptSha512Final(g, kState, kOut, 64);
      });
      Measure host;
#if defined(_WIN32)
      host = Time(n, [&] { sha512.Hash(nullptr, 0, g.host(kIn), size, host_out, 64); }, false);
      if (std::memcmp(host_out, g.host(kOut), 64) != 0) std::printf("  !! SHA-512 differs from CNG at %u bytes\n", size);
#endif
      PrintRow("XeCryptSha512 Init+Update+Final", size, ours, host);
    }
    // Numbers of 1024, 2048 and 4096 bits (16, 32, 64 words), the modulus odd with its top bit set.
    const uint32_t kA = kText, kB = kText + 0x200, kM = kText + 0x400, kR = kText + 0x600;
    for (uint32_t i = 0; i < 64; ++i) {
      g.Store64(kA + 8 * i, 0x9E3779B97F4A7C15ull * (i + 1)), g.Store64(kB + 8 * i, 0xC2B2AE3D27D4EB4Full * (i + 3));
      g.Store64(kM + 8 * i, 0xFF51AFD7ED558CCDull * (i + 7) | (i == 63 ? 1ull << 63 : 0) | (i == 0 ? 1 : 0));
    }
    const uint64_t inverse = XeCryptBnQwNeModInv(g.Load64(kM));
    for (uint32_t words : {16u, 32u, 64u}) {
      g.Store64(kM + 8 * (words - 1), g.Load64(kM + 8 * (words - 1)) | 1ull << 63);
      PrintRow("XeCryptBnQwNeModMul", words * 8, Time(words == 64 ? 20000 : 100000, [&] {
                 XeCryptBnQwNeModMul(g, kA, kB, kR, inverse, kM, words);
               }),
               Measure{});
    }
    uint64_t v = 0x123456789ABCDEF1ull;
    PrintRow("XeCryptBnQwNeModInv", 8, Time(1000000, [&] { v = XeCryptBnQwNeModInv(v) | 1; }), Measure{});
    g_sink = g_sink + uint32_t(v);
    for (uint32_t words : {16u, 64u}) {
      PrintRow("XeCryptBnQw_Copy", words * 8, Time(1000000, [&] { XeCryptBnQw_Copy(g, kA, kR, words); }),
               Time(1000000, [&] { std::memmove(g.host(kR), g.host(kA), size_t(words) * 8); }, false));
    }
    std::memcpy(g.host(kKey), "XBOX360SVC", 10);
    PrintRow("XeCryptBnQwBeSigFormat", 256, Time(200000, [&] { XeCryptBnQwBeSigFormat(g, kR, kIn, kKey); }), Measure{});
    PrintRow("XeCryptBnDwLePkcs1Format", 256, Time(1000000, [&] { XeCryptBnDwLePkcs1Format(g, kIn, 0, kR, 256); }), Measure{});
    PrintRow("XeKeysGetConsoleID", 5, Time(1000000, [&] { XeKeysGetConsoleID(g, kOut, kOut + 16); }), Measure{});
    PrintRow("XeKeysGetConsoleType", 4, Time(1000000, [&] { XeKeysGetConsoleType(g, kOut); }), Measure{});
    uint32_t key = 0;
    PrintRow("XeKeysGetKeyProperties", 0, Time(1000000, [&] { g_sink = g_sink + XeKeysGetKeyProperties(key++ & 0x7F); }),
             Measure{});
  }
  // Step 12, eighth batch: the language and locale, the Xapi helpers, IPTV's name, XamAllocEx (with the XamFree that
  // returns its pages, against the host's VirtualAlloc + VirtualFree of as many zeroed pages), and LDI: a whole LZX
  // stream decoded chunk by chunk (Create on scratch memory, every chunk, Destroy) against cabinet.dll's FDI decoding
  // the same CAB from memory (its allocations counted through its allocator).
  {
    PrintRow("XTLGetLanguageV2", 0, Time(1000000, [&] { g_sink = g_sink + XTLGetLanguageV2(); }), Measure{});
    PrintRow("XapipGetLocale", 16, Time(1000000, [&] { g_sink = g_sink + XapipGetLocale(g, 16, kOut); }), Measure{});
    PrintRow("XapiFormatTimeOut", 8, Time(1000000, [&] { g_sink = g_sink + XapiFormatTimeOut(g, kOut, 1500); }), Measure{});
    uint32_t processor = 0;
    PrintRow("XapiThreadFlags (XapipCreateThread)", 0,
             Time(1000000, [&] { g_sink = g_sink + XapiThreadFlags(4, processor++ & 7); }), Measure{});
    PrintRow("XamIptvGetServiceName", 4, Time(1000000, [&] { g_sink = g_sink + XamState().IptvServiceName(g, kOut); }),
             Measure{});

    std::vector<uint8_t> memory(0x1000000);
    const Guest gm{memory.data()};
    Memory().Reset({{0x00000000, 0x400000, 0x1000, false}, {0x400000, 0x400000, 0x10000, false},
                    {0x800000, 0x400000, 0x1000, true}, {0xC00000, 0x400000, 0x10000, true}});
    const uint32_t io = 0x3FF000;
    for (const uint32_t size : {0x2345u, 0x40000u}) {
      Measure host;
#if defined(_WIN32)
      host = Time(20000, [&] {
        void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        static_cast<volatile uint8_t*>(p)[0] = 1;
        VirtualFree(p, 0, MEM_RELEASE);
      }, false);
#endif
      PrintRow("XamAllocEx physical + XamFree", size, Time(20000, [&] {
                 Memory().XamAllocEx(gm, 0x20000000, 0x40000000, size, io);
                 Memory().XamFree(gm, gm.Load32(io));
               }),
               host);
      PrintRow("XamAllocEx virtual + XamFree", size, Time(20000, [&] {
                 Memory().XamAllocEx(gm, 0, 0, size, io);
                 Memory().XamFree(gm, gm.Load32(io));
               }),
               host);
    }
    Memory().Reset();

#if defined(_WIN32)
    // Streams of 1 MiB: text and x86-like code (E8 calls), and incompressible bytes (uncompressed blocks).
    std::vector<uint8_t> text_code, noise(1u << 20);
    {
      uint32_t x = 12345;
      auto next = [&] { return x = x * 1664525u + 1013904223u; };
      static const char* kWords[] = {"the ", "kernel ", "frame ", "window ", "of ", "LZX ", "decoder ", "block ", "\n",
                                     "tree ", "match ", "offset ", "length ", "and ", "a ", "title "};
      while (text_code.size() < (1u << 20)) {
        if ((next() >> 20) % 3) {
          const char* w = kWords[(next() >> 16) % 16];
          text_code.insert(text_code.end(), w, w + std::strlen(w));
        } else {
          const uint32_t target = (next() >> 8) % 0x60000;
          const uint8_t call[] = {0xE8, uint8_t(target), uint8_t(target >> 8), uint8_t(target >> 16), 0, 0x8B, 0x45, 0xF8};
          text_code.insert(text_code.end(), call, call + 8);
        }
      }
      text_code.resize(1u << 20);
      for (auto& b : noise) b = uint8_t(next() >> 24);
    }
    struct Stream {
      const char* name;
      const std::vector<uint8_t>* data;
      uint32_t bits;
    };
    const Stream streams[] = {{"LDI stream, text and code, 2^17", &text_code, 17},
                              {"LDI stream, text and code, 2^21", &text_code, 21},
                              {"LDI stream, random bytes, 2^17", &noise, 17}};
    for (const Stream& s : streams) {
      const any360::test::cab::Bytes cab = any360::test::cab::MakeCab(*s.data, s.bits);
      const auto chunks = any360::test::cab::Chunks(cab);
      if (chunks.empty()) {
        std::printf("  !! FCI gave no LZX stream for %s\n", s.name);
        continue;
      }
      // The chunks laid out in guest memory one after the other; the frame out at 0x10000.
      std::vector<uint32_t> at;
      uint32_t next = 0x100000;
      for (const auto& c : chunks) {
        at.push_back(next);
        std::memcpy(gm.host(next), c.data.data(), c.data.size());
        next += uint32_t(c.data.size() + 15) & ~15u;
      }
      const uint32_t scratch = 0x1000, out = 0x10000, sizes = 0x2000;
      gm.Store32(sizes, 0x8000);
      gm.Store32(sizes + 4, 1u << s.bits);
      bool same = true;
      const uint32_t n = 20;
      const Measure ours = Time(n, [&] {
        Ldi().Create(gm, sizes, sizes + 4, 0, 0, scratch, 0, sizes + 8, nullptr, nullptr);
        uint32_t offset = 0;
        for (size_t i = 0; i < chunks.size(); ++i) {
          gm.Store32(sizes + 12, chunks[i].frame);
          same &= Ldi().Decompress(gm, scratch, at[i], uint32_t(chunks[i].data.size()), out, sizes + 12) == 0 &&
                  std::memcmp(gm.host(out), s.data->data() + offset, chunks[i].frame) == 0;
          offset += chunks[i].frame;
        }
        Ldi().Destroy(gm, scratch, nullptr, nullptr);
      });
      any360::test::cab::Fdi fdi;
      std::vector<uint8_t> host_out(s.data->size());
      const Measure host = Time(n, [&] { same &= fdi.Decode(cab, host_out); }, false);
      same &= host_out == *s.data;
      PrintRow(s.name, uint32_t(s.data->size()), ours,
               {host.ns, double(fdi.bytes) / double(n * g_repeats), double(fdi.allocations) / double(n * g_repeats)});
      std::printf("    %zu chunks, %zu bytes compressed: Any360 %.1f MB/s, FDI %.1f MB/s%s\n", chunks.size(), cab.size(),
                  double(s.data->size()) / ours.ns * 1e3, double(s.data->size()) / host.ns * 1e3,
                  same ? "" : "  !! output differs");
    }
#endif
  }
  // Step 12, ninth batch: the content "Internal" forms on packages made as a title makes them (XamContentCreate: the
  // folder and its header; 2 and 32 of them), the enumerator closed again; XamReadString; network storage's
  // privilege. The host's floor beside the enumerator: the same folder listed and the same headers read with nothing
  // else (std::filesystem's iterator, fopen / fread / fclose); and what two folder date reads cost (a listing cache
  // would still pay them).
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path content = fs::temp_directory_path() / "any360-step12-bench-content";
    // Past the other rows' buffers, beside the kernel heap (0x80000) the enumerators live in.
    const uint32_t out = 0xEC000, data = 0xE0000, meta = 0xE1000, root_name = 0xEC100;
    std::memcpy(g.host(root_name), "bench", 6);
    for (const uint32_t packages : {2u, 32u}) {
      fs::remove_all(content, ec);
      XamState().title().content_root = content;
      for (uint32_t p = 0; p < packages; ++p) {
        std::memset(g.host(data), 0, 0x134);
        g.Store32(data, 1), g.Store32(data + 4, 1);
        char name[16];
        std::snprintf(name, sizeof name, "slot%02u", p);
        for (uint32_t k = 0; name[k]; ++k) g.Store16(data + 8 + 2 * k, uint16_t(name[k]));
        std::memcpy(g.host(data + 0x108), name, std::strlen(name) + 1);
        XamState().ContentCreate(g, 0, root_name, data, 2, 0, 0, 0);
        XamState().ContentClose(g, root_name, 0);
      }
      std::memcpy(g.host(data + 0x108), "slot00", 7);
      bool ok = true;
      const Measure ours = Time(packages > 2 ? 500 : 2000, [&] {
        ok &= XamState().ContentCreateEnumeratorInternal(g, 1, 1, packages, out + 4, out) == 0;
        Objects().Close(g.Load32(out));
      });
      const Measure host = Time(packages > 2 ? 500 : 2000, [&] {
        uint8_t header[0x148];
        for (const auto& entry : fs::directory_iterator(content / "00000001", ec)) {
          if (!entry.is_directory()) continue;
          const fs::path path = content / "Headers" / "00000001" / (entry.path().filename().string() + ".header");
          std::FILE* file = nullptr;
#if defined(_WIN32)
          if (_wfopen_s(&file, path.c_str(), L"rb")) file = nullptr;
#else
          file = std::fopen(path.c_str(), "rb");
#endif
          if (file) {
            g_sink = g_sink + uint32_t(std::fread(header, 1, sizeof header, file));
            std::fclose(file);
          }
        }
      }, false);
      char label[64];
      std::snprintf(label, sizeof label, "XamContentCreateEnumeratorInternal+close (host: list+headers)");
      PrintRow(label, packages, ours, host);
      const Measure meta_row = Time(2000, [&] { ok &= XamState().ContentGetMetaDataInternal(g, data, meta, 0) == 0; });
      PrintRow("XamContentGetMetaDataInternal", packages, meta_row, Measure{});
      if (packages == 2) {
        PrintRow("XamContentSetThumbnailInternal", 4, Time(1000, [&] { ok &= XamState().ContentSetThumbnail(g, data, out, 4, 0) == 0; }),
                 Measure{});
        PrintRow("two folder dates (a cache's check)", 2, Time(5000, [&] {
                   g_sink = g_sink + uint32_t(fs::last_write_time(content / "00000001", ec).time_since_epoch().count());
                   g_sink = g_sink + uint32_t(fs::last_write_time(content / "Headers" / "00000001", ec).time_since_epoch().count());
                 }, false),
                 Measure{});
      }
      if (!ok) std::printf("  !! a content row failed (%u packages)\n", packages);
    }
    g.Store32(out + 8, 16);
    PrintRow("XamReadString", 2, Time(1000000, [&] { g_sink = g_sink + XamState().ReadString(g, 5, out + 16, out + 8, 0); }),
             Measure{});
    uint32_t user = 0;
    PrintRow("XamNetworkStorageUserHasPrivilege", 0,
             Time(1000000, [&] { g_sink = g_sink + XamState().NetworkStorageUserHasPrivilege(user++ & 3); }), Measure{});
    XamState().title().content_root.clear();
    fs::remove_all(content, ec);
  }
  // Step 12, tenth batch: ObCreateObject with the type's allocate procedure played by the host (the same block each
  // time), unnamed and named; XexLoadImageFromMemory for a module no title loaded.
  {
    const uint32_t type = 0xED000, attributes = 0xED040, name = 0xED060, out = 0xED100, block = 0xEE000;
    std::memset(g.host(type), 0, 0x20);
    g.Store32(type, 0x82001000), g.Store32(type + 0x18, 0x54657374);
    std::memset(g.host(attributes), 0, 12);
    g.Store32(attributes + 4, name);
    g.Store16(name, 13), g.Store16(name + 2, 13), g.Store32(name + 4, name + 8);
    std::memcpy(g.host(name + 8), "\\Title\\Event7", 13);
    auto allocate = [](uint32_t, uint32_t, uint32_t, uint32_t, void*) -> uint32_t { return 0xEE000; };
    bool ok = true;
    PrintRow("ObCreateObject unnamed", 0x40,
             Time(1000000, [&] { ok &= ObCreateObject(g, type, 0, 0x40, out, 1, allocate, nullptr) == 0; }), Measure{});
    PrintRow("ObCreateObject named", 0x40,
             Time(1000000, [&] { ok &= ObCreateObject(g, type, attributes, 0x40, out, 1, allocate, nullptr) == 0; }), Measure{});
    ok &= g.Load32(out) == block + 0x20;
    std::memcpy(g.host(name + 0x40), "dlc.xex", 8);
    PrintRow("XexLoadImageFromMemory (not loaded)", 0,
             Time(1000000, [&] { ok &= XamState().LoadImageFromMemory(g, name + 0x40, out) == 0xC000000F; }), Measure{});
    if (!ok) std::printf("  !! a tenth-batch row failed\n");
  }
  // Step 12, the avatar calls Xenia stubs too: the manifest kept in the profile (in memory, then also written to the
  // profile folder as a binary setting) and read back; the package of an asset.
  {
    const uint32_t info = 0xE0000, meta = 0xE1000, asset = 0xE2000, package = 0xE2100;
    for (uint32_t i = 0; i < 0x3E8; ++i) g.Store8(info + i, uint8_t(i));
    bool ok = true;
    PrintRow("XamAvatarSetManifest (in memory)", 0x3E8, Time(200000, [&] { ok &= XamState().AvatarSetManifest(g, 0, info, 0) == 0; }),
             Measure{});
    PrintRow("XamAvatarGetManifestLocalUser", 0x3E8,
             Time(200000, [&] { ok &= XamState().AvatarGetManifestLocalUser(g, 0, meta, 0) == 0; }), Measure{});
    g.Store64(asset, 0x0123456789ABCDEFull), g.Store32(asset + 8, 0x11223344), g.Store32(asset + 12, 0x4D5307E6);
    PrintRow("XamAvatarGetInstalledAssetPackageDescription", 0x148,
             Time(200000, [&] { XamState().AvatarGetInstalledAssetPackageDescription(g, asset, package); }), Measure{});
    if (!ok) std::printf("  !! an avatar row failed\n");
  }
  std::printf("sys_step12_bench: allocated per call %.2f B in %.4f allocations over %llu timed calls\n",
              g_totals.calls ? double(g_totals.bytes) / double(g_totals.calls) : 0.0,
              g_totals.calls ? double(g_totals.count) / double(g_totals.calls) : 0.0,
              static_cast<unsigned long long>(g_totals.calls));
  return 0;
}
