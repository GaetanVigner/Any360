// The D3D self-test's cases (d3d_selftest.h): for each D3D function Any360 replaces, its replacement on the
// registers of a call and the inputs of one call, generated (random within what the titles pass). A title registers
// the cases of the functions it binds with ANY360_D3D_CASE and its own addresses and device offsets.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "any360/xdk/d3d.h"
#include "d3d_selftest.h"
#include "d3d_draw.h"
#include "xdk_natives.h"

namespace any360::host::rexglue::d3d {

// Big-endian words into guest memory.
inline void Put32(CallSetup& s, uint32_t address, uint32_t value) {
  uint8_t* p = s.Host(address);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * (3 - i)));
}

// A mask of the shapes the runtime gets: one bit, one run, all set, sparse, random. Never 0: the runtime's callers
// never pass one, and its upload functions would loop through memory for ever on it.
inline uint64_t RandomMask(std::mt19937_64& rng) {
  uint64_t m = 0;
  switch (rng() % 6) {
    case 0: m = uint64_t(1) << (rng() % 64); break;
    case 1: m = ~uint64_t(0); break;
    case 2: m = ~uint64_t(0) << (rng() % 64); break;
    case 3: {
      const uint32_t low = uint32_t(rng() % 64), length = 1 + uint32_t(rng() % (64 - low));
      m = (length == 64 ? ~uint64_t(0) : ((uint64_t(1) << length) - 1)) << low;
      break;
    }
    case 4: m = rng() & rng(); break;
    default: m = rng(); break;
  }
  return m ? m : 1;
}

