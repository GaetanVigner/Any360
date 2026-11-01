// Hand encoding of Xenos shader microcode for tests (no game data): control flow, vector ALU and vertex fetch
// instructions packed from the bit layouts of ucode.h's structures (which only decode).
#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/format/ucode.h>

namespace any360::gfx::test {

namespace uc = rex::graphics::ucode;

// One control flow instruction: 32 bits in dword_0, 16 bits in dword_1 (ucode::ControlFlowInstruction).
struct Cf {
  uint32_t dword_0 = 0;
  uint32_t dword_1 = 0;
};

// `sequence`: 2 bits per instruction, bit 0 fetch (1) or ALU (0), bit 1 serialize.
inline Cf CfExec(uc::ControlFlowOpcode opcode, uint32_t address, uint32_t count, uint32_t sequence) {
  Cf cf;
  cf.dword_0 = (address & 0xFFF) | ((count & 0x7) << 12) | ((sequence & 0xFFF) << 16);
  cf.dword_1 = (1u << 9) /* is_predicate_clean */ | (uint32_t(uc::AddressingMode::kRelative) << 11) |
               (uint32_t(opcode) << 12);
  return cf;
}

inline Cf CfAlloc(uc::AllocType type, uint32_t size) {
  Cf cf;
  cf.dword_0 = size & 0x7;
  cf.dword_1 = (uint32_t(type) << 9) | (uint32_t(uc::ControlFlowOpcode::kAlloc) << 12);
  return cf;
}

// Packs a pair of control flow instructions into 3 dwords (inverse of ucode::UnpackControlFlowInstructions).
inline void PushCfPair(std::vector<uint32_t>& words, const Cf& a, const Cf& b) {
  words.push_back(a.dword_0);
  words.push_back((a.dword_1 & 0xFFFF) | (b.dword_0 << 16));
  words.push_back((b.dword_0 >> 16) | (b.dword_1 << 16));
}

struct AluOperand {
  uint32_t reg = 0;
  bool is_temp = false;  // false: float constant c[reg]
};

// An exporting (or temp-writing) vector ALU operation with the scalar unit idle (kRetainPrev, empty write mask).
// Swizzles are identity, no negation, no predication, no relative addressing.
inline void PushAluVector(std::vector<uint32_t>& words, uc::AluVectorOpcode opcode, uint32_t dest, bool is_export,
                          uint32_t write_mask, AluOperand src1, AluOperand src2) {
  const uint32_t w0 = (dest & 0x3F) | ((is_export ? 1u : 0u) << 15) | ((write_mask & 0xF) << 16) |
                      (uint32_t(uc::AluScalarOpcode::kRetainPrev) << 26);
  const uint32_t w2 = (src2.reg & 0xFF) << 8 | (src1.reg & 0xFF) << 16 | (uint32_t(opcode) & 0x1F) << 24 |
                      (src2.is_temp ? 1u : 0u) << 30 | (src1.is_temp ? 1u : 0u) << 31;
  words.push_back(w0);
  words.push_back(0);
  words.push_back(w2);
}

// vfetch_full: dest.xyzw = vertex fetch constant `fetch_constant` (0-95) at index src.x (floored), `format`, a
// stride and offset in dwords (ucode::VertexFetchInstruction).
inline void PushVertexFetch(std::vector<uint32_t>& words, uint32_t dest, uint32_t src, uint32_t fetch_constant,
                            rex::graphics::xenos::VertexFormat format, uint32_t stride_dwords, int32_t offset_dwords) {
  const uint32_t w0 = uint32_t(uc::FetchOpcode::kVertexFetch) | (src & 0x3F) << 5 | (dest & 0x3F) << 12 | 1u << 19 |
                      (fetch_constant / 3) << 20 | (fetch_constant % 3) << 25;
  const uint32_t dest_swizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;  // xyzw
  const uint32_t w1 = dest_swizzle | (uint32_t(format) & 0x3F) << 16;
  const uint32_t w2 = (stride_dwords & 0xFF) | (uint32_t(offset_dwords) & 0x7FFFFF) << 8;
  words.push_back(w0);
  words.push_back(w1);
  words.push_back(w2);
}

// alloc <type>; exece (one ALU instruction at slot 1); export = max(src, src).
inline std::vector<uint32_t> BuildSingleExportShader(uc::AllocType alloc, uint32_t export_reg, AluOperand src) {
  std::vector<uint32_t> words;
  PushCfPair(words, CfAlloc(alloc, 0), CfExec(uc::ControlFlowOpcode::kExecEnd, 1, 1, 0));
  PushAluVector(words, uc::AluVectorOpcode::kMax, export_reg, true, 0b1111, src, src);
  return words;
}

// A pixel shader writing two colour targets: oC0 = c0, oC1 = c1.
inline std::vector<uint32_t> BuildTwoTargetPixelShader() {
  std::vector<uint32_t> words;
  PushCfPair(words, CfAlloc(uc::AllocType::kPsColors, 0), CfExec(uc::ControlFlowOpcode::kExecEnd, 1, 2, 0));
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kPSColor0), true, 0b1111, {0, false},
                {0, false});
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kPSColor1), true, 0b1111, {1, false},
                {1, false});
  return words;
}

// A vertex shader: r1 = vfetch(fetch constant 0, r0.x = vertex index, float4, stride 4); oPos = r1.
inline std::vector<uint32_t> BuildFetchPositionShader() {
  std::vector<uint32_t> words;
  PushCfPair(words, CfAlloc(uc::AllocType::kVsPosition, 0), CfExec(uc::ControlFlowOpcode::kExecEnd, 1, 2, 0b0001));
  PushVertexFetch(words, 1, 0, 0, rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT, 4, 0);
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kVSPosition), true, 0b1111, {1, true},
                {1, true});
  return words;
}

}  // namespace any360::gfx::test
