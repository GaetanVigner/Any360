// The compute shader that resolves a k_16_16_16_16 colour target into a k_16_16_16_16 texture on the GPU, as the CPU
// path does texel by texel (VulkanRenderer::Copy, `fixed16`): the target is held at a 32nd of its -32...32 range
// (R16G16B16A16_SNORM); a destination texel is the average of its pixel's samples (summed as 16-bit integers, rounded
// half away from zero), times 32 and 2^bias, clamped to 0...1, written as 16-bit unorm (rounded half away from zero).
// Every product is NoContraction, in the CPU path's order, so the words are the same. Built as SPIR-V with the
// vendored glslang builder like the depth copy. In GLSL terms:
//
//   layout(local_size_x = 8, local_size_y = 8) in;
//   layout(set = 0, binding = 0) uniform texture2D source;           // the target, its samples as texels
//   layout(set = 0, binding = 1, rgba16) writeonly uniform image2D dest;
//   layout(push_constant) uniform Region {
//     ivec2 source; ivec2 dest; uvec2 size;  // host pixels: the first guest pixel's, the destination's, the extent
//     ivec2 pixel;                          // host pixels a guest pixel (samples x scale)
//     int scale; int samples; float bias;   // host pixels a sample, samples averaged, 2^bias
//   };
//   void main() {
//     uvec2 p = gl_GlobalInvocationID.xy;
//     if (p.x < size.x && p.y < size.y) {
//       ivec2 at = source + ivec2(p / uint(scale)) * pixel + ivec2(p % uint(scale));
//       ivec4 sum = ivec4(0);
//       for (int k = 0; k < samples; ++k)   // sample k: column k >> 1, row k & 1
//         sum += ivec4(floor(texelFetch(source, at + ivec2(k >> 1, k & 1) * scale, 0) * 32767.0 + 0.5));
//       vec4 average = vec4(sum) / float(samples);
//       vec4 word = sign(average) * floor(abs(average) + 0.5);
//       vec4 value = clamp(word * (32.0 / 32767.0) * bias, 0.0, 1.0);
//       imageStore(dest, dest + ivec2(p), floor(value * 65535.0 + 0.5) / 65535.0);
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/GLSL.std.450.h"
#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildFixed16ResolveShader() {
  spv::SpvBuildLogger logger;
  spv::Builder b(0x10000, 0, &logger);  // SPIR-V 1.0
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.addCapability(spv::CapabilityShader);
  b.addCapability(spv::CapabilityStorageImageExtendedFormats);  // rgba16
  const spv::Id glsl = b.import("GLSL.std.450");

  const spv::Id t_bool = b.makeBoolType();
  const spv::Id t_uint = b.makeUintType(32), t_int = b.makeIntType(32), t_float = b.makeFloatType(32);
  const spv::Id t_uvec2 = b.makeVectorType(t_uint, 2), t_ivec2 = b.makeVectorType(t_int, 2);
  const spv::Id t_uvec3 = b.makeVectorType(t_uint, 3), t_vec4 = b.makeVectorType(t_float, 4);
  const spv::Id t_ivec4 = b.makeVectorType(t_int, 4);

  // Interface.
  const spv::Id global_id = b.createVariable(spv::NoPrecision, spv::StorageClassInput, t_uvec3, "gl_GlobalInvocationID");
  b.addDecoration(global_id, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId);
  const spv::Id t_source = b.makeImageType(t_float, spv::Dim2D, false, false, false, 1, spv::ImageFormatUnknown);
  const spv::Id source = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_source, "source");
  b.addDecoration(source, spv::DecorationDescriptorSet, 0);
  b.addDecoration(source, spv::DecorationBinding, 0);
  const spv::Id t_dest = b.makeImageType(t_float, spv::Dim2D, false, false, false, 2, spv::ImageFormatRgba16);
  const spv::Id dest = b.createVariable(spv::NoPrecision, spv::StorageClassUniformConstant, t_dest, "dest");
  b.addDecoration(dest, spv::DecorationDescriptorSet, 0);
  b.addDecoration(dest, spv::DecorationBinding, 1);
  b.addDecoration(dest, spv::DecorationNonReadable);
  const spv::Id t_region = b.makeStructType({t_ivec2, t_ivec2, t_uvec2, t_ivec2, t_int, t_int, t_float}, "Region");
  const int offsets[7] = {0, 8, 16, 24, 32, 36, 40};
  for (unsigned i = 0; i < 7; ++i) b.addMemberDecoration(t_region, i, spv::DecorationOffset, offsets[i]);
  b.addDecoration(t_region, spv::DecorationBlock);
  const spv::Id region = b.createVariable(spv::NoPrecision, spv::StorageClassPushConstant, t_region, "region");

  spv::Function* main = b.makeEntryPoint("main");
  spv::Instruction* entry = b.addEntryPoint(spv::ExecutionModelGLCompute, main, "main");
  entry->addIdOperand(global_id);
  b.addExecutionMode(main, spv::ExecutionModeLocalSize, 8, 8, 1);

  auto member = [&](int index) {
    return b.createLoad(b.createAccessChain(spv::StorageClassPushConstant, region, {b.makeIntConstant(index)}), spv::NoPrecision);
  };
  // Products and sums kept as written (no fused multiply-add the CPU path does not do).
  auto exact = [&](spv::Op op, spv::Id type, spv::Id a, spv::Id c) {
    const spv::Id r = b.createBinOp(op, type, a, c);
    b.addDecoration(r, spv::DecorationNoContraction);
    return r;
  };
  auto splat = [&](float v) {
    const spv::Id c = b.makeFloatConstant(v);
    return b.makeCompositeConstant(t_vec4, {c, c, c, c});
  };
  auto call = [&](int op, std::vector<spv::Id> args) { return b.createBuiltinCall(t_vec4, glsl, op, args); };

  const spv::Id sum = b.createVariable(spv::NoPrecision, spv::StorageClassFunction, t_ivec4, "sum");
  const spv::Id gid = b.createLoad(global_id, spv::NoPrecision);
  const spv::Id p = b.createOp(spv::OpVectorShuffle, t_uvec2, std::vector<spv::IdImmediate>{{true, gid}, {true, gid}, {false, 0}, {false, 1}});
  const spv::Id size = member(2);
  const spv::Id inside = b.createBinOp(
      spv::OpLogicalAnd, t_bool,
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 0), b.createCompositeExtract(size, t_uint, 0)),
      b.createBinOp(spv::OpULessThan, t_bool, b.createCompositeExtract(p, t_uint, 1), b.createCompositeExtract(size, t_uint, 1)));
  spv::Builder::If if_inside(inside, spv::SelectionControlMaskNone, b);
  {
    const spv::Id scale = member(4), samples = member(5);
    const spv::Id scale_u = b.createUnaryOp(spv::OpBitcast, t_uint, scale);
    const spv::Id scale_uvec = b.createCompositeConstruct(t_uvec2, {scale_u, scale_u});
    const spv::Id pixel = b.createUnaryOp(spv::OpBitcast, t_ivec2, b.createBinOp(spv::OpUDiv, t_uvec2, p, scale_uvec));
    const spv::Id within = b.createUnaryOp(spv::OpBitcast, t_ivec2, b.createBinOp(spv::OpUMod, t_uvec2, p, scale_uvec));
    const spv::Id at = b.createBinOp(spv::OpIAdd, t_ivec2, b.createBinOp(spv::OpIAdd, t_ivec2, member(0),
                                                                         b.createBinOp(spv::OpIMul, t_ivec2, pixel, member(3))),
                                     within);
    const spv::Id zero = b.makeIntConstant(0);
    b.createStore(b.makeCompositeConstant(t_ivec4, {zero, zero, zero, zero}), sum);
    for (int k = 0; k < 4; ++k) {  // at most four samples a pixel
      spv::Builder::If if_sample(b.createBinOp(spv::OpSLessThan, t_bool, b.makeIntConstant(k), samples),
                                 spv::SelectionControlMaskNone, b);
      const spv::Id offset = b.createCompositeConstruct(
          t_ivec2, {b.createBinOp(spv::OpIMul, t_int, b.makeIntConstant(k >> 1), scale),
                    b.createBinOp(spv::OpIMul, t_int, b.makeIntConstant(k & 1), scale)});
      const spv::Id texel = b.createOp(
          spv::OpImageFetch, t_vec4,
          std::vector<spv::IdImmediate>{{true, b.createLoad(source, spv::NoPrecision)},
                                        {true, b.createBinOp(spv::OpIAdd, t_ivec2, at, offset)},
                                        {false, spv::ImageOperandsLodMask},
                                        {true, zero}});
      const spv::Id word = b.createUnaryOp(
          spv::OpConvertFToS, t_ivec4,
          call(GLSLstd450Floor, {exact(spv::OpFAdd, t_vec4, exact(spv::OpFMul, t_vec4, texel, splat(32767.0f)), splat(0.5f))}));
      b.createStore(b.createBinOp(spv::OpIAdd, t_ivec4, b.createLoad(sum, spv::NoPrecision), word), sum);
      if_sample.makeEndIf();
    }
    const spv::Id count = b.createUnaryOp(spv::OpConvertSToF, t_float, samples);
    const spv::Id average = exact(spv::OpFDiv, t_vec4, b.createUnaryOp(spv::OpConvertSToF, t_vec4, b.createLoad(sum, spv::NoPrecision)),
                                  b.createCompositeConstruct(t_vec4, {count, count, count, count}));
    const spv::Id rounded = exact(spv::OpFMul, t_vec4, call(GLSLstd450FSign, {average}),
                                  call(GLSLstd450Floor, {exact(spv::OpFAdd, t_vec4, call(GLSLstd450FAbs, {average}), splat(0.5f))}));
    const spv::Id bias = member(6);
    const spv::Id value = call(GLSLstd450FClamp,
                               {exact(spv::OpFMul, t_vec4, exact(spv::OpFMul, t_vec4, rounded, splat(32.0f / 32767.0f)),
                                      b.createCompositeConstruct(t_vec4, {bias, bias, bias, bias})),
                                splat(0.0f), splat(1.0f)});
    const spv::Id unorm = exact(spv::OpFDiv, t_vec4,
                                call(GLSLstd450Floor, {exact(spv::OpFAdd, t_vec4, exact(spv::OpFMul, t_vec4, value, splat(65535.0f)),
                                                             splat(0.5f))}),
                                splat(65535.0f));
    const spv::Id to = b.createBinOp(spv::OpIAdd, t_ivec2, member(1), b.createUnaryOp(spv::OpBitcast, t_ivec2, p));
    b.createNoResultOp(spv::OpImageWrite, {b.createLoad(dest, spv::NoPrecision), to, unorm});
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
