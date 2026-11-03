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

// Any ALU instruction: a vector and a scalar operation co-issued, identity swizzles, no negation or predication.
// Exporting, the vector operation writes `vector_mask` and the scalar `scalar_mask` of export `dest` (both: 1.0);
// else the vector one writes temp `dest` and the scalar one temp `scalar_dest`. The scalar operation reads src3
// (one operand: a = W; two: a = W, b = X).
struct Alu {
  uc::AluVectorOpcode vector = uc::AluVectorOpcode::kMax;
  uint32_t vector_mask = 0;
  uc::AluScalarOpcode scalar = uc::AluScalarOpcode::kRetainPrev;
  uint32_t scalar_mask = 0;
  uint32_t dest = 0, scalar_dest = 0;
  bool is_export = false;
  AluOperand src1{}, src2{}, src3{};
  uint32_t src3_swizzle = 0;  // raw (constant-scalar operations keep a temp register's bits there)
};
inline void PushAlu(std::vector<uint32_t>& words, const Alu& a) {
  words.push_back((a.dest & 0x3F) | (a.scalar_dest & 0x3F) << 8 | (a.is_export ? 1u : 0u) << 15 |
                  (a.vector_mask & 0xF) << 16 | (a.scalar_mask & 0xF) << 20 | (uint32_t(a.scalar) & 0x3F) << 26);
  words.push_back(a.src3_swizzle & 0xFF);
  words.push_back((a.src3.reg & 0xFF) | (a.src2.reg & 0xFF) << 8 | (a.src1.reg & 0xFF) << 16 |
                  (uint32_t(a.vector) & 0x1F) << 24 | (a.src3.is_temp ? 1u : 0u) << 29 |
                  (a.src2.is_temp ? 1u : 0u) << 30 | (a.src1.is_temp ? 1u : 0u) << 31);
}

// A pixel shader of `alus` ALU instructions in one exec (at most 6), ending the shader.
inline std::vector<uint32_t> BuildAluPixelShader(const std::vector<Alu>& alus) {
  std::vector<uint32_t> words;
  PushCfPair(words, CfAlloc(uc::AllocType::kPsColors, 0),
             CfExec(uc::ControlFlowOpcode::kExecEnd, 1, uint32_t(alus.size()), 0));
  for (const Alu& a : alus) PushAlu(words, a);
  return words;
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

// tfetch2D dest.xyzw = texture fetch constant `fetch_constant` (0-31) at src.xy, filters from the fetch constant
// (ucode::TextureFetchInstruction).
inline void PushTextureFetch2D(std::vector<uint32_t>& words, uint32_t dest, uint32_t src, uint32_t fetch_constant) {
  const uint32_t src_swizzle = 0 | 1 << 2 | 2 << 4;  // x, y, z
  const uint32_t w0 = uint32_t(uc::FetchOpcode::kTextureFetch) | (src & 0x3F) << 5 | (dest & 0x3F) << 12 |
                      (fetch_constant & 0x1F) << 20 | src_swizzle << 26;
  const uint32_t dest_swizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;
  // mag, min, mip: use the fetch constant's (3); anisotropy: the fetch constant's (7); volume filters likewise; the
  // level of detail computed from the derivatives (use_comp_lod), as a D3D sample instruction does.
  const uint32_t w1 = dest_swizzle | 3u << 12 | 3u << 14 | 3u << 16 | 7u << 18 | 3u << 24 | 3u << 26 | 1u << 28;
  // Sample at pixel centres, 2D.
  const uint32_t w2 = 1u << 1 | uint32_t(rex::graphics::xenos::FetchOpDimension::k2D) << 14;
  words.push_back(w0);
  words.push_back(w1);
  words.push_back(w2);
}

// A vertex shader for textured draws: vertices of 6 floats (position xyzw, texcoord uv) from vertex fetch
// constant 0; oPos = position, interpolator 0 = texcoord.
inline std::vector<uint32_t> BuildTexturedVertexShader() {
  std::vector<uint32_t> words;
  // Two control flow pairs (slots 0-1): alloc position, exec 3 at slot 2; alloc interpolators, exece 1 at slot 5.
  PushCfPair(words, CfAlloc(uc::AllocType::kVsPosition, 0), CfExec(uc::ControlFlowOpcode::kExec, 2, 3, 0b000101));
  PushCfPair(words, CfAlloc(uc::AllocType::kVsInterpolators, 0), CfExec(uc::ControlFlowOpcode::kExecEnd, 5, 1, 0));
  PushVertexFetch(words, 1, 0, 0, rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT, 6, 0);
  PushVertexFetch(words, 2, 0, 0, rex::graphics::xenos::VertexFormat::k_32_32_FLOAT, 6, 4);
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kVSPosition), true, 0b1111, {1, true},
                {1, true});
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kVSInterpolator0), true, 0b1111,
                {2, true}, {2, true});
  return words;
}

// A pixel shader sampling texture fetch constant `fetch_constant` at interpolator 0 (r0): oC0 = tfetch2D(r0.xy).
inline std::vector<uint32_t> BuildTexturedPixelShader(uint32_t fetch_constant) {
  std::vector<uint32_t> words;
  PushCfPair(words, CfAlloc(uc::AllocType::kPsColors, 0), CfExec(uc::ControlFlowOpcode::kExecEnd, 1, 2, 0b0001));
  PushTextureFetch2D(words, 1, 0, fetch_constant);
  PushAluVector(words, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kPSColor0), true, 0b1111, {1, true},
                {1, true});
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
