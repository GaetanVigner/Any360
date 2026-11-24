// Texture patches (docs/GLYPHS.md, section 4, mechanism A keyed by address): a guest 2D texture shown with regions
// replaced in the host image, never in guest memory. A title adapter finds the texture through the title's own objects
// (a font page's address, read from the font the title draws prompts with: no content hash) and hands its regions'
// pixels here; the texture cache, when it uploads that texture, decodes the guest's base level, enlarges it `scale`
// times and draws the regions over it, so a 4x patch gives sharp glyphs on a 256x256 page. The guest's content still
// counts: a texture rewritten by the title is patched again over its new content.
//
//   TexturePatches::Global().Set({base, 256, 256, 4, {{x, y, w, h, pixels}...}});  // pixels: (w*4) x (h*4) RGBA8
//
// Thread-safe: an adapter sets patches from the title's threads, the renderer reads them on its own.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "any360/sys/png.h"

namespace any360::gfx {

struct TexturePatch {
  uint32_t base = 0;            // the texture's base address (physical: masked with 0x1FFFFFFF)
  uint32_t width = 0, height = 0;  // the guest texture's size, texels (a patch applies only at this size)
  uint32_t scale = 1;           // host texels a guest texel along each axis (1-8)
  struct Region {
    uint32_t x = 0, y = 0, width = 0, height = 0;  // guest texels
    std::vector<uint32_t> pixels;  // (width * scale) x (height * scale) RGBA8, R in the low byte, straight alpha;
                                   // replaces the region's texels, alpha included
  };
  std::vector<Region> regions;
  uint64_t version = 0;  // set by TexturePatches::Set: moves with each change
};

class TexturePatches {
 public:
  // The process's patches, which the Vulkan renderer's texture cache reads.
  static TexturePatches& Global();

  // Adds or replaces the patch at `patch.base`. Regions outside the texture are clipped.
  void Set(TexturePatch patch);
  void Remove(uint32_t base);
  // The patch at `base` (physical), or null. The pointer stays valid while it is held.
  std::shared_ptr<const TexturePatch> Find(uint32_t base) const;
  // Moves on every Set or Remove: a cache rechecks its patched textures then.
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
  bool empty() const { return count_.load(std::memory_order_acquire) == 0; }

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, std::shared_ptr<const TexturePatch>> patches_;
  std::atomic<uint64_t> generation_{0};
  std::atomic<size_t> count_{0};
};

// The host image of a patched texture: `guest` (the base level decoded, RGBA8) enlarged `patch.scale` times
// (bilinear, edges clamped, as a sampler would read the original between its texels), then each region's pixels written
// over it. `out`'s buffer is reused.
void ComposePatched(const sys::RgbaImage& guest, const TexturePatch& patch, sys::RgbaImage& out);

// The mip chain below `level0` (each level a 2x2 box filter of the one above, odd sizes clamped), appended to `levels`
// (level 1 first) down to 1x1: a patched texture drawn smaller than its host size does not shimmer.
void BuildMips(const sys::RgbaImage& level0, std::vector<sys::RgbaImage>& levels);

}  // namespace any360::gfx
