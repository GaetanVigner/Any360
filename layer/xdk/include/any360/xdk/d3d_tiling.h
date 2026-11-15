// The XDK's predicated tiling, replayed (docs/D3D.md, D6). EndTiling hands the bracket's tile ops list to a DPC (from
// the fence's interrupt), whose interpreter (Crackdown sub_826CDAF8) runs the ops: each recorded segment submitted
// once a tile, each tile's bin select set before its segments (TileSelect), the tiles' resource lists walked. The
// ops are words with their top bit set (the top byte the op); any other top byte is a link to the next block (its
// address + 4), 0xC0000000 the list's end; a word without the top bit ends the run too.
//
//   0x80  the bracket's header, 248 bytes copied to the state's +116 (+116 its flags, +120 the tile count); the tile
//         (+44) back to 0, the select in force (+364) 0x7FFFFFFF.
//   0x81  the run's end.
//   0x82  a segment submitted: the low 24 bits its dwords, the next word its address.
//   0x83  not written yet: read again until it is.
//   0x84  a pass over the tiles begins (its address kept at +368); its flags (+40) get bit 30, and bit 31 from the
//         second tile on when the header's bit 1 is set; the tile selected.
//   0x85  that pass ends (bit 30 cleared): back to its op +372's after when bit 31 is set, else the tile selected.
//   0x86  bit 31 flipped (its address kept at +372): set, back to the pass's op (+368); clear, the tile selected.
//   0x87  the tile loop's begin (its address kept at +376, flags bit 29), the tile selected.
//   0x88  the tile loop's end: the next tile (+44), back to +376's op while one is left, else the tile selected.
//   0x89  the L2 cache locked (128 KB zeroed at 0x7F100000).
//   0x8A  the GPU waited for, the L2 unlocked.
//   0x8B  with bit 31 of the select in force, the tiles' visibility of resources computed: the next word's list,
//         then each list of the blocks from the word after.
//   0x8C  with the select in force having a bit of the next word, the run goes on at the state's +96 (+80 where to
//         come back).
// The state: +40 flags, +44 the tile, +80, +96, +116 the header, +364 the select in force, +368 +372 +376 addresses.
#pragma once

#include <cstdint>
#include <cstring>

#include "any360/xdk/d3d_state.h"

namespace any360::xdk {

// TileSelect's bin select (sub_826CD7D8) from the interpreter's flags (+40: bit 31 the odd tiles' bits, bit 30 the
// even ones', bit 29 one tile, +44, alone) and the tile.
inline uint32_t TileMask(uint32_t flags, uint32_t tile) {
  uint32_t mask = (flags & 0x80000000u) ? 0x2AAAAAAAu : (flags & 0x40000000u) ? 0x15555555u : 0xFFFFFFFFu;
  if (flags & 0x20000000u) {
    const uint32_t shift = (tile << 1) & 63;  // slw: 0 from 32 on
    mask &= shift < 32 ? 3u << shift : 0;
    if (!tile && !(flags & 0x80000000u)) mask |= 0x80000000u;
  }
  return mask;
}

// What the interpreter calls (the title's functions, or a test's).
struct TileOpsCalls {
  virtual ~TileOpsCalls() = default;
  virtual void Select(uint32_t state) = 0;                                // TileSelect (sub_826CD7D8)
  virtual void Submit(uint32_t dwords, uint32_t address) = 0;            // the ring's submit (sub_826BAD58)
  virtual void Resources(uint32_t header, uint32_t list, bool flush) = 0;  // sub_826CD9F8
  virtual void Wait(uint32_t state) = 0;                                  // sub_826CD950
  virtual void LockL2() = 0;    // KeLockL2(0, 0x7F100000, 0x20000, 1, 1) and the 128 KB zeroed
  virtual void UnlockL2() = 0;  // KeUnlockL2(0)
  virtual void Again() = 0;     // a 0x83 op read: before it is read again
};

// Runs the ops from `at` with the interpreter's `state` -> where the run stopped (the 0x81 op, the list's end, or the
// first word without its top bit).
inline uint32_t RunTileOps(const Memory& m, uint32_t at, uint32_t state, TileOpsCalls& calls) {
  using detail::At32;
  using detail::Set32;
  const auto word = [&](uint32_t a) { return At32(m.Host(a)); };
  const auto set = [&](uint32_t a, uint32_t v) { Set32(m.Host(a), v); };
  uint32_t op = word(at);
  while (op & 0x80000000u) {
    bool select = false;
    switch (op >> 24) {
      case 0x80:
        std::memcpy(m.Host(state + 116), m.Host(at + 4), 248);
        at += 252;
        set(state + 44, 0), set(state + 364, 0x7FFFFFFFu);
        break;
      case 0x81:
        return at;
      case 0x82:
        calls.Submit(op & 0x00FFFFFFu, word(at + 4));
        at += 8;
        break;
      case 0x83:
        calls.Again();
        break;
      case 0x84: {
        const uint32_t tile = word(state + 44), flags = word(state + 40) | 0x40000000u;
        set(state + 368, at), set(state + 40, flags);
        at += 4;
        if (tile && (word(state + 116) & 2)) set(state + 40, flags | 0x80000000u);
        select = true;
        break;
      }
      case 0x85: {
        const uint32_t flags = word(state + 40) & ~0x40000000u;
        set(state + 40, flags);
        at += 4;
        if (flags & 0x80000000u) {
          at = word(state + 372);
        } else {
          select = true;
        }
        break;
      }
      case 0x86: {
        set(state + 372, at);
        at += 4;
        const uint32_t flags = word(state + 40) ^ 0x80000000u;
        set(state + 40, flags);
        if (flags & 0x80000000u) {
          at = word(state + 368);
        } else {
          select = true;
        }
        break;
      }
      case 0x87:
        set(state + 376, at);
        at += 4;
        set(state + 40, word(state + 40) | 0x20000000u);
        select = true;
        break;
      case 0x88: {
        const uint32_t tile = word(state + 44) + 1, flags = word(state + 40) & ~0x20000000u, count = word(state + 120);
        at += 4;
        set(state + 44, tile), set(state + 40, flags);
        if (tile >= count) {
          select = true;
        } else {
          at = word(state + 376);
        }
        break;
      }
      case 0x89:
        calls.LockL2();
        at += 4;
        break;
      case 0x8A:
        calls.Wait(state);
        calls.UnlockL2();
        at += 4;
        break;
      case 0x8B:
        if (word(state + 364) & 0x80000000u) {
          const uint32_t header = state + 116;
          uint32_t block = word(at + 8);
          calls.Resources(header, word(at + 4), false);
          for (;;) {
            const uint32_t end = word(block);
            uint32_t p = block + 4;
            for (; p < end; p += 4) calls.Resources(header, word(p), true);
            block = word(p);
            if (block == 0xC0000000u) break;
          }
        }
        at += 12;
        break;
      case 0x8C: {
        const uint32_t mask = word(at + 4);
        at += 8;
        if (word(state + 364) & mask) set(state + 80, at), at = state + 96;
        break;
      }
      default:
        if (op == 0xC0000000u) return at;
        at = op + 4;
        break;
    }
    if (select) calls.Select(state);
    op = word(at);
  }
  return at;
}

}  // namespace any360::xdk
