// zlib 1.1.4's inflate_fast, native: the decoding loop titles run on every compressed byte they read (PNG images
// through libpng, D3DX's own copy; where each title has it: db/xdk_seed.json). Its model, the function's
// own PowerPC step by step, is any360/xdk/zlib_reference.h; the registers and the layout are described there.
//
// What it takes and what it leaves:
// - It declines (returns kDeclined, nothing written, the function's own code runs) a call whose state is outside
//   what zlib gives it: lookup bits above 16, a window that is empty or not plain memory, read or write pointers
//   outside the window, fewer than 258 bytes of room or 10 bytes of input (the function's loop runs once anyway:
//   zlib never calls it so), input or state that is not plain memory, state that overlaps the window.
// - It decodes symbol by symbol, each one in full (its input bits, its table entries, its length and distance)
//   before it writes anything of it. A symbol it does not model, it hands back: it stores the state as the function
//   does on leaving (UNGRAB, UPDATE) as it was before that symbol and returns kHandedOff, and the caller runs the
//   function's own code, which loads that state and goes on from the same symbol, writing what the uninterrupted
//   function would (the bits it gives back are the bytes it reads again, as long as its bit count does not wrap
//   below 0, which only a corrupted table does). Handed back: invalid codes (the function writes a message whose
//   address only the build knows), a table entry outside plain memory, a table link whose mask index is above 16
//   (the build's table ends there), more than 16 links in one symbol (zlib's tables have two levels; a corrupted
//   one can link to itself), a copy of fewer than 3 bytes (the function's loop does not end) or of more bytes than
//   there is room, and a distance that reaches before address 0 or past the write pointer. zlib's tables (built by
//   its huft_build from any input) give only the first: an invalid code, after which the function returns.
// - An entry that drops more bits than the buffer holds is followed as the PowerPC does it (the count wraps, a shift
//   of 32 to 63 gives 0), not handed back: the function's last UNGRAB would then give back other bytes than a run
//   that stops there.
// - It writes what the function writes: the window bytes, bitb, bitk, write, next_in, avail_in, total_in. The
//   registers it saves below r1 are the binding's to write (any360/host/xdk_natives.h).
// Allocates nothing.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

#include "any360/xdk/crt.h"

namespace any360::xdk {

enum class InflateOutcome : uint32_t { kDeclined, kDone, kHandedOff };

struct InflateFastCall {
  uint32_t bl = 0, bd = 0, tl = 0, td = 0, s = 0, z = 0;  // r3-r8
};

namespace detail {

inline uint32_t GuestLoad32(const Memory& memory, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, memory.Host(address), 4);
  return std::byteswap(v);
}
inline void GuestStore32(const Memory& memory, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(memory.Host(address), &value, 4);
}

struct Huft {
  uint32_t exop = 0, bits = 0, base = 0;
};
inline bool LoadHuft(const Memory& memory, uint32_t address, Huft& h) {
  if (!IsPlainMemory(address, 8)) return false;
  const uint8_t* p = memory.Host(address);
  uint32_t base;
  std::memcpy(&base, p + 4, 4);
  h = {p[0], p[1], std::byteswap(base)};
  return true;
}

inline uint32_t InflateMask(uint32_t bits) { return (1u << bits) - 1; }  // bits <= 16: zlib's inflate_mask
// The PowerPC's srw: the amount's low 6 bits, 32 to 63 giving 0.
inline uint32_t Srw(uint32_t v, uint32_t amount) { return (amount & 32) ? 0 : v >> (amount & 31); }

// The function's forward byte copy of `count` bytes inside the window: a source just behind the destination repeats
// what it has just written.
inline void WindowCopy(uint8_t* window, uint32_t dest, uint32_t source, uint32_t count) {
  if (source < dest && dest - source < count) {
    for (uint32_t i = 0; i < count; ++i) window[dest + i] = window[source + i];
  } else {
    std::memmove(window + dest, window + source, count);
  }
}

}  // namespace detail

// `hand_back_at` (tests): hand back before that symbol (0: decline) as if it were one the function does not model.
inline InflateOutcome InflateFast(const Memory& memory, const InflateFastCall& call, uint32_t& result,
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

}  // namespace any360::xdk
