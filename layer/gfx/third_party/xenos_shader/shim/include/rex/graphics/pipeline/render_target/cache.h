// any360 shim for <rex/graphics/pipeline/render_target/cache.h> (the EDRAM render target cache). The SPIR-V
// render-backend code (spirv_translator_rb.cpp, fragment shader interlock path) only uses the pixel shader interlock
// color format flags below, copied verbatim from RenderTargetCache (BSD-3, see ../../../../../LICENSE).
#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

class RenderTargetCache {
 public:
  // Appended to the format in the format constant via bitwise OR.
  enum : uint32_t {
    kPSIColorFormatFlag_64bpp_Shift = xenos::kColorRenderTargetFormatBits,
    // Requires clamping of blending sources and factors.
    kPSIColorFormatFlag_FixedPointColor_Shift,
    kPSIColorFormatFlag_FixedPointAlpha_Shift,

    kPSIColorFormatFlag_64bpp = uint32_t(1) << kPSIColorFormatFlag_64bpp_Shift,
    kPSIColorFormatFlag_FixedPointColor = uint32_t(1) << kPSIColorFormatFlag_FixedPointColor_Shift,
    kPSIColorFormatFlag_FixedPointAlpha = uint32_t(1) << kPSIColorFormatFlag_FixedPointAlpha_Shift,
  };

  static constexpr uint32_t AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat format) {
    uint32_t format_flags = uint32_t(format);
    if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT ||
        format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
      format_flags |= kPSIColorFormatFlag_64bpp;
    }
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
        format == xenos::ColorRenderTargetFormat::k_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_16_16_16_16 ||
        format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) {
      format_flags |= kPSIColorFormatFlag_FixedPointColor | kPSIColorFormatFlag_FixedPointAlpha;
    } else if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) {
      format_flags |= kPSIColorFormatFlag_FixedPointAlpha;
    }
    return format_flags;
  }
};

}  // namespace rex::graphics
