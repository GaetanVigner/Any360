// Any360's own command records (docs/D3D.md, phase D2): what Any360's native D3D writes into a title's command
// buffer in place of PM4, in the host's byte order. A record is a type-3 packet (its header big-endian, as every packet
// the command processor reads, so that records and the runtime's PM4 still in the same buffer keep their order) with
// an opcode no XDK runtime emits; its words follow in the host's byte order, the first being the record's kind and
// argument:
//
//   kRegisters  argument: the first register. Then the registers' values (what a type-0 packet of the same registers
//               sets, without its byte swap).
//   kDraw       argument: the draw's source as in the initiator (0 DMA indices, 2 auto). Then the initiator (as
//               VGT_DRAW_INITIATOR) and, for DMA indices, VGT_DMA_BASE and VGT_DMA_SIZE: what DRAW_INDX draws, without
//               its visibility query dword (the command processor reads the query from PA_SC_VIZ_QUERY).
//   kBins       argument: 0 the bin mask's low half, 1 its high half, 2 the bin select's low half, 3 its high half
//               (SET_BIN_MASK_LO and the others). Then the value.
//   kEvent      argument: the PM4 event packet it stands for (EVENT_WRITE 0x46, EVENT_WRITE_SHD 0x58,
//               EVENT_WRITE_EXT 0x5A, EVENT_WRITE_ZPD 0x5B). Then that packet's words: the initiator, and the
//               address and value it writes.
//   kShader     argument: 0 the vertex shader, 1 the pixel shader. Then the microcode as it is in the title's memory
//               (big-endian: copied as is, as IM_LOAD_IMMEDIATE carries it).
//   kInvalidate argument: 0. Then INVALIDATE_STATE's word.
//   kWait       argument: 0. Then WAIT_REG_MEM's five words (function, address or register, reference, mask,
//               interval).
//   kShaderLoad argument: 0. Then IM_LOAD's two words: the microcode's address with the shader type in its low 2 bits
//               (0 vertex, 1 pixel), and its size in dwords.
//   kConstantLoad argument: 0. Then LOAD_ALU_CONSTANT's three words: the values' address, the offset and type of the
//               first constant (as SET_CONSTANT's), the dwords.
//   kIndirect   argument: 0. Then INDIRECT_BUFFER's two words: the buffer's address and its size in dwords (a recorded
//               command buffer, PM4 or records, run where the record is).
//
// A record is predicated as PM4 is: bit 0 of its header (the runtime predicates its draws, which run only for the
// tiles that the bin select and mask both have).
//
// The writers (any360/xdk/d3d_state.h and the host's D3D natives) and the command processor (layer/gfx) share this
// header only; it has no dependency.
#pragma once

#include <cstdint>

namespace any360::xdk::record {

constexpr uint32_t kOpcode = 0x7E;
enum Kind : uint32_t {
  kRegisters = 1,
  kDraw = 2,
  kBins = 3,
  kEvent = 4,
  kShader = 5,
  kInvalidate = 6,
  kWait = 7,
  kShaderLoad = 8,
  kConstantLoad = 9,
  kIndirect = 10,
};

// The packet's header for `words` words after it (the kind word included), `predicated` or not.
constexpr uint32_t Header(uint32_t words, bool predicated = false) {
  return (3u << 30) | ((words - 1) << 16) | (kOpcode << 8) | (predicated ? 1u : 0u);
}
constexpr uint32_t KindWord(Kind kind, uint32_t argument) { return uint32_t(kind) | (argument << 8); }

// The words after the header of a register record of `count` registers, and of a draw record (the kind word included).
constexpr uint32_t RegistersWords(uint32_t count) { return 1 + count; }
constexpr uint32_t DrawWords(bool dma) { return dma ? 4 : 2; }
constexpr uint32_t kBinsWords = 2;
constexpr uint32_t EventWords(uint32_t packet_words) { return 1 + packet_words; }
constexpr uint32_t kShaderLoadWords = 3;
constexpr uint32_t kConstantLoadWords = 4;
constexpr uint32_t kIndirectWords = 3;

}  // namespace any360::xdk::record
