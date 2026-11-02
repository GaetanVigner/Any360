// The compute shader that puts a presented frame through the display gamma ramp, built as SPIR-V with the
// vendored glslang builder (no shader compiler at build or run time). In GLSL terms:
//
//   layout(local_size_x = 8, local_size_y = 8) in;
//   layout(set = 0, binding = 0) uniform texture2D source;           // the frame's surface
//   layout(set = 0, binding = 1, rgba8) writeonly uniform image2D dest;
//   layout(set = 0, binding = 2) buffer Lut { uint lut[]; };        // 3 x 256 8-bit outputs: red, green, blue
//   layout(push_constant) uniform Region { ivec2 offset; uvec2 size; };
//   void main() {
//     uvec2 p = gl_GlobalInvocationID.xy;
//     if (p.x < size.x && p.y < size.y) {
//       vec4 c = texelFetch(source, ivec2(p) + offset, 0);
//       uvec3 i = uvec3(c.rgb * 255.0 + 0.5);
//       imageStore(dest, ivec2(p), vec4(lut[i.r], lut[256 + i.g], lut[512 + i.b], 255.0) / 255.0);
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildGammaShader() {
  spv::SpvBuildLogger logger;
  spv::Builder b(0x10000, 0, &logger);  // SPIR-V 1.0
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.addCapability(spv::CapabilityShader);

  const spv::Id t_bool = b.makeBoolType();
  const spv::Id t_uint = b.makeUintType(32), t_int = b.makeIntType(32), t_float = b.makeFloatType(32);
  const spv::Id t_uvec2 = b.makeVectorType(t_uint, 2), t_ivec2 = b.makeVectorType(t_int, 2);
  const spv::Id t_uvec3 = b.makeVectorType(t_uint, 3), t_vec4 = b.makeVectorType(t_float, 4);

  // Interface.
  const spv::Id global_id = b.createVariable(spv::NoPrecision, spv::StorageClassInput, t_uvec3, "gl_GlobalInvocationID");
  b.addDecoration(global_id, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
  const spv::Id t_source = b.makeImageType(t_float, spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
  const spv::Id source = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_source, "source");
  b.addDecoration(source, spv::DecorationDescriptorSet, 0);
  b.addDecoration(source, spv::DecorationBinding, 0);
  const spv::Id t_dest = b.makeImageType(t_float, spv::Dim2D, false, false, false, 2, spv::ImageFormatRgba8);
  const spv::Id dest = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_dest, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 0);
  b.addDecoration(dest, spv::DecorationBinding, 1);
  b.addDecoration(dest, spv::DecorationNonReadable);
  const spv::Id t_lut_array = b.makeRuntimeArray(t_uint);
  b.addDecoration(t_lut_array, spv::DecorationArrayStride, 4);
  const spv::Id t_lut = b.makeStructType({t_lut_array}, "Lut");
  b.addMemberDecoration(t_lut, 0, spv::DecorationOffset, 0);
  b.addDecoration(t_lut, spv::DecorationBufferBlock);
  const spv::Id lut = b.createVariable(spv::NoPrecision, spv::StorageClassUniform, t_lut, "lut");
  b.addDecoration(lut, spv::DecorationDescriptorSet, 0);
  b.addDecoration(lut, spv::DecorationBinding, 2);
  const spv::Id t_region = b.makeStructType({t_ivec2, t_uvec2}, "Region");
  b.addMemberDecoration(t_region, 0, spv::DecorationOffset, 0);
  b.addMemberDecoration(t_region, 1, spv::DecorationOffset, 8);
  b.addDecoration(t_region, spv::DecorationBlock);
  const spv::Id region = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, t_region, "region");

  spv::Function* main = b.makeEntryPoint("main");
  spv::Instruction* entry = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  entry->addIdOperand(global_id);
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);

  const spv::Id int0 = b.makeIntConstant(0), int1 = b.makeIntConstant(1);
  const spv::Id gid = b.createLoad(global_id, spv::NoPrecision);
  const spv::Id p = b.createOp(spv::OpVectorShuffle, t_uvec2, std::vector<spv::IdImmediate>{{true, gid}, {true, gid}, {false, 0}, {false, 1}});
  const spv::Id size = b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, region, {int1}), spv::NoPrecision);
  const spv::Id inside = b.createBinOp(
      spv::OpLogicalAnd, t_bool,
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 0), b.createCompositeExtract(size, t_uint, 0)),
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 1), b.createCompositeExtract(size, t_uint, 1)));
  spv::Builder::If if_inside(inside, spv::SelectionControlMaskNone, b);
  {
    const spv::Id offset = b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, region, {int0}), spv::NoPrecision);
    const spv::Id p_int = b.createUnaryOp(spv::OpBitcast, t_ivec2, p);
    const spv::Id coord = b.createBinOp(spv::OpIAdd, t_ivec2, p_int, offset);
    const spv::Id texel = b.createOp(spv::OpImageFetch, t_vec4,
                                     std::vector<spv::IdImmediate>{{true, b.createLoad(source, spv::NoPrecision)},
                                                                   {true, coord},
                                                                   {false, spv::ImageOperandsLodMask},
                                                                   {true, int0}});
    spv::Id out[4];
    const spv::Id f255 = b.makeFloatConstant(255.0f), half = b.makeFloatConstant(0.5f);
    for (int c = 0; c < 3; ++c) {
      const spv::Id value = b.createCompositeExtract(texel, t_float, unsigned(c));
      const spv::Id index = b.createUnaryOp(
          spv::OpConvertFToU, t_uint, b.createBinOp(spv::OpFAdd, t_float, b.createBinOp(spv::OpFMul, t_float, value, f255), half));
      const spv::Id slot = b.createBinOp(spv::OpIAdd, t_uint, index, b.makeUintConstant(256u * unsigned(c)));
      const spv::Id mapped = b.createLoad(b.createAccessChain(spv::StorageClassUniform, lut, {int0, slot}), spv::NoPrecision);
      out[c] = b.createBinOp(spv::OpFDiv, t_float, b.createUnaryOp(spv::OpConvertUToF, t_float, mapped), f255);
    }
    out[3] = b.makeFloatConstant(1.0f);
    const spv::Id color = b.createCompositeConstruct(t_vec4, {out[0], out[1], out[2], out[3]});
    b.createNoResultOp(spv::OpImageWrite, {b.createLoad(dest, spv::NoPrecision), p_int, color});
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
