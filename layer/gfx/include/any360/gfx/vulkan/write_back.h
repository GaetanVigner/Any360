// A resolve's pixels written back into guest memory: the rows the GPU copied out of the render target (linear,
// `width` elements a row) into the destination's 32x32-block tiling, each element in the destination's byte order;
// depth as the guest's depth word (24-bit depth << 8, unorm or float24). The renderer's completion thread runs it
// for each resolve the title reads back (VulkanRenderer::FinishWriteBacks).
#pragma once

#include <cstdint>

namespace any360::gfx::vk {

struct WriteBack {
  uint32_t base = 0;            // guest physical address of the destination
  uint32_t x = 0, y = 0;        // where the rectangle starts in the destination, in elements
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;           // the destination's pitch, in elements
  uint32_t endian = 0;          // RB_COPY_DEST_INFO's Endian128
  uint32_t element = 4;         // bytes an element: 4 or 8
  bool depth = false, float24 = false;
};

// Writes `src` into `physical` (guest physical memory, `mirror_bytes` long; an element past its end is left out).
void WriteBackTiled(uint8_t* physical, uint32_t mirror_bytes, const uint8_t* src, const WriteBack& w);
// The first version, kept as the model the tests compare WriteBackTiled with: one tiled offset per 16 bytes.
void WriteBackTiledReference(uint8_t* physical, uint32_t mirror_bytes, const uint8_t* src, const WriteBack& w);

}  // namespace any360::gfx::vk
