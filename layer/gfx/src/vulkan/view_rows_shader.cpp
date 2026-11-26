// The compute shader that moves a 2x MSAA surface's rows between its two layouts (CopyViews), as words in one
// storage buffer: the rows image's order (a pixel row is 2 x scale host rows: sample 0's scale rows, then sample 1's)
// and the layers' order (layer 0's rows, then layer 1's, each a pixel row of scale host rows). Built as SPIR-V with
// the vendored glslang builder like the depth copies. In GLSL terms:
//
//   layout(local_size_x = 64) in;
//   layout(set = 0, binding = 0) buffer Data { uint data[]; };
//   layout(push_constant) uniform Region { uint count, row_words, rows, scale, rows_order, layers_order, into_layers; };
//   void main() {
//     uint i = gl_GlobalInvocationID.x;
//     if (i < count) {
//       uint row = i / row_words, word = i - row * row_words;             // a row of the rows image's order
//       uint pixel = row / (2 * scale), sample = (row / scale) & 1, sub = row % scale;
//       uint layer_row = sample * (rows / 2) + pixel * scale + sub;       // the same row in the layers' order
//       uint a = rows_order + i, b = layers_order + layer_row * row_words + word;
//       if (into_layers != 0) data[b] = data[a]; else data[a] = data[b];
//     }
//   }
#include <cstdint>
#include <vector>

#include "SPIRV/SpvBuilder.h"

namespace any360::gfx::vk {

std::vector<uint32_t> BuildViewRowsShader() {
  spv::SpvBuildLogger logger;
  spv::Builder b(0x10000, 0, &logger);  // SPIR-V 1.0
  b.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  b.addCapability(spv::CapabilityShader);

  const spv::Id t_bool = b.makeBoolType();
  const spv::Id t_uint = b.makeUintType(32);
  const spv::Id t_uvec3 = b.makeVectorType(t_uint, 3);

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
  constexpr unsigned kMembers = 7;
  const spv::Id t_region = b.makeStructType(std::vector<spv::Id>(kMembers, t_uint), "Region");
  for (unsigned i = 0; i < kMembers; ++i) b.addMemberDecoration(t_region, i, spv::DecorationOffset, int(4 * i));
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
  auto at = [&](spv::Id index) { return b.createAccessChain(spv::StorageClassUniform, data, {int0, index}); };

  const spv::Id i = b.createCompositeExtract(b.createLoad(global_id, spv::NoPrecision), t_uint, 0);
  spv::Builder::If if_inside(b.createBinOp(spv::OpULessThan, t_bool, i, member(0)), spv::SelectionControlMaskNone, b);
  {
    const spv::Id row_words = member(1), rows = member(2), scale = member(3);
    const spv::Id row = op(spv::OpUDiv, i, row_words);
    const spv::Id word = op(spv::OpISub, i, op(spv::OpIMul, row, row_words));
    const spv::Id pixel = op(spv::OpUDiv, row, op(spv::OpIMul, scale, u(2)));
    const spv::Id sample = op(spv::OpBitwiseAnd, op(spv::OpUDiv, row, scale), u(1));
    const spv::Id sub = op(spv::OpUMod, row, scale);
    const spv::Id layer_row = op(spv::OpIAdd, op(spv::OpIMul, sample, op(spv::OpShiftRightLogical, rows, u(1))),
                                 op(spv::OpIAdd, op(spv::OpIMul, pixel, scale), sub));
    const spv::Id a = op(spv::OpIAdd, member(4), i);
    const spv::Id c = op(spv::OpIAdd, member(5), op(spv::OpIAdd, op(spv::OpIMul, layer_row, row_words), word));
    spv::Builder::If if_into(b.createBinOp(spv::OpINotEqual, t_bool, member(6), u(0)), spv::SelectionControlMaskNone, b);
    b.createStore(b.createLoad(at(a), spv::NoPrecision), at(c));
    if_into.makeBeginElse();
    b.createStore(b.createLoad(at(c), spv::NoPrecision), at(a));
    if_into.makeEndIf();
  }
  if_inside.makeEndIf();
  b.makeReturn(true);
  b.leaveFunction();

  std::vector<unsigned int> words;
  b.dump(words);
  return std::vector<uint32_t>(words.begin(), words.end());
}

}  // namespace any360::gfx::vk
