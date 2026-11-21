// How far down a draw without clipping reaches, from its vertices: the guest vertex shader run on the CPU (the
// vendored reference interpreter, interpreter.cpp) for each vertex the draw fetches, as the reference's
// DrawExtentEstimator::EstimateVertexMaxY does (draw_extent_estimator.cpp of the ReXGlue SDK, from Xenia; BSD-3).
// Direct3D 9 draws its screen-space rectangles (clears, copies, full-screen passes) with clipping off and a scissor
// of 8192: without this, such a draw counted as written down to the end of EDRAM, over the surfaces below it.
#include "any360/gfx/vulkan/draw_extent.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <optional>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace any360::gfx::vk {

namespace rg = rex::graphics;

namespace {

// The vertex shader's position Y and W, point size and kill flag (the reference's PositionYExportSink).
class PositionYSink : public rg::ShaderInterpreter::ExportSink {
 public:
  void Export(rg::ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    if (reg == rg::ucode::ExportRegister::kVSPosition) {
      if (mask & 0b0001) x = value[0];
      if (mask & 0b0010) y = value[1];
      if (mask & 0b1000) w = value[3];
    } else if (reg == rg::ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
      if (mask & 0b0001) point_size = value[0];
      if (mask & 0b0100) {
        uint32_t bits;
        std::memcpy(&bits, &value[2], 4);
        kill = bits;
      }
    }
  }
  void Reset() { x.reset(), y.reset(), w.reset(), point_size.reset(), kill.reset(); }
  std::optional<float> x, y, w, point_size;
  std::optional<uint32_t> kill;
};

int32_t FloatToFixed16p8(float f32) {  // the reference's ui::FloatToD3D11Fixed16p8
  if (!(std::abs(f32) >= 1.0f / 512.0f)) return 0;
  if (f32 >= 32768.0f - 1.0f / 256.0f) return (1 << 23) - 1;
  if (f32 <= -32768.0f) return -32768 * 256;
  uint32_t bits;
  std::memcpy(&bits, &f32, 4);
  union {
    int32_t s;
    uint32_t u;
  } result;
  result.u = (bits & ((1u << 23) - 1)) | (1u << 23);
  if (bits >> 31) result.s = -result.s;
  const int32_t exponent = int32_t((bits >> 23) & 255) - 127;
  const uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    result.u += (1u << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    result.s >>= extra_bits;
  }
  return result.s;
}

}  // namespace

