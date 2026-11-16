// Timings of zlib 1.1.4's inflate_fast (any360/xdk/zlib.h) against its model and the version first written (kept here
// as the fixed point, never edited): nanoseconds a call and a byte written, on three mixes of symbols. Not a test. The
// figures that decide a binding are the title's own, with --any360_xdk_bench (the function's recompiled code against
// the replacement, on the title's calls): docs/NATIVE_FUNCTIONS.md.
// Run: xdk_zlib_bench [repeats]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xdk_bench.h"

#include "any360/xdk/zlib.h"
#include "any360/xdk/zlib_reference.h"
#include "xdk_zlib_streams.h"

namespace first {
using any360::xdk::InflateFastCall;
using any360::xdk::InflateOutcome;
using any360::xdk::IsPlainMemory;
using any360::xdk::Memory;
namespace detail = any360::xdk::detail;

// `hand_back_at` (tests): hand back before that symbol (0: decline) as if it were one the function does not model.
NOINLINE InflateOutcome InflateFast(const Memory& memory, const InflateFastCall& call, uint32_t& result,
                                  uint32_t hand_back_at = 0xFFFFFFFFu) {
  using detail::GuestLoad32;
  using detail::GuestStore32;
  const uint32_t s = call.s, z = call.z;
  if (call.bl > 16 || call.bd > 16 || !IsPlainMemory(s, 56) || !IsPlainMemory(z, 12)) return InflateOutcome::kDeclined;
  const uint32_t window = GuestLoad32(memory, s + 40), end = GuestLoad32(memory, s + 44);
  const uint32_t read = GuestLoad32(memory, s + 48), write = GuestLoad32(memory, s + 52);
  const uint32_t next_in = GuestLoad32(memory, z + 0), avail_in = GuestLoad32(memory, z + 4);
  uint32_t b = GuestLoad32(memory, s + 32), k = GuestLoad32(memory, s + 28);
  if (window >= end || !IsPlainMemory(window, end - window) || read < window || read > end || write < window ||
      write > end || k >= 32) {
    return InflateOutcome::kDeclined;
  }
  uint32_t m = write < read ? read - write - 1 : end - write;
  if (m < 258 || avail_in < 10 || !IsPlainMemory(next_in, avail_in)) return InflateOutcome::kDeclined;
  const auto overlaps = [&](uint32_t a, uint32_t size) { return a < end && window < a + size; };
  if (overlaps(s, 56) || overlaps(z, 12)) return InflateOutcome::kDeclined;

  uint8_t* const out = memory.Host(window);
  const uint32_t size = end - window;
  const uint8_t* const in = memory.Host(next_in);
  const uint32_t ml = detail::InflateMask(call.bl), md = detail::InflateMask(call.bd);
  uint32_t q = write - window, p = 0, n = avail_in, symbols = 0;

  // UNGRAB and UPDATE: the whole bytes of bits not used go back to the input.
  const auto leave = [&](uint32_t bits, uint32_t count) {
    const uint32_t c = std::min(count >> 3, avail_in - n);
    GuestStore32(memory, s + 32, bits);
    GuestStore32(memory, s + 28, count - (c << 3));
    GuestStore32(memory, z + 4, n + c);
    const uint32_t next = next_in + p - c;
    GuestStore32(memory, z + 8, GuestLoad32(memory, z + 8) - next_in + next);
    GuestStore32(memory, z + 0, next);
    GuestStore32(memory, s + 52, window + q);
  };

  for (;; ++symbols) {
    if (symbols == hand_back_at) goto hand_back;
    {
      uint32_t bb = b, kk = k, pp = p;
      while (kk < 20) bb |= uint32_t(in[pp++]) << kk, kk += 8;
      uint32_t at = call.tl + ((bb & ml) << 3);
      detail::Huft t;
      if (!detail::LoadHuft(memory, at, t)) goto hand_back;
      bb = detail::Srw(bb, t.bits), kk -= t.bits;
      uint32_t e = t.exop, links = 0;
      if (e == 0) goto literal;
      while (!(e & 16)) {
        if (e & 64) {
          if (!(e & 32)) goto hand_back;  // an invalid code: the function writes its message
          n -= pp - p, p = pp;            // the end of the block
          leave(bb, kk);
          result = 1;
          return InflateOutcome::kDone;
        }
        if (e > 16 || ++links > 16) goto hand_back;
        at += ((detail::InflateMask(e) & bb) + t.base) << 3;
        if (!detail::LoadHuft(memory, at, t)) goto hand_back;
        bb = detail::Srw(bb, t.bits), kk -= t.bits;
        e = t.exop;
        if (e == 0) goto literal;
      }
      {
        // A length, then its distance.
        uint32_t extra = e & 15;
        const uint32_t c = t.base + (detail::InflateMask(extra) & bb);
        bb >>= extra, kk -= extra;
        while (kk < 15) bb |= uint32_t(in[pp++]) << kk, kk += 8;
        at = call.td + ((bb & md) << 3);
        if (!detail::LoadHuft(memory, at, t)) goto hand_back;
        for (;;) {
          bb = detail::Srw(bb, t.bits), kk -= t.bits;
          e = t.exop;
          if (e & 16) break;
          if ((e & 64) || e > 16 || ++links > 32) goto hand_back;
          at += ((detail::InflateMask(e) & bb) + t.base) << 3;
          if (!detail::LoadHuft(memory, at, t)) goto hand_back;
        }
        extra = e & 15;
        while (kk < extra) bb |= uint32_t(in[pp++]) << kk, kk += 8;
        const uint32_t d = t.base + (detail::InflateMask(extra) & bb);
        bb >>= extra, kk -= extra;
        if (c < 3 || c > m) goto hand_back;
        uint32_t r = window + q - d;  // the function's address arithmetic, 32 bits
        if (r < window) {
          r += (window - r + size - 1) / size * size;  // into the window, as its loop does
          const uint32_t to_end = end - r;
          if (c > to_end) {
            detail::WindowCopy(out, q, r - window, to_end);
            detail::WindowCopy(out, q + to_end, 0, c - to_end);
          } else {
            detail::WindowCopy(out, q, r - window, c);
          }
        } else {
          if (r - window > q) goto hand_back;  // a distance of 0 is a copy in place; one past address 0 is not
          detail::WindowCopy(out, q, r - window, c);
        }
        q += c, m -= c;
        goto next;
      }
    literal:
      out[q] = uint8_t(t.base);
      q += 1, m -= 1;
    next:
      b = bb, k = kk, n -= pp - p, p = pp;
      if (m < 258 || n < 10) {
        leave(b, k);
        result = 0;
        return InflateOutcome::kDone;
      }
      continue;
    }
  hand_back:
    if (symbols == 0) return InflateOutcome::kDeclined;
    leave(b, k);
    return InflateOutcome::kHandedOff;
  }
}

}  // namespace first

