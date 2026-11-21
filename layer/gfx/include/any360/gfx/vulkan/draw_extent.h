// How far down a draw without clipping reaches, from its vertices (draw_extent.cpp).
#pragma once

#include <cstdint>

#include "any360/gfx/command_processor.h"

namespace rex::graphics {
class Shader;
}

namespace any360::gfx::vk {

// The guest pixel row below the lowest one `draw`'s vertices reach (exclusive; the window offset in, the top-left
// rule applied), its vertex shader run on the CPU for each vertex it fetches. False when it cannot be known: immediate
// indices, tessellation, or a vertex shader that reads textures; the caller keeps its scissor then.
// `cover`, for a single axis-aligned rectangle (a rectangle list of three corners): the guest pixels it surely
// covers (left/top inclusive, right/bottom exclusive, the window offset in); `valid` false otherwise.
struct DrawCover {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool valid = false;
};
bool EstimateVertexMaxY(const uint32_t* regs, const uint8_t* physical, const rex::graphics::Shader& vertex_shader,
                        const DrawCall& draw, uint32_t& max_y, DrawCover* cover = nullptr);

}  // namespace any360::gfx::vk
