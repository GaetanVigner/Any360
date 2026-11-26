// The compute shader that resolves a guest 2x MSAA colour surface drawn as views (one layer a sample) straight from its
// layers: each host texel of the destination the two samples' texels at the same place averaged, times the resolve's
// exponent bias (2^bias, 1 without), as the console's resolve averages a pixel's samples. Without it the layers were
// copied into the rows image first (a transpose of every texel) and the rows blitted, then scaled: three passes over
// the data instead of one. Built as SPIR-V with the vendored glslang builder like the depth copy. In GLSL terms:
//
//   layout(local_size_x = 8, local_size_y = 8) in;
//   layout(set = 0, binding = 0) uniform texture2DArray layers;   // sample 0, then sample 1
//   layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D dest;
//   layout(push_constant) uniform Region { ivec2 source; ivec2 dest_origin; uvec2 size; float scale; };
//   void main() {
//     uvec2 p = gl_GlobalInvocationID.xy;
//     if (p.x < size.x && p.y < size.y) {
//       ivec2 at = source + ivec2(p);
//       vec4 sum = texelFetch(layers, ivec3(at, 0), 0) + texelFetch(layers, ivec3(at, 1), 0);
//       imageStore(dest, dest_origin + ivec2(p), sum * scale);   // scale = 0.5 * 2^bias
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildViewsResolveShader() {
  spv::SpvBuildLogger logger;
  spv::Builder b(0x10000, 0, &logger);  // SPIR-V 1.0
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.addCapability(spv::CapabilityShader);

  const spv::Id t_bool = b.makeBoolType();
  const spv::Id t_uint = b.makeUintType(32), t_int = b.makeIntType(32), t_float = b.makeFloatType(32);
  const spv::Id t_uvec2 = b.makeVectorType(t_uint, 2), t_ivec2 = b.makeVectorType(t_int, 2);
  const spv::Id t_ivec3 = b.makeVectorType(t_int, 3);
  const spv::Id t_uvec3 = b.makeVectorType(t_uint, 3), t_vec4 = b.makeVectorType(t_float, 4);

  // Interface.
  const spv::Id global_id = b.createVariable(spv::NoPrecision, spv::StorageClassInput, t_uvec3, "gl_GlobalInvocationID");
  b.addDecoration(global_id, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
  const spv::Id t_layers = b.makeImageType(t_float, spv::Dim2D, false, true, false, 1, spv::ImageFormatUnknown);
  const spv::Id layers = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_layers, "layers");
  b.addDecoration(layers, spv::DecorationDescriptorSet, 0);
  b.addDecoration(layers, spv::DecorationBinding, 0);
  const spv::Id t_dest = b.makeImageType(t_float, spv::Dim2D, false, false, false, 2, spv::ImageFormatRgba16f);
  const spv::Id dest = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_dest, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 0);
  b.addDecoration(dest, spv::DecorationBinding, 1);
  b.addDecoration(dest, spv::DecorationNonReadable);
  const spv::Id t_region = b.makeStructType({t_ivec2, t_ivec2, t_uvec2, t_float}, "Region");
  for (unsigned i = 0; i < 4; ++i) b.addMemberDecoration(t_region, i, spv::DecorationOffset, int(8 * i));
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
  const spv::Id size = member(2);
  const spv::Id inside = b.createBinOp(
      spv::OpLogicalAnd, t_bool,
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 0), b.createCompositeExtract(size, t_uint, 0)),
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 1), b.createCompositeExtract(size, t_uint, 1)));
  spv::Builder::If if_inside(inside, spv::SelectionControlMaskNone, b);
  {
    const spv::Id p_int = b.createUnaryOp(spv::OpBitcast, t_ivec2, p);
    const spv::Id at = b.createBinOp(spv::OpIAdd, t_ivec2, member(0), p_int);
    const spv::Id x = b.createCompositeExtract(at, t_int, 0), y = b.createCompositeExtract(at, t_int, 1);
    auto fetch = [&](int layer) {
      const spv::Id coord = b.createCompositeConstruct(t_ivec3, {x, y, b.makeIntConstant(layer)});
      return b.createOp(spv::OpImageFetch, t_vec4,
                        std::vector<spv::IdImmediate>{{true, b.createLoad(layers, spv::NoPrecision)},
                                                      {true, coord},
                                                      {false, spv::ImageOperandsLodMask},
                                                      {true, b.makeIntConstant(0)}});
    };
    const spv::Id sum = b.createBinOp(spv::OpFAdd, t_vec4, fetch(0), fetch(1));
    const spv::Id value = b.createBinOp(spv::OpVectorTimesScalar, t_vec4, sum, member(3));
    const spv::Id to = b.createBinOp(spv::OpIAdd, t_ivec2, member(1), p_int);
    b.createNoResultOp(spv::OpImageWrite, {b.createLoad(dest, spv::NoPrecision), to, value});
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
