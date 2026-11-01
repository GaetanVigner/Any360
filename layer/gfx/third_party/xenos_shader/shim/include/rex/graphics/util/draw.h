// any360 shim for <rex/graphics/util/draw.h> (draw-state helpers over the register file and guest memory). The
// SPIR-V render-backend code (spirv_translator_rb.cpp) only uses the D3D10 standard MSAA sample positions, whose
// values are copied from src/graphics/util/draw.cpp (BSD-3, see ../../../../LICENSE).
#pragma once

#include <cstdint>

namespace rex::graphics::draw_util {

inline constexpr int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
inline constexpr int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace rex::graphics::draw_util