bool EstimateVertexMaxY(const uint32_t* regs, const uint8_t* physical, const rg::Shader& vertex_shader,
                        const DrawCall& draw, uint32_t& max_y, DrawCover* cover) {
  if (cover) *cover = {};
  if (!draw.index_count) {
    max_y = 0;
    return true;
  }
  // Immediate indices and tessellation are not reproduced (the reference's limits too).
  if (draw.source == DrawSource::kImmediate) return false;
  const rg::RegisterFile file(regs);
  if (draw.major_mode_explicit &&
      file.Get<rg::reg::VGT_OUTPUT_PATH_CNTL>().path_select == rg::xenos::VGTOutputPath::kTessellationEnable) {
    return false;
  }
  if (vertex_shader.type() != rg::xenos::ShaderType::kVertex || !vertex_shader.is_ucode_analyzed() ||
      !rg::ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return false;
  }
  const uint32_t index_size = draw.index_32 ? 4 : 2;
  const uint32_t index_words = draw.source == DrawSource::kDma ? draw.index_bytes / index_size : 0;
  // 16-bit indices swap within 16 bits, as the primitive processor normalizes them.
  Endian index_endian = draw.index_endian;
  if (!draw.index_32) index_endian = index_endian == Endian::k8in32 ? Endian::k8in16 : index_endian == Endian::k16in32 ? Endian::kNone : index_endian;
  const uint8_t* indices = draw.source == DrawSource::kDma ? physical + (draw.index_base & 0x1FFFFFFF & ~(index_size - 1)) : nullptr;

  const auto mode_cntl = file.Get<rg::reg::PA_SU_SC_MODE_CNTL>();
  const uint32_t reset_index = file.Get<rg::reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  const uint32_t index_offset = file.Get<rg::reg::VGT_INDX_OFFSET>().indx_offset;
  const uint32_t min_index = file.Get<rg::reg::VGT_MIN_VTX_INDX>().min_indx;
  const uint32_t max_index = file.Get<rg::reg::VGT_MAX_VTX_INDX>().max_indx;
  const auto vte = file.Get<rg::reg::PA_CL_VTE_CNTL>();
  const float y_scale = vte.vport_y_scale_ena ? file.Get<float>(rg::XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  const float y_offset = vte.vport_y_offset_ena ? file.Get<float>(rg::XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  const float x_scale = vte.vport_x_scale_ena ? file.Get<float>(rg::XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  const float x_offset = vte.vport_x_offset_ena ? file.Get<float>(rg::XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  // A single rectangle (three corners): the window coordinates of its corners, for the area it surely covers.
  const bool one_rectangle = cover && draw.primitive_type == 0x08 && draw.index_count == 3;
  float corner_x[3] = {}, corner_y[3] = {};
  uint32_t corners = 0;
  const bool point_list = draw.primitive_type == uint32_t(rg::xenos::PrimitiveType::kPointList);
  int32_t point_min_diameter = 0, point_max_diameter = 0;
  float point_constant_radius = 0.0f;
  if (point_list) {
    const auto minmax = file.Get<rg::reg::PA_SU_POINT_MINMAX>();
    const float min_d = float(minmax.min_size) * (2.0f / 16.0f), max_d = float(minmax.max_size) * (2.0f / 16.0f);
    std::memcpy(&point_min_diameter, &min_d, 4);
    std::memcpy(&point_max_diameter, &max_d, 4);
    point_constant_radius = float(file.Get<rg::reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  const rex::memory::Memory memory(physical);
  rg::ShaderInterpreter interpreter(file, memory);
  interpreter.SetShader(vertex_shader);
  PositionYSink sink;
  interpreter.SetExportSink(&sink);
  float highest = -FLT_MAX;
  for (uint32_t i = 0; i < draw.index_count; ++i) {
    uint32_t vertex_index;
    if (draw.source == DrawSource::kDma) {
      if (i < index_words) {
        if (draw.index_32) {
          uint32_t v;
          std::memcpy(&v, indices + 4 * size_t(i), 4);
          vertex_index = rg::xenos::GpuSwap(v, rg::xenos::Endian(uint32_t(index_endian)));
        } else {
          uint16_t v;
          std::memcpy(&v, indices + 2 * size_t(i), 2);
          vertex_index = rg::xenos::GpuSwap(v, rg::xenos::Endian(uint32_t(index_endian)));
        }
        vertex_index &= 0xFFFFFF;  // the Xenos uses 24 bits of an index
      } else {
        vertex_index = 0;
      }
      if (mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) continue;
    } else {
      vertex_index = i;
    }
    vertex_index = std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    sink.Reset();
    interpreter.temp_registers()[0] = float(vertex_index);
    interpreter.Execute();
    if (sink.kill && (*sink.kill & ~(1u << 31))) continue;
    if (!sink.y) continue;
    float y = *sink.y;
    float x = sink.x ? *sink.x : 0.0f;
    if (!vte.vtx_xy_fmt) {
      if (!sink.w) continue;
      y /= *sink.w;
      x /= *sink.w;
    }
    y = y * y_scale + y_offset;
    x = x * x_scale + x_offset;
    if (one_rectangle && sink.x && corners < 3) corner_x[corners] = x, corner_y[corners++] = y;
    if (point_list) {
      float radius = point_constant_radius;
      if (sink.point_size) {
        int32_t bits;
        std::memcpy(&bits, &*sink.point_size, 4);
        bits = std::min(point_max_diameter, std::max(point_min_diameter, bits));
        float diameter;
        std::memcpy(&diameter, &bits, 4);
        radius = 0.5f * diameter;
      }
      y += radius;
    }
    highest = std::max(highest, y);  // NaN keeps the previous value
  }
  interpreter.SetExportSink(nullptr);

  int32_t y_24p8 = FloatToFixed16p8(highest);
  if (file.Get<rg::reg::PA_SU_VTX_CNTL>().pix_center == rg::xenos::PixelCenter::kD3DZero) y_24p8 += 128;  // D3D pixel centres at .0
  if (mode_cntl.vtx_window_offset_enable) y_24p8 += file.Get<rg::reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  // Top-left rule: .5 exclusive without MSAA, 1.0 exclusive with it.
  const bool msaa = file.Get<rg::reg::RB_SURFACE_INFO>().msaa_samples != rg::xenos::MsaaSamples::k1X;
  max_y = (uint32_t(std::max(0, y_24p8)) + (msaa ? 255u : 127u)) >> 8;
  // The rectangle's surely covered pixels: an axis-aligned one (two distinct x and two distinct y among its corners),
  // a pixel in from each edge (whatever the pixel centre and the fill rules), the window offset in.
  if (one_rectangle && corners == 3) {
    const float min_x = std::min({corner_x[0], corner_x[1], corner_x[2]}), max_x = std::max({corner_x[0], corner_x[1], corner_x[2]});
    const float min_y = std::min({corner_y[0], corner_y[1], corner_y[2]}), max_y_f = std::max({corner_y[0], corner_y[1], corner_y[2]});
    bool aligned = true;
    for (uint32_t i = 0; i < 3; ++i) {
      aligned &= (corner_x[i] == min_x || corner_x[i] == max_x) && (corner_y[i] == min_y || corner_y[i] == max_y_f);
    }
    if (aligned && min_x > -16384.0f && max_x < 16384.0f && min_y > -16384.0f && max_y_f < 16384.0f) {
      const auto offset = file.Get<rg::reg::PA_SC_WINDOW_OFFSET>();
      const int32_t dx = mode_cntl.vtx_window_offset_enable ? offset.window_x_offset : 0;
      const int32_t dy = mode_cntl.vtx_window_offset_enable ? offset.window_y_offset : 0;
      cover->left = int32_t(std::ceil(min_x)) + 1 + dx, cover->right = int32_t(std::floor(max_x)) - 1 + dx;
      cover->top = int32_t(std::ceil(min_y)) + 1 + dy, cover->bottom = int32_t(std::floor(max_y_f)) - 1 + dy;
      cover->valid = cover->right > cover->left && cover->bottom > cover->top;
    }
  }
  return true;
}

}  // namespace any360::gfx::vk
