// The compute shader that turns a depth surface's texels into the EDRAM words a colour surface over the same EDRAM
// reads (SyncDepthIntoColour): depth (24-bit unorm, or float24 for D24FS8) << 8 | stencil, as ResolveDepth composes
// them on the CPU. Its input is a copy of the depth aspect (4 bytes a texel: the host's D24 in the low 24 bits, or a
// float) and of the stencil aspect (a byte a texel), all in one storage buffer. Built as SPIR-V with the vendored
// glslang builder like the depth copy. In GLSL terms:
//
//   layout(local_size_x = 64) in;
//   layout(set = 0, binding = 0) buffer Data { uint data[]; };
//   layout(push_constant) uniform Region { uint count, mode, depth, stencil, words; };  // offsets in words
//   void main() {
//     uint i = gl_GlobalInvocationID.x;
//     if (i < count) {
//       uint raw = data[depth + i], d;
//       if (mode == 0) d = raw & 0xFFFFFF;                                  // D24 unorm as it is
//       else if (mode == 1) d = min(uint(clamp(uintBitsToFloat(raw), 0, 1) * 16777215 + 0.5), 0xFFFFFF);  // to unorm24
//       else if (mode == 2) d = FloatTo20e4(raw);                            // a float to float24 (textures.cpp)
//       else d = FloatTo20e4(floatBitsToUint(float(raw & 0xFFFFFF) / 16777215));  // D24 unorm to float24
//       uint s = (data[stencil + (i >> 2)] >> ((i & 3) * 8)) & 0xFF;
//       data[words + i] = d << 8 | s;
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/GLSL.std.450.h"
#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildDepthWordShader() {
  spv::SpvBuildLogger logger;
  spv::Builder b(0x10000, 0, &logger);  // SPIR-V 1.0
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.addCapability(spv::CapabilityShader);
  const spv::Id glsl = b.import("GLSL.std.450");

  const spv::Id t_bool = b.makeBoolType();
  const spv::Id t_uint = b.makeUintType(32), t_int = b.makeIntType(32), t_float = b.makeFloatType(32);
  const spv::Id t_uvec3 = b.makeVectorType(t_uint, 3);

  // Interface.
  const spv::Id global_id = b.createVariable(spv::NoPrecision, spv::StorageClassInput, t_uvec3, "gl_GlobalInvocationID");
  b.addDecoration(global_id, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
  const spv::Id t_array = b.makeRuntimeArray(t_uint);
  b.addDecoration(t_array, spv::DecorationArrayStride, 4);
  const spv::Id t_data = b.makeStructType({t_array}, "Data");
  b.addMemberDecoration(t_data, 0, spv::DecorationOffset, 0);
  b.addDecoration(t_data, spv::DecorationBufferBlock);
  const spv::Id data = b.createVariable(spv::NoPrecision, spv::StorageClassUniform, t_data, "data");
  b.addDecoration(data, spv::DecorationDescriptorSet, 0);
  b.addDecoration(data, spv::DecorationBinding, 0);
  const spv::Id t_region = b.makeStructType({t_uint, t_uint, t_uint, t_uint, t_uint}, "Region");
  for (unsigned i = 0; i < 5; ++i) b.addMemberDecoration(t_region, i, spv::DecorationOffset, int(4 * i));
  b.addDecoration(t_region, spv::DecorationBlock);
  const spv::Id region = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, t_region, "region");

  spv::Function* main = b.makeEntryPoint("main");
  spv::Instruction* entry = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  entry->addIdOperand(global_id);
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 64, 1, 1);

  const spv::Id int0 = b.makeIntConstant(0);
  auto member = [&](int index) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, region, {b.makeIntConstant(index)}),
                        spv::NoPrecision);
  };
  auto u = [&](uint32_t value) { return b.makeUintConstant(value); };
  auto op = [&](spv::Op o, spv::Id x, spv::Id y) { return b.createBinOp(o, t_uint, x, y); };
  auto select = [&](spv::Id condition, spv::Id x, spv::Id y) { return b.createTriOp(spv::OpSelect, t_uint, condition, x, y); };
  auto load = [&](spv::Id index) {
    return b.createLoad(b.createAccessChain(spv::StorageClassUniform, data, {int0, index}), spv::NoPrecision);
  };

  const spv::Id i = b.createCompositeExtract(b.createLoad(global_id, spv::NoPrecision), t_uint, 0);
  spv::Builder::If if_inside(b.createBinOp(spv::OpULessThan, t_bool, i, member(0)), spv::SelectionControlMaskNone, b);
  {
    const spv::Id mode = member(1);
    const spv::Id raw = load(op(spv::OpIAdd, member(2), i));
    // Mode 0: the host's D24 unorm, in the low 24 bits.
    const spv::Id unorm24 = op(spv::OpBitwiseAnd, raw, u(0xFFFFFF));
    // Mode 1: a float, to 24-bit unorm rounded to nearest.
    const spv::Id value = b.createUnaryOp(spv::OpBitcast, t_float, raw);
    const spv::Id clamped = b.createBuiltinCall(t_float, glsl, GLSLstd450FClamp,
                                                {value, b.makeFloatConstant(0.0f), b.makeFloatConstant(1.0f)});
    // 1.0 * 16777215 + 0.5 rounds to 16777216 in float: clamped back to 24 bits.
    const spv::Id from_float = b.createBuiltinCall(
        t_uint, glsl, GLSLstd450UMin,
        {b.createUnaryOp(spv::OpConvertFToU, t_uint,
                         b.createBinOp(spv::OpFAdd, t_float,
                                       b.createBinOp(spv::OpFMul, t_float, clamped, b.makeFloatConstant(16777215.0f)),
                                       b.makeFloatConstant(0.5f))),
         u(0xFFFFFF)});
    // Mode 2: a float, to float24 (FloatTo20e4: 4 exponent bits, bias 15, 20 mantissa bits, to nearest even); mode 3:
    // the host's D24 unorm as a float first (a D24FS8 surface on a D24 host).
    const spv::Id unorm_as_float = b.createBinOp(spv::OpFDiv, t_float, b.createUnaryOp(spv::OpConvertUToF, t_float, unorm24),
                                                 b.makeFloatConstant(16777215.0f));
    const spv::Id bits = select(b.createBinOp(spv::OpIEqual, t_bool, mode, u(3)),
                                b.createUnaryOp(spv::OpBitcast, t_uint, unorm_as_float), raw);
    const spv::Id f = b.createBuiltinCall(t_uint, glsl, GLSLstd450UMin, {bits, u(0x3FFFFFF8u)});
    const spv::Id shift = b.createBuiltinCall(t_uint, glsl, GLSLstd450UMin,
                                              {op(spv::OpISub, u(113), op(spv::OpShiftRightLogical, f, u(23))), u(24)});
    const spv::Id denormal = op(spv::OpShiftRightLogical, op(spv::OpBitwiseOr, u(0x800000), op(spv::OpBitwiseAnd, f, u(0x7FFFFF))), shift);
    const spv::Id normal = op(spv::OpIAdd, f, u(0xC8000000u));
    const spv::Id biased = select(b.createBinOp(spv::OpULessThan, t_bool, f, u(0x38800000u)), denormal, normal);
    const spv::Id rounded = op(spv::OpIAdd, biased, op(spv::OpIAdd, u(3), op(spv::OpBitwiseAnd, op(spv::OpShiftRightLogical, biased, u(3)), u(1))));
    const spv::Id positive = b.createBinOp(
        spv::OpLogicalAnd, t_bool, b.createBinOp(spv::OpSGreaterThan, t_bool, b.createUnaryOp(spv::OpBitcast, t_int, bits), b.makeIntConstant(0)),
        b.createBinOp(spv::OpULessThanEqual, t_bool, bits, u(0x7F800000u)));
    const spv::Id float24 = select(positive, op(spv::OpBitwiseAnd, op(spv::OpShiftRightLogical, rounded, u(3)), u(0xFFFFFF)), u(0));
    const spv::Id depth = select(b.createBinOp(spv::OpIEqual, t_bool, mode, u(0)), unorm24,
                                 select(b.createBinOp(spv::OpIEqual, t_bool, mode, u(1)), from_float, float24));
    // The stencil byte of texel i, 4 to a word.
    const spv::Id stencil_word = load(op(spv::OpIAdd, member(3), op(spv::OpShiftRightLogical, i, u(2))));
    const spv::Id stencil = op(spv::OpBitwiseAnd,
                               op(spv::OpShiftRightLogical, stencil_word, op(spv::OpShiftLeftLogical, op(spv::OpBitwiseAnd, i, u(3)), u(3))),
                               u(0xFF));
    const spv::Id word = op(spv::OpBitwiseOr, op(spv::OpShiftLeftLogical, depth, u(8)), stencil);
    b.createStore(word, b.createAccessChain(spv::StorageClassUniform, data, {int0, op(spv::OpIAdd, member(4), i)}));
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