namespace {

namespace xdk = any360::xdk;
using namespace xdk_zlib;

template <class F>
double NsPerCall(F&& f, int calls) {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < calls; ++i) f();
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  xdk_bench::Allocated& a = xdk_bench::g_allocated;
  a.calls += uint64_t(calls), a.bytes += spent.bytes, a.count += spent.count;
  a.max_bytes_a_call = std::max(a.max_bytes_a_call, double(spent.bytes) / double(calls));
  return ns;
}

}  // namespace

int main(int argc, char** argv) {
  const int repeats = argc > 1 ? std::atoi(argv[1]) : 3;
  std::vector<uint8_t> arena(1u << 20, 0);
  xdk::Memory memory;
  memory.base = arena.data();
  const xdk::reference::InflateFastBuild build{kMasks, kMsgLiteral, kMsgDistance, uint32_t(arena.size())};
  struct Mix {
    const char* name;
    uint32_t literal_share;
  };
  for (int r = 0; r < repeats; ++r) {
    for (const Mix& mix : {Mix{"mostly literals", 90}, Mix{"half matches", 50}, Mix{"mostly matches", 15}}) {
      std::mt19937 rng(0x2026 + mix.literal_share);
      Case c = MakeCase(rng, false, 9000, 32768, mix.literal_share);
      c.window_size = 0x8000, c.write = 0, c.read = 0;
      Place(arena, c);
      std::vector<uint8_t> state(arena.begin() + kS, arena.begin() + kS + 56), stream(arena.begin() + kZ, arena.begin() + kZ + 12);
      const auto reset = [&] {
        std::memcpy(&arena[kS], state.data(), state.size());
        std::memcpy(&arena[kZ], stream.data(), stream.size());
      };
      const xdk::InflateFastCall call{c.bl, c.bd, kTables + c.tl * 8, kTables + c.td * 8, kS, kZ};
      uint32_t result = 0;
      reset();
      xdk::InflateFast(memory, call, result);
      const uint32_t written = xdk::detail::GuestLoad32(memory, kS + 52) - kWindow;
      const int calls = 300;
      const double current = NsPerCall([&] { reset(); xdk::InflateFast(memory, call, result); xdk_bench::g_sink = result; }, calls);
      const double original = NsPerCall([&] { reset(); first::InflateFast(memory, call, result); xdk_bench::g_sink = result; }, calls);
      const double model = NsPerCall([&] {
        reset();
        xdk_bench::g_sink = xdk::reference::InflateFast(memory, call.bl, call.bd, call.tl, call.td, call.s, call.z, build);
      }, calls / 4);
      std::printf("inflate_fast, %-15s (%5u bytes a call): current %8.0f ns (%.2f ns a byte), first %8.0f ns (%.2fx), "
                  "model %8.0f ns (%.2fx)\n",
                  mix.name, written, current, current / written, original, original / current, model, model / current);
    }
  }
  xdk_bench::PrintAllocated("xdk_zlib_bench");
  return 0;
}
