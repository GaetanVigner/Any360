// The compute shader that applies a resolve's exponent bias on the GPU: every component of the texels over a
// rectangle of an RGBA16F resolve image multiplied by 2^bias, in place, as the CPU path does texel by texel
// (RB_COPY_DEST_INFO copy_dest_exp_bias). Built as SPIR-V with the vendored glslang builder like the depth copy.
// In GLSL terms:
//
//   layout(local_size_x = 8, local_size_y = 8) in;
//   layout(set = 0, binding = 0, rgba16f) uniform image2D image;
//   layout(push_constant) uniform Region { ivec2 origin; uvec2 size; float scale; };
//   void main() {
//     uvec2 p = gl_GlobalInvocationID.xy;
//     if (p.x < size.x && p.y < size.y) {
//       ivec2 at = origin + ivec2(p);
//       imageStore(image, at, imageLoad(image, at) * scale);
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildScaleShader() {
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
  const spv::Id t_image = b.makeImageType(t_float, spv::Dim2D, false, false, false, 2, spv::ImageFormatRgba16f);
  const spv::Id image = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_image, "image");
  b.addDecoration(image, spv::DecorationDescriptorSet, 0);
  b.addDecoration(image, spv::DecorationBinding, 0);
  const spv::Id t_region = b.makeStructType({t_ivec2, t_uvec2, t_float}, "Region");
  b.addMemberDecoration(t_region, 0, spv::DecorationOffset, 0);
  b.addMemberDecoration(t_region, 1, spv::DecorationOffset, 8);
  b.addMemberDecoration(t_region, 2, spv::DecorationOffset, 16);
  b.addDecoration(t_region, spv::DecorationBlock);
  const spv::Id region = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, t_region, "region");

  spv::Function* main = b.makeEntryPoint("main");
  spv::Instruction* entry = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  entry->addIdOperand(global_id);
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);

  auto member = [&](int index) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, region, {b.makeIntConstant(index)}), spv::NoPrecision);
  };
  const spv::Id gid = b.createLoad(global_id, spv::NoPrecision);
  const spv::Id p = b.createOp(spv::OpVectorShuffle, t_uvec2, std::vector<spv::IdImmediate>{{true, gid}, {true, gid}, {false, 0}, {false, 1}});
  const spv::Id size = member(1);
  const spv::Id inside = b.createBinOp(
      spv::OpLogicalAnd, t_bool,
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 0), b.createCompositeExtract(size, t_uint, 0)),
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 1), b.createCompositeExtract(size, t_uint, 1)));
  spv::Builder::If if_inside(inside, spv::SelectionControlMaskNone, b);
  {
    const spv::Id at = b.createBinOp(spv::OpIAdd, t_ivec2, member(0), b.createUnaryOp(spv::OpBitcast, t_ivec2, p));
    const spv::Id loaded = b.createLoad(image, spv::NoPrecision);
    const spv::Id texel = b.createOp(spv::OpImageRead, t_vec4, std::vector<spv::IdImmediate>{{true, loaded}, {true, at}});
    const spv::Id scaled = b.createBinOp(spv::OpVectorTimesScalar, t_vec4, texel, member(2));
    b.createNoResultOp(spv::OpImageWrite, {b.createLoad(image, spv::NoPrecision), at, scaled});
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
