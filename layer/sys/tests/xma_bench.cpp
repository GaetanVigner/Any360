// Timings of the XMA frame decoder (any360/sys/xma.h's XmaFrameDecoder: FFmpeg's xmaframes decoder and the output
// conversion) on frames captured from a title (ANY360_XMA_CAPTURE, see xma.cpp; a capture holds a title's audio and
// is never committed): nanoseconds a frame, the host heap's allocations a frame (the C runtime's malloc family as well
// as operator new: FFmpeg allocates with av_malloc), and a hash of every sample decoded, so that a change to the
// decoder is checked to give the same output bit for bit. The C runtime's allocations are counted on Windows only
// (through the program's import table).
// Run: xma_bench <capture> [repeats]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/xma.h"

#ifdef _WIN32
// After Any360's headers: its macros (min, max, Yield) would reach them.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

// The C runtime's heap calls counted, by patching this program's own import table (FFmpeg is linked in statically,
// so its malloc family goes through it). Windows only.
std::atomic<uint64_t> g_crt_allocations{0}, g_crt_bytes{0};
#ifdef _WIN32
constexpr bool kCrtCounted = true;
using MallocFn = void* (*)(size_t);
using CallocFn = void* (*)(size_t, size_t);
using ReallocFn = void* (*)(void*, size_t);
using AlignedMallocFn = void* (*)(size_t, size_t);
using AlignedReallocFn = void* (*)(void*, size_t, size_t);
MallocFn g_malloc;
CallocFn g_calloc;
ReallocFn g_realloc;
AlignedMallocFn g_aligned_malloc;
AlignedReallocFn g_aligned_realloc;
void* CountedMalloc(size_t n) { g_crt_allocations++, g_crt_bytes += n; return g_malloc(n); }
void* CountedCalloc(size_t c, size_t n) { g_crt_allocations++, g_crt_bytes += c * n; return g_calloc(c, n); }
void* CountedRealloc(void* p, size_t n) { g_crt_allocations++, g_crt_bytes += n; return g_realloc(p, n); }
void* CountedAlignedMalloc(size_t n, size_t a) { g_crt_allocations++, g_crt_bytes += n; return g_aligned_malloc(n, a); }
void* CountedAlignedRealloc(void* p, size_t n, size_t a) {
  g_crt_allocations++, g_crt_bytes += n;
  return g_aligned_realloc(p, n, a);
}

void PatchImports() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const char* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
      struct Hook {
        const char* name;
        void* replacement;
        void** original;
      };
      const Hook hooks[] = {
          {"malloc", reinterpret_cast<void*>(&CountedMalloc), reinterpret_cast<void**>(&g_malloc)},
          {"calloc", reinterpret_cast<void*>(&CountedCalloc), reinterpret_cast<void**>(&g_calloc)},
          {"realloc", reinterpret_cast<void*>(&CountedRealloc), reinterpret_cast<void**>(&g_realloc)},
          {"_aligned_malloc", reinterpret_cast<void*>(&CountedAlignedMalloc),
           reinterpret_cast<void**>(&g_aligned_malloc)},
          {"_aligned_realloc", reinterpret_cast<void*>(&CountedAlignedRealloc),
           reinterpret_cast<void**>(&g_aligned_realloc)},
      };
      void* replacement = nullptr;
      void** original = nullptr;
      for (const Hook& h : hooks) {
        if (!std::strcmp(name, h.name)) replacement = h.replacement, original = h.original;
      }
      if (!replacement) continue;
      DWORD protect = 0;
      VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protect);
      *original = reinterpret_cast<void*>(slots->u1.Function);
      slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
      VirtualProtect(&slots->u1.Function, sizeof(void*), protect, &protect);
    }
  }
}
#else
constexpr bool kCrtCounted = false;
void PatchImports() {}
#endif

struct Record {
  uint32_t context, rate_id, frame_bits, padding;
  bool stereo;
  std::vector<uint8_t> bytes;  // empty for a reset
};

std::vector<Record> Load(const char* path) {
  std::vector<Record> records;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return records;
  uint32_t head[5];
  while (std::fread(head, sizeof(head), 1, f) == 1) {
    Record r{head[0], head[1] & 0xFF, head[2], head[3], ((head[1] >> 8) & 1) != 0, {}};
    r.bytes.resize(head[4]);
    if (head[4] && std::fread(r.bytes.data(), head[4], 1, f) != 1) break;
    records.push_back(std::move(r));
  }
  std::fclose(f);
  return records;
}

uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("xma_bench: no capture given (xma_bench <capture> [repeats]); nothing timed\n");
    return 0;
  }
  PatchImports();
  const std::vector<Record> records = Load(argv[1]);
  const int repeats = argc > 2 ? std::atoi(argv[2]) : 3;
  uint64_t frames = 0, stereo = 0, resets = 0;
  std::map<uint32_t, int> contexts;
  for (const Record& r : records) {
    contexts[r.context]++;
    if (!r.frame_bits) ++resets;
    else ++frames, stereo += r.stereo;
  }
  std::printf("capture: %llu frames (%llu stereo), %llu resets, %zu contexts; FFmpeg SIMD flags 0x%X\n",
              (unsigned long long)frames, (unsigned long long)stereo, (unsigned long long)resets, contexts.size(),
              unsigned(any360::sys::XmaFrameDecoder::SimdFlags()));
  if (!frames) return 1;
  std::vector<uint8_t> frame(1 + 4096 + 8), out(2 * 1024 * 2);
  for (int rep = 0; rep < repeats; ++rep) {
    std::map<uint32_t, std::unique_ptr<any360::sys::XmaFrameDecoder>> decoders;
    uint64_t hash = 0xCBF29CE484222325ull, decoded = 0, dropped = 0;
    const uint64_t crt_before = g_crt_allocations.load(), crt_bytes_before = g_crt_bytes.load();
    const any360::sys::AllocCounts new_before = any360::sys::ThreadAllocations();
    const auto t0 = std::chrono::steady_clock::now();
    for (const Record& r : records) {
      auto& d = decoders[r.context];
      if (!d) d = std::make_unique<any360::sys::XmaFrameDecoder>();
      if (!r.frame_bits) {
        d->Reset();
        continue;
      }
      std::memset(frame.data(), 0, frame.size());
      std::memcpy(frame.data() + 1, r.bytes.data(), r.bytes.size());
      const int prepared = d->Prepare(r.rate_id, r.stereo);
      if (prepared >= 0 && d->Decode(frame.data(), r.frame_bits, r.padding, r.stereo, out.data())) {
        hash = Fnv(hash, out.data(), size_t(1024) << r.stereo);
        ++decoded;
      } else {
        ++dropped;
      }
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    const any360::sys::AllocCounts new_spent = any360::sys::ThreadAllocations() - new_before;
    const uint64_t crt = g_crt_allocations.load() - crt_before, crt_bytes = g_crt_bytes.load() - crt_bytes_before;
    char crt_text[64] = "not counted here";
    if (kCrtCounted) {
      std::snprintf(crt_text, sizeof(crt_text), "%.2f (%.0f B)", double(crt) / double(frames),
                    double(crt_bytes) / double(frames));
    }
    std::printf("decode: %.0f ns a frame (%llu decoded, %llu dropped); host heap a frame: C runtime allocations %s, "
                "%.2f operator new (%.0f B); output hash %016llX\n",
                ns / double(frames), (unsigned long long)decoded, (unsigned long long)dropped, crt_text,
                double(new_spent.count) / double(frames), double(new_spent.bytes) / double(frames),
                (unsigned long long)hash);
  }
  return 0;
}