// D3D's copy of the title's shader constants into the device (xdk::D3dSetShaderConstants).
struct SetShaderConstants {
  static bool Native(PPCContext& ctx, uint8_t*) {
    return any360::xdk::D3dSetShaderConstants(SelfTestMemory(), ctx.r3.u64, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                              ctx.r7.u64, ctx.r1.u32);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = rng() % 256;                                   // the first register
    ctx.r5.u64 = s.scratch + 0x100 + uint32_t(rng() % 4096);    // any alignment
    ctx.r6.u64 = rng() % 4 == 0 ? rng() % 64 : rng() % 9;       // most copies are a few vectors
    ctx.r7.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
};

// D3D's upload of the dirty shader constants (xdk::D3dDirtyConstants / D3dDirtyWords): the device's command buffer
// pointer and end (at kCursor) set into the scratch, so the call writes there and nowhere in the title's own buffer.
// Always room: out of room, the runtime's own code submits the buffer to the ring (the replacement declines those
// runs: the unit tests cover it).
inline const char* R3R4R5Differ(const PPCContext& a, const PPCContext& b) {
  if (a.r3.u64 != b.r3.u64) return "r3";
  if (a.r4.u64 != b.r4.u64) return "r4";
  if (a.r5.u64 != b.r5.u64) return "r5";
  return nullptr;
}
// kRecords: as the adapter writes them with any360_d3d_records (records, else PM4), compared through Normalize, r4
// (the cursor) not compared.
inline const char* R3R5Differ(const PPCContext& a, const PPCContext& b) {
  if (a.r3.u64 != b.r3.u64) return "r3";
  if (a.r5.u64 != b.r5.u64) return "r5";
  return nullptr;
}
template <uint32_t kCursor, bool kWords, bool kRecords = false>
struct DirtyUpload {
  static bool Native(PPCContext& ctx, uint8_t*) {
    uint64_t index = ctx.r5.u64, cursor = 0;
    const any360::xdk::Memory& m = SelfTestMemory();
    bool taken = false;
    if (kRecords)
      taken = kWords ? any360::xdk::D3dDirtyWordsRecords(m, ctx.r3.u32, kCursor, ctx.r4.u64, index, ctx.r6.u32, cursor)
                     : any360::xdk::D3dDirtyConstantsRecords(m, ctx.r3.u32, kCursor, ctx.r4.u64, index, ctx.r6.u32, cursor);
    if (!taken)
      taken = kWords ? any360::xdk::D3dDirtyWords(m, ctx.r3.u32, kCursor, ctx.r4.u64, index, ctx.r6.u32, cursor)
                     : any360::xdk::D3dDirtyConstants(m, ctx.r3.u32, kCursor, ctx.r4.u64, index, ctx.r6.u32, cursor);
    if (!taken) return false;
    ctx.r4.u64 = cursor;
    ctx.r5.u64 = index;
    return true;
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    const uint32_t end = s.scratch + 0x30000;
    Put32(s, s.scratch, cursor);  // the buffer's start, for Normalize
    Put32(s, s.device + kCursor, cursor);
    Put32(s, s.device + kCursor + 4, end);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = RandomMask(rng);
    ctx.r5.u64 = rng() % 0x4400;
    ctx.r6.u64 = s.scratch + 0x34000 + (kWords ? uint32_t(rng() % 64) * 4 : uint32_t(rng() % 64) * 16);
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = kRecords ? &R3R5Differ : &R3R4R5Differ;
  static void Normalize(CallSetup& s, PPCContext&);
};

// A state setter of xdk_natives.h (xdk::d3d_state: Native on the registers, in the title's layout) with the inputs
// of `Inputs` (a static Setup).
inline const char* NoRegisterDiffers(const PPCContext&, const PPCContext&) { return nullptr; }
template <class Kind, class Inputs>
struct State {
  static bool Native(PPCContext& ctx, uint8_t*) { return Kind::Native(SelfTestMemory(), ctx); }
  static void Setup(CallSetup& s, PPCContext& ctx) { Inputs::Setup(s, ctx); }
  // A function that returns nothing leaves r3 as its code last used it: not compared.
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) =
      xdk::KeepsR3<Kind>() ? nullptr : &NoRegisterDiffers;
};
namespace state {
using xdk::g_d3d_layout;
// A value as titles pass them (0, 1, a small enumeration) and now and then any 64 bits.
inline uint64_t Value(std::mt19937_64& rng) {
  switch (rng() % 5) {
    case 0: return 0;
    case 1: return 1;
    case 2:
    case 3: return rng() % 32;
    default: return rng();
  }
}
// The blend setters: the device's blend flags and word changed now and then (both paths of each setter).
struct Blend {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    if (rng() % 3 == 0) Put32(s, s.device + g_d3d_layout.blend_flags, uint32_t(rng()));
    if (rng() % 3 == 0) Put32(s, s.device + g_d3d_layout.blend_control, uint32_t(rng()));
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = Value(rng);
  }
};
// The bool and integer constants: registers and counts within the bank and now and then past it, a source in the
// scratch at any alignment. (A count of 0 never: the runtime's code loops on it.)
template <uint32_t kRegisters, uint32_t kBytes>
struct Constants {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t first = rng() % 8 ? uint32_t(rng() % kRegisters) : uint32_t(rng() % (4 * kRegisters));
    const uint32_t most = rng() % 8 ? std::max<uint32_t>(kRegisters - first % kRegisters, 1) : 2 * kRegisters;
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = first;
    ctx.r5.u64 = s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = 1 + rng() % most;
    (void)kBytes;
  }
};
// The resource binders: the old resource in the slot (none or a header in the scratch), a fence pending or not, the
// busy mask, the release list in the scratch with room for 32 records (never full, even over the timed calls in a
// row: the runtime would take a new block from the command buffer, which in the device copy is the title's own).
inline void Release(CallSetup& s, uint32_t slot) {
  std::mt19937_64& rng = *s.rng;
  const any360::xdk::D3dLayout& l = g_d3d_layout;
  Put32(s, s.device + slot, rng() % 4 == 0 ? 0 : s.scratch + 0x20000 + uint32_t(rng() % 0x1000));
  Put32(s, s.device + l.resource_fence, rng() % 3 == 0 ? uint32_t(rng()) : 0);
  Put32(s, s.device + l.resource_busy_mask, rng() % 4 == 0 ? 0 : uint32_t(rng()));
  const uint32_t cursor = s.scratch + 0x28000 + 8 * uint32_t(rng() % 256);
  Put32(s, s.device + l.release_cursor, cursor);
  Put32(s, s.device + l.release_end, cursor + 256);
}
struct Texture {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t stage = uint32_t(rng() % 32);
    Release(s, g_d3d_layout.texture_bound + 4 * stage);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = stage;
    ctx.r5.u64 = rng() % 4 == 0 ? 0 : s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
};
struct Stream {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t stream = uint32_t(rng() % 16);
    Release(s, g_d3d_layout.stream_bound + 4 * stream);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = stream;
    ctx.r5.u64 = rng() % 4 == 0 ? 0 : s.scratch + 0x100 + uint32_t(rng() % 0x8000);
    ctx.r6.u64 = rng() % 3 == 0 ? 0 : rng() % 0x10000;
    ctx.r7.u64 = rng() % 8 == 0 ? rng() : 4 * (rng() % 32);
    ctx.r8.u64 = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
  }
};
struct SamplerFilter {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const any360::xdk::D3dLayout& l = g_d3d_layout;
    const uint32_t stage = uint32_t(rng() % 32);
    if (rng() % 3 == 0) s.Host(s.device + l.stage_filter_index + stage)[0] = uint8_t(rng() % 8);
    if (rng() % 3 == 0) s.Host(s.device + l.stage_lod_limit + stage)[0] = uint8_t(rng());
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = stage;
    ctx.r5.u64 = rng() % 2 ? rng() % 8 : rng();
  }
};
struct VertexDeclaration {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    Release(s, g_d3d_layout.vertex_declaration);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = rng() % 4 == 0 ? 0 : s.scratch + 0x100 + uint32_t(rng() % 0x8000);
  }
};
// A shader in the scratch with a constant program writing within the first 768 bytes of the fetch constants.
inline void PutHalf(CallSetup& s, uint32_t at, uint32_t v) {
  uint8_t* p = s.Host(at);
  p[0] = uint8_t(v >> 8), p[1] = uint8_t(v);
}
template <bool kVertex>
struct Shader {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    Release(s, kVertex ? g_d3d_layout.vertex_shader : g_d3d_layout.pixel_shader);
    ctx.r3.u64 = s.device;
    if (rng() % 5 == 0) {
      ctx.r4.u64 = 0;
      return;
    }
    const uint32_t shader = s.scratch + 0x8000 + 4 * uint32_t(rng() % 0x800);
    const uint32_t block = shader + (kVertex ? 872 : 40);
    ctx.r4.u64 = shader;
    if (rng() % 6 == 0) {
      Put32(s, block + 20, 0);
      return;
    }
    const uint32_t header = s.scratch + 0x10000 + 4 * uint32_t(rng() % 0x1000);
    Put32(s, block + 20, header - block);
    if (rng() % 3 == 0) Put32(s, header + 8, 0), Put32(s, header + 12, 0);
    const uint32_t program = header + 20;
    uint32_t at = program;
    const auto place = [&](uint32_t bytes) { return uint32_t(rng() % (768 - bytes)) & ~3u; };
    for (uint32_t i = rng() % 4; i > 0; --i) PutHalf(s, at + 2, 1 + uint32_t(rng() % 100)), at += 8;
    if (rng() % 5) {
      PutHalf(s, at + 2, 0), at += 4;
      for (uint32_t i = rng() % 4; i > 0; --i) {
        const uint32_t words = 1 + uint32_t(rng() % 8);
        PutHalf(s, at, place(4 * words)), PutHalf(s, at + 2, words), at += 4 + 4 * words;
      }
      if (rng() % 3) {
        PutHalf(s, at + 2, 0), at += 4;
        for (uint32_t i = rng() % 4; i > 0; --i) {
          const uint32_t count = 2 * (1 + uint32_t(rng() % 8));
          PutHalf(s, at, place(2 * count)), PutHalf(s, at + 2, count), at += 4 + 4 * count;
        }
        if (rng() % 2) PutHalf(s, at + 2, 0), at += 4;
      }
    }
    Put32(s, header + 16, at - program);
  }
};
// The CPU range write-back: r3 the start, r4 the end (not a size: an end below the start makes the runtime's loop
// run for 2^29 lines), a range below 1 MB.
struct FlushRange {
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    ctx.r3.u64 = rng() % 4 == 0 ? 0x7F100000u + uint32_t(rng() % 0x1000000) : s.scratch + uint32_t(rng() % 0x40000);
    ctx.r4.u64 = ctx.r3.u64 + rng() % (1u << 20);
  }
};
// D3D's DrawIndexedVertices (d3d_draw.h). Its dirty groups: only the uploads (float constants, registers of a word),
// not the groups that load shaders, flush state or wait (they follow pointers the device copy shares with the title).
// The command buffer in the scratch, its start kept in the scratch's first word for Normalize; room to its end.
inline uint32_t Get32(CallSetup& s, uint32_t address) {
  const uint8_t* p = s.Host(address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void Put64(CallSetup& s, uint32_t address, uint64_t value) {
  Put32(s, address, uint32_t(value >> 32));
  Put32(s, address + 4, uint32_t(value));
}
// The command buffer from the start to `cursor` (the last word written: by default the device's cursor), decoded into
// register writes and draws in the order they come, in place of the buffer (the rest of its room zeroed); the device's
// cursor made the list's length. PM4 and Any360's records of the same writes and draws give the same list.
inline void CanonicalCommands(CallSetup& s, uint32_t cursor = 0) {
  namespace record = any360::xdk::record;
  const uint32_t start = Get32(s, s.scratch);
  if (!cursor) cursor = Get32(s, s.device + 48);
  std::vector<uint32_t> out;
  uint32_t at = start + 4;
  while (at <= cursor && out.size() < 0x4000) {
    const uint32_t h = Get32(s, at);
    at += 4;
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    switch (h >> 30) {
      case 0:
        for (uint32_t i = 0; i < count; ++i, at += 4)
          out.insert(out.end(), {0, (h & 0x8000) ? (h & 0x7FFF) : (h & 0x7FFF) + i, Get32(s, at)});
        break;
      case 1:
        out.insert(out.end(), {0, h & 0x7FF, Get32(s, at), 0, (h >> 11) & 0x7FF, Get32(s, at + 4)});
        at += 8;
        break;
      case 2:
        break;
      default: {
        const uint32_t opcode = (h >> 8) & 0x7F;
        uint32_t raw[8] = {};
        for (uint32_t i = 0; i < 8 && i < count; ++i) std::memcpy(&raw[i], s.Host(at + 4 * i), 4);
        const uint32_t predicated = h & 1;
        if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kDraw && count == record::DrawWords(true)) {
          out.insert(out.end(), {1, raw[1], raw[2], raw[3], predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kDraw && count == record::DrawWords(false)) {
          out.insert(out.end(), {1, raw[1], 0, 0, predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kBins && count == record::kBinsWords) {
          out.insert(out.end(), {3, raw[0] >> 8, raw[1], predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kRegisters) {
          for (uint32_t i = 1; i < count; ++i) {
            uint32_t v;
            std::memcpy(&v, s.Host(at + 4 * i), 4);
            out.insert(out.end(), {0, (raw[0] >> 8) + i - 1, v});
            if (predicated) out.push_back(1);
          }
        } else if (opcode == 0x22 && count == 4) {  // DRAW_INDX with DMA indices: its query dword is not drawn
          out.insert(out.end(), {1, Get32(s, at + 4), Get32(s, at + 8), Get32(s, at + 12), predicated});
        } else if (opcode == 0x22 && count == 2) {  // DRAW_INDX with auto indices
          out.insert(out.end(), {1, Get32(s, at + 4), 0, 0, predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kShader) {
          out.insert(out.end(), {5, raw[0] >> 8, count - 1, predicated});
          for (uint32_t i = 1; i < count; ++i) out.push_back(Get32(s, at + 4 * i));
        } else if (opcode == 0x2B && count >= 2) {  // IM_LOAD_IMMEDIATE
          const uint32_t dwords = Get32(s, at + 4) & 0xFFFF;
          out.insert(out.end(), {5, Get32(s, at), dwords, predicated});
          for (uint32_t i = 0; i < dwords && 2 + i < count; ++i) out.push_back(Get32(s, at + 8 + 4 * i));
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kInvalidate) {
          out.insert(out.end(), {6, raw[1], predicated});
        } else if (opcode == 0x3B && count == 1) {  // INVALIDATE_STATE
          out.insert(out.end(), {6, Get32(s, at), predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kWait) {
          out.insert(out.end(), {7, raw[1], raw[2], raw[3], raw[4], raw[5], predicated});
        } else if (opcode == 0x3C && count == 5) {  // WAIT_REG_MEM
          out.insert(out.end(), {7, Get32(s, at), Get32(s, at + 4), Get32(s, at + 8), Get32(s, at + 12), Get32(s, at + 16),
                                 predicated});
        } else if (opcode == 0x2D && count >= 1) {  // SET_CONSTANT: registers from its type's base
          static constexpr uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t offset_type = Get32(s, at), type = (offset_type >> 16) & 0xFF;
          const uint32_t first = (type < 5 ? kBase[type] : 0) + (offset_type & 0x7FF);
          for (uint32_t i = 1; i < count; ++i) {
            out.insert(out.end(), {0, first + i - 1, Get32(s, at + 4 * i)});
            if (predicated) out.push_back(1);
          }
        } else if (opcode == 0x36 && count == 1) {  // DRAW_INDX_2 with auto indices
          out.insert(out.end(), {1, Get32(s, at), 0, 0, predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kEvent) {
          out.insert(out.end(), {4, raw[0] >> 8, count - 1, predicated});
          for (uint32_t i = 1; i < count && i < 8; ++i) out.push_back(raw[i]);
        } else if (opcode == 0x46 || opcode == 0x58 || opcode == 0x5A || opcode == 0x5B) {  // the events
          out.insert(out.end(), {4, opcode, count, predicated});
          for (uint32_t i = 0; i < count && i < 7; ++i) out.push_back(Get32(s, at + 4 * i));
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kShaderLoad) {
          out.insert(out.end(), {8, raw[1], raw[2], predicated});
        } else if (opcode == 0x27 && count == 2) {  // IM_LOAD
          out.insert(out.end(), {8, Get32(s, at), Get32(s, at + 4), predicated});
        } else if (opcode == record::kOpcode && (raw[0] & 0xFF) == record::kConstantLoad) {
          out.insert(out.end(), {9, raw[1], raw[2], raw[3], predicated});
        } else if (opcode == 0x2F && count == 3) {  // LOAD_ALU_CONSTANT
          out.insert(out.end(), {9, Get32(s, at), Get32(s, at + 4), Get32(s, at + 8), predicated});
        } else if (opcode >= 0x60 && opcode <= 0x63 && count == 1) {  // SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI
          out.insert(out.end(), {3, opcode - 0x60, Get32(s, at), predicated});
        } else {
          out.insert(out.end(), {2, opcode, count});
          for (uint32_t i = 0; i < count; ++i) out.push_back(Get32(s, at + 4 * i));
        }
        at += 4 * count;
      }
    }
  }
  std::memset(s.Host(start + 4), 0, s.scratch + 0x30000 - (start + 4));
  std::memcpy(s.Host(start + 4), out.data(), 4 * out.size());
  Put32(s, s.device + 48, uint32_t(out.size()));
}
// kHelpers: every dirty group, the title's shader, state, misc and wait helpers called too (they read through the
// device copy's pointers, which are the title's: shaders and buffers, read only by them).
// The Begin draws (d3d_draw.h): the draws' device, the runtime's allocator given a segment in the scratch (its
// descriptor at +13140 a fake one whose word at +152 is set, its top at +14540), failing now and then (bit 6 of
// +10813). BeginIndexedVertices' index pointer in the scratch, its vertex pointer the ninth argument (the caller's
// stack word at +84). Their packet lies past the cursor (EndVertices moves it): decoded up to +13076.
template <bool kRecords, bool kIndexed>
struct Begin {
  static bool Native(PPCContext& ctx, uint8_t* base) {
    return kIndexed ? d3d_draw::BeginIndexedVertices(ctx, base, SelfTestMemory(), kRecords)
                    : d3d_draw::BeginVertices(ctx, base, SelfTestMemory(), kRecords);
  }
  static void Setup(CallSetup& s, PPCContext& ctx);
  // r3 compared: the block, or the HRESULT.
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
  static void Normalize(CallSetup& s, PPCContext& after) {
    if constexpr (!kRecords) return;
    const bool written = kIndexed ? after.r3.u32 == 0 : after.r3.u32 != 0;
    CanonicalCommands(s, written ? Get32(s, s.device + 13076) : 0);
  }
};

// kIndexed false: DrawVertices (r5 the first vertex, r6 the count).
template <bool kRecords, bool kHelpers = false, bool kIndexed = true>
struct DrawIndexed {
  static bool Native(PPCContext& ctx, uint8_t* base) {
    return kIndexed ? d3d_draw::DrawIndexed(ctx, base, SelfTestMemory(), kRecords)
                    : d3d_draw::DrawVertices(ctx, base, SelfTestMemory(), kRecords);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t start = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.scratch, start);
    Put32(s, s.device + 48, start);
    Put32(s, s.device + 52, s.scratch + 0x30000);
    Put32(s, s.device + 56, s.scratch + 0x2F000);
    Put64(s, s.device + 0, rng() % 2 ? RandomMask(rng) : 0);
    Put64(s, s.device + 8, rng() % 2 ? RandomMask(rng) : 0);
    const uint32_t busy_hi = Get32(s, s.device + 40), busy_lo = Get32(s, s.device + 44);
    const uint64_t busy = uint64_t(busy_hi) << 32 | busy_lo;
    // Sparse masks too (a bit, a run, a few bits): a group's test and its upload's mask must agree bit for bit.
    if (kHelpers) {
      Put64(s, s.device + 16, rng() % 2 ? RandomMask(rng) : 0);
      Put64(s, s.device + 24, rng() % 2 ? RandomMask(rng) : 0);
      Put64(s, s.device + 32, rng() % 2 ? RandomMask(rng) : 0);
    } else {
      Put64(s, s.device + 16, rng() % 2 ? RandomMask(rng) & ~uint64_t(0x001E0000) & ~busy : 0);
      Put64(s, s.device + 24, rng() % 2 ? RandomMask(rng) & 0x007FFFFC00000000ull : 0);
      Put64(s, s.device + 32, rng() % 2 ? RandomMask(rng) & (0x0000003FFFFFFFFFull | 0x00003FC000000000ull | (uint64_t(1) << 56)) : 0);
    }
    s.Host(s.device + 10812)[0] &= 0xFD;
    s.Host(s.device + 11907)[0] = uint8_t(rng() % 4);
    const uint32_t declaration = s.scratch + 0x8000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.device + 12156, declaration);
    static constexpr uint32_t kPrimitives[] = {1, 2, 3, 4, 5, 6, 8, 13};
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = kPrimitives[rng() % 8];
    ctx.r5.u64 = uint32_t(rng());
    ctx.r6.u64 = rng() % 0x10000;
    ctx.r7.u64 = rng() % 16 == 0 ? 65536 + rng() % 200000 : 1 + rng() % 3000;
    if (!kIndexed) ctx.r5.u64 = rng() % 0x10000, ctx.r6.u64 = ctx.r7.u64;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// SetPredication (d3d_draw.h): the draws' device with its tiling flags, bound surfaces and query bytes changed now and
// then, a mask of 0 (all tiles) one call in three.
template <bool kRecords>
struct Predication {
  static bool Native(PPCContext& ctx, uint8_t* base) {
    return d3d_draw::SetPredication(ctx, base, SelfTestMemory(), kRecords);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    DrawIndexed<kRecords>::Setup(s, ctx);
    std::mt19937_64& rng = *s.rng;
    s.Host(s.device + 10812)[0] = uint8_t(rng());
    s.Host(s.device + 10815)[0] = uint8_t(rng());
    s.Host(s.device + 11841)[0] = rng() % 2 ? 0 : uint8_t(rng());
    s.Host(s.device + 11907)[0] = rng() % 2 ? 0 : uint8_t(rng() % 4);
    for (uint32_t k = 0; k < 5; ++k) {
      const uint32_t surface = rng() % 3 == 0 ? 0 : uint32_t(rng() % 4);
      Put32(s, s.device + 12160 + 4 * k, surface);
      Put32(s, s.device + 12448 + 4 * k, rng() % 2 ? surface : uint32_t(rng() % 4));
    }
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = rng() % 3 == 0 ? 0 : uint32_t(rng());
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// IDirect3DQuery9::Issue (d3d_draw.h): a query in the scratch on the device copy, of type 8, 9 (occlusion: the
// runtime's helper writes its packets), 10 (event) or another, its result blocks in the scratch, begun, ended or
// neither; the device tiling (its block count from +12468) now and then.
template <bool kRecords>
struct Query {
  static bool Native(PPCContext& ctx, uint8_t* base) { return d3d_draw::QueryIssue(ctx, base, SelfTestMemory(), kRecords); }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    DrawIndexed<kRecords>::Setup(s, ctx);
    std::mt19937_64& rng = *s.rng;
    const uint32_t query = s.scratch + 0x5000;
    Put32(s, query, s.device);
    static constexpr uint32_t kTypes[] = {8, 9, 10, 10, 4};
    Put32(s, query + 4, kTypes[rng() % 5]);
    for (uint32_t i = 0; i < 30; ++i) Put32(s, query + 24 + 4 * i, s.scratch + 0x5800 + 16 * i);
    Put32(s, query + 144, 1 + uint32_t(rng() % 4));  // the blocks a begin counted (an end's helper walks them)
    uint8_t& tiling = s.Host(s.device + 10812)[0];
    tiling = rng() % 3 == 0 ? tiling | 0x20 : tiling & ~0x20;
    Put32(s, s.device + 12468, 1 + uint32_t(rng() % 4));
    static constexpr uint32_t kFlags[] = {1, 2, 3, 0};
    ctx.r3.u64 = query;
    ctx.r4.u64 = kFlags[rng() % 4];
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// The clear (d3d_draw.h): the draws' device (its bound surfaces the title's: the runtime's target clear reads them),
// the viewport's floats (now and then NaN or huge: fctiwz), the scissor, the rect, the flags, the colors (none: the
// default), the depth surface absent one call in four.
template <bool kRecords>
struct ClearCase {
  static bool Native(PPCContext& ctx, uint8_t* base) { return d3d_draw::Clear(ctx, base, SelfTestMemory(), kRecords); }
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    DrawIndexed<false>::Setup(s, ctx);
    std::mt19937_64& rng = *s.rng;
    // The target clear takes blocks through the runtime's allocator: a segment in the scratch (as for the Begin
    // draws), never failing (without one it waits for the GPU on the title's ring).
    const uint32_t segment = s.scratch + 0x7000;
    Put32(s, s.device + 13140, segment);
    Put32(s, segment + 152, 1);
    Put32(s, s.device + 14540, s.scratch + 0x2E000);
    s.Host(s.device + 10813)[0] &= ~0x40;
    const auto coordinate = [&] { return uint32_t(int32_t(rng() % 1500) - 100); };
    for (uint32_t k = 0; k < 4; ++k) {
      float f = float(rng() % 1400);
      if (rng() % 32 == 0) f = rng() % 2 ? std::bit_cast<float>(0x7FC00000u) : 3.0e9f;
      Put32(s, s.device + 12368 + 4 * k, std::bit_cast<uint32_t>(f));
      Put32(s, s.device + 12396 + 4 * k, coordinate());
    }
    Put32(s, s.device + 11576, rng() % 2);
    if (rng() % 4 == 0) Put32(s, s.device + 12176, 0);
    const uint32_t rect = s.scratch + 0x6100;
    for (uint32_t k = 0; k < 4; ++k) Put32(s, rect + 4 * k, coordinate());
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = rng() % 64;
    ctx.r5.u64 = rect;
    ctx.r6.u64 = rng() % 3 == 0 ? 0 : s.scratch + 0x6200;
    ctx.r8.u64 = rng() % 256;
    ctx.f1.f64 = double(rng() % 1000) / 1000.0;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
};

// The resolve (d3d_draw.h): the draws' device with an allocator segment in the scratch (the copy's rect vertices),
// a destination texture made in the scratch (format, pitch, size, tiling, endianness, kind), level and slice 0, the
// source rect, point and clear color given or not, the color and depth clears after the copy one call in eight each,
// the caller's two stack arguments set.
template <bool kRecords>
struct ResolveCase {
  static bool Native(PPCContext& ctx, uint8_t* base) { return d3d_draw::Resolve(ctx, base, SelfTestMemory(), kRecords); }
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    DrawIndexed<false>::Setup(s, ctx);
    std::mt19937_64& rng = *s.rng;
    const uint32_t segment = s.scratch + 0x7000;
    Put32(s, s.device + 13140, segment);
    Put32(s, segment + 152, 1);
    Put32(s, s.device + 14540, s.scratch + 0x2E000);
    s.Host(s.device + 10813)[0] &= ~0x40;
    const uint32_t texture = s.scratch + 0x6400;
    Put32(s, texture + 28, (uint32_t(1 + rng() % 64) << 22) | (uint32_t(rng() % 4) << 2));
    Put32(s, texture + 32, (s.scratch & 0xFFFFF000u) | uint32_t(rng() % 64) | (uint32_t(rng() % 4) << 6));
    Put32(s, texture + 36, uint32_t(rng() % 1280) | (uint32_t(rng() % 720) << 13));
    Put32(s, texture + 40, uint32_t(rng()));
    Put32(s, texture + 48, uint32_t(rng() % 4) << 9);
    uint32_t flags = uint32_t(rng() % 5);
    const uint32_t surface = flags == 4 ? Get32(s, s.device + 12176) : Get32(s, s.device + 12160 + 4 * flags);
    if (rng() % 2 || !surface) flags |= uint32_t(1 + rng() % 7) << 4;
    if (rng() % 8 == 0) flags |= 0x100;
    if (rng() % 8 == 0) flags |= 0x200;
    if (rng() % 4 == 0) flags |= 0x2000;
    flags |= uint32_t(rng() % 64) << 26;
    const uint32_t rect = s.scratch + 0x6500, point = s.scratch + 0x6520, colors = s.scratch + 0x6540;
    Put32(s, rect, uint32_t(rng() % 640)), Put32(s, rect + 4, uint32_t(rng() % 360));
    Put32(s, rect + 8, uint32_t(640 + rng() % 640)), Put32(s, rect + 12, uint32_t(360 + rng() % 360));
    Put32(s, point, uint32_t(rng() % 64)), Put32(s, point + 4, uint32_t(rng() % 64));
    // The tenth argument: a surface for the clears or 0 (the destination texture made here as one).
    Put32(s, s.stack + 92, uint32_t(rng() % 256)), Put32(s, s.stack + 100, rng() % 2 ? 0 : texture);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = flags;
    ctx.r5.u64 = rng() % 3 ? rect : 0;
    ctx.r6.u64 = texture;
    ctx.r7.u64 = rng() % 2 ? point : 0;
    // Level and slice 0: the runtime's level helper (the other levels' geometry) follows the texture's fields, which a
    // texture made here does not fill as a real one does. That path calls the helper with the same arguments.
    ctx.r8.u64 = 0, ctx.r9.u64 = 0;
    ctx.r10.u64 = rng() % 2 ? colors : 0;
    ctx.f1.f64 = double(rng() % 1000) / 1000.0;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
};

// The draws' fetch-constant and wait-group helpers (d3d_draw.h): the device copy's command buffer in the scratch (its
// start in the scratch's first word), every pointer alignment, masks of every shape (never 0).
template <bool kRecords, bool kWait>
struct GroupHelper {
  static bool Native(PPCContext& ctx, uint8_t*) {
    return kWait ? d3d_draw::FlushWaitGroup(ctx, SelfTestMemory(), kRecords)
                 : d3d_draw::FlushFetchConstants(ctx, SelfTestMemory(), kRecords);
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.scratch, cursor);
    Put32(s, s.device + 48, cursor);
    Put32(s, s.device + 52, s.scratch + 0x30000);
    Put32(s, s.device + 56, s.scratch + 0x2F000);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = RandomMask(rng);
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// The draws' state helper (d3d_draw.h): every path (tiled or not, the five words matching or not, every +11572 mode,
// 1-4 tiles mostly, up to 19), random values, r3 (the bits left) compared.
template <bool kRecords>
struct StateHelper {
  static bool Native(PPCContext& ctx, uint8_t*) { return d3d_draw::FlushState(ctx, SelfTestMemory(), kRecords); }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.scratch, cursor);
    Put32(s, s.device + 48, cursor);
    Put32(s, s.device + 52, s.scratch + 0x30000);
    Put32(s, s.device + 56, s.scratch + 0x2F000);
    s.Host(s.device + 10815)[0] = uint8_t(rng());
    s.Host(s.device + 10812)[0] = uint8_t(rng());
    for (uint32_t k = 0; k < 5; ++k) {
      const uint32_t want = rng() % 3 == 0 ? 0 : uint32_t(rng() % 4);
      Put32(s, s.device + 12160 + 4 * k, want);
      Put32(s, s.device + 12448 + 4 * k, rng() % 4 ? want : uint32_t(rng() % 4));
    }
    Put32(s, s.device + 12468, rng() % 16 == 0 ? uint32_t(rng() % 20) : 1 + uint32_t(rng() % 4));
    for (uint32_t i = 0; i < 16; ++i) Put32(s, s.device + 12832 + 4 * i, uint32_t(rng()));
    for (uint32_t at : {11572u, 12428u, 12432u, 10240u, 12896u, 10332u}) Put32(s, s.device + at, uint32_t(rng()));
    const auto some = [&](uint64_t v) { return v | (rng() % 2 ? uint64_t(1) << 57 : 0) | (rng() % 2 ? uint64_t(1) << 37 : 0); };
    Put64(s, s.device + 40, some(rng() % 2 ? rng() : 0));
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = some(rng() % 2 ? rng() : 0) | (rng() % 4 ? 0x100 : 0);
    ctx.r5.u64 = rng();
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// The draws' shader helper (d3d_draw.h): a vertex and a pixel shader built in the scratch (their parts, constant
// lists and microcode entries at random, the interpolators matching or not), every device field the helper reads at
// random, no pixel shader a third of the time, no vertex shader now and then. The calls that would patch or link the
// shaders (sub_826C6B00, sub_826C68E0: they write the title's shaders, its shader counter, and allocate) are kept
// from happening: their dirty bits cleared, or +11540 zeroed.
template <bool kRecords>
struct ShaderHelper {
  static bool Native(PPCContext& ctx, uint8_t* base) {
    return d3d_draw::FlushShaders(ctx, base, SelfTestMemory(), kRecords);
  }
  // A constant list at `at`, its values within 0x2100 bytes of the data's base (a title's hook reads them).
  static void List(CallSetup& s, std::mt19937_64& rng, uint32_t at) {
    const uint32_t n = uint32_t(rng() % 5);
    Put32(s, at + 16, 8 * n + (rng() % 4 == 0 ? 8 : 0));
    for (uint32_t i = 0; i <= n; ++i) {
      const uint32_t dwords = rng() % 16 == 0 ? 0 : 1 + uint32_t(rng() % 64);
      Put32(s, at + 20 + 8 * i, uint32_t(rng() % 0x400) << 16 | dwords);
      Put32(s, at + 24 + 8 * i, 4 * uint32_t(rng() % 0x800));
    }
  }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.scratch, cursor);
    Put32(s, s.device + 48, cursor);
    Put32(s, s.device + 52, s.scratch + 0x30000);
    Put32(s, s.device + 56, s.scratch + 0x2F000);
    const uint32_t vs = s.scratch + 0x2000, ps = s.scratch + 0x5000;
    Put32(s, vs + 32, s.scratch + 0x8000);
    Put32(s, ps + 24, s.scratch + 0x8000);
    Put32(s, vs + 892, rng() % 4 ? 0x400 - 872 : 0);
    Put32(s, vs + 896, 0x500);
    Put32(s, vs + 904, 0x600);
    List(s, rng, vs + 0x400);
    Put32(s, ps + 60, rng() % 4 ? 0x300 - 40 : 0);
    Put32(s, ps + 64, 0x100);
    List(s, rng, ps + 0x300);
    const bool has_ps = rng() % 3 != 0, has_vs = rng() % 16 != 0;
    Put32(s, s.device + 12416, has_vs ? vs : 0);
    Put32(s, s.device + 12412, has_ps ? ps : 0);
    const uint32_t pixel_mask = uint32_t(rng());
    Put32(s, ps + 0x100 + 40 + 20, pixel_mask);
    for (uint32_t part : {vs + 0x500 + 872, vs + 0x600 + 872})
      Put32(s, part + 20, rng() % 2 ? (has_ps ? pixel_mask : pixel_mask & ~0x1Fu) : uint32_t(rng()));
    for (uint32_t at : {10428u, 12428u, 12432u, 11572u, 10420u, 10432u}) Put32(s, s.device + at, uint32_t(rng()));
    const uint32_t modes[] = {4, 5, uint32_t(rng() % 8)};
    const uint32_t interpolation = (uint32_t(rng()) & ~7u) | modes[rng() % 3];
    Put32(s, s.device + 10452, interpolation);
    Put32(s, s.device + 12420, rng() % 2 ? (interpolation & ~7u) | (rng() % 2 ? 5 : interpolation & 7) : uint32_t(rng()));
    for (uint32_t at : {10812u, 10813u, 10814u}) s.Host(s.device + at)[0] = uint8_t(rng());
    Put32(s, s.device + 11540, rng() % 2 ? uint32_t(rng()) : 0);
    uint64_t bits = (rng() & ~uint64_t(0x1E0008)) | uint64_t(rng() % 16) << 17 | uint64_t(rng() % 2) << 3;
    if (has_vs) {
      const uint32_t part = (!has_ps && (Get32(s, vs + 872) & 0x20)) ? vs + 0x600 + 872 : vs + 0x500 + 872;
      uint64_t will = bits | ((bits & 0x20000) ? 0x100 : 0);
      const uint32_t mode = interpolation & 7;
      if (!has_ps && mode != 5) will |= 0x80008;
      if (has_ps && (will & 0x100000) && mode != 4) will |= 0x80008;
      const uint32_t mismatch = has_ps ? (pixel_mask ^ Get32(s, part + 20)) & 0x7FFFFu : Get32(s, part + 20) & 0x1Fu;
      const bool linked = (s.Host(s.device + 10813)[0] & 1) || mismatch;
      if (linked && (will & 0x180000)) {
        bits &= ~uint64_t(0x180000);
        if (!has_ps) Put32(s, s.device + 10452, (interpolation & ~7u) | 5);
      } else if (!linked && (will & 0x80000) && !(Get32(s, vs + 872) & 0x40)) {
        // The patched copies' path: no link (+10812 bit 7), the first copy cached; the odd tiles' copy cached or
        // still read by the GPU (never one the runtime would patch).
        s.Host(s.device + 10812)[0] &= 0x7F;
        if (Get32(s, s.device + 11540)) {
          const uint32_t patch = s.scratch + 0x7000, done = s.scratch + 0x7100;
          Put32(s, s.device + 11540, patch);
          Put32(s, s.device + 10768, done);
          const uint32_t now = uint32_t(rng());
          Put32(s, s.device + 10780, now);
          Put32(s, done, now - 100);
          const uint32_t which = (!has_ps && (Get32(s, vs + 872) & 0x20)) ? 1 : 0;
          const auto at64 = [&](uint32_t a) { return uint64_t(Get32(s, a)) << 32 | Get32(s, a + 4); };
          for (uint32_t e : {vs, vs + 416}) {
            const bool busy = e != vs + 416 * which && rng() % 3 == 0;
            Put32(s, e + 40, busy ? ~Get32(s, patch + 48) : Get32(s, patch + 48));
            Put64(s, e + 48, at64(s.device + 12248) ^ (rng() & ~at64(patch + 32)));
            Put64(s, e + 56, at64(s.device + 12256) ^ (rng() & ~at64(patch + 40)));
            Put32(s, e + 64, busy ? now - uint32_t(rng() % 100) : uint32_t(rng()));
          }
        }
      }
    }
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = bits;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = nullptr;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

// The shaders' literal constants (sub_826C6A50): a list in the scratch (as the shader helper's), its data there.
template <bool kRecords>
struct ConstantListCase {
  static bool Native(PPCContext& ctx, uint8_t*) { return d3d_draw::ConstantList(ctx, SelfTestMemory(), kRecords); }
  static void Setup(CallSetup& s, PPCContext& ctx) {
    std::mt19937_64& rng = *s.rng;
    const uint32_t cursor = s.scratch + 0x10000 + 4 * uint32_t(rng() % 64);
    Put32(s, s.scratch, cursor);
    Put32(s, s.device + 48, cursor);
    Put32(s, s.device + 52, s.scratch + 0x30000);
    Put32(s, s.device + 56, s.scratch + 0x2F000);
    const uint32_t part = s.scratch + 0x2000;
    Put32(s, part + 20, rng() % 8 ? 0x400 : 0);
    ShaderHelper<kRecords>::List(s, rng, part + 0x400);
    ctx.r3.u64 = s.device;
    ctx.r4.u64 = part;
    ctx.r5.u64 = s.scratch + 0x8000;
  }
  static constexpr const char* (*Differs)(const PPCContext&, const PPCContext&) = &NoRegisterDiffers;
  static void Normalize(CallSetup& s, PPCContext&) {
    if constexpr (kRecords) CanonicalCommands(s);
  }
};

template <bool kRecords, bool kIndexed>
void Begin<kRecords, kIndexed>::Setup(CallSetup& s, PPCContext& ctx) {
  DrawIndexed<kRecords, true>::Setup(s, ctx);
  std::mt19937_64& rng = *s.rng;
  const uint32_t segment = s.scratch + 0x7000;
  Put32(s, s.device + 13140, segment);
  Put32(s, segment + 152, 1);
  Put32(s, s.device + 14540, s.scratch + 0x2E000);
  uint8_t& flags = s.Host(s.device + 10813)[0];
  flags = rng() % 8 == 0 ? flags | 0x40 : flags & ~0x40;
  const uint64_t primitive = ctx.r4.u64;
  ctx.r4.u64 = primitive;
  if (kIndexed) {
    ctx.r5.u64 = uint32_t(rng());
    ctx.r6.u64 = 1 + rng() % 256;
    ctx.r7.u64 = 1 + rng() % 512;
    ctx.r8.u64 = rng() % 8;
    ctx.r9.u64 = 4 * (1 + rng() % 16);
    ctx.r10.u64 = s.scratch + 0x6000;
    Put32(s, s.stack + 84, s.scratch + 0x6004);
  } else {
    ctx.r5.u64 = 1 + rng() % 256;
    ctx.r6.u64 = 4 * (1 + rng() % 16);
  }
}
// SetPixelShaderConstantF: as SetShaderConstants.
struct PixelFloats {
  static void Setup(CallSetup& s, PPCContext& ctx) { SetShaderConstants::Setup(s, ctx); }
};
}  // namespace state

template <uint32_t kCursor, bool kWords, bool kRecords>
void DirtyUpload<kCursor, kWords, kRecords>::Normalize(CallSetup& s, PPCContext&) {
  if constexpr (kRecords) {
    // The device's cursor field at kCursor (48 here, as the canonical decoding reads).
    if constexpr (kCursor == 48) state::CanonicalCommands(s, 0);
  }
}


}  // namespace any360::host::rexglue::d3d
