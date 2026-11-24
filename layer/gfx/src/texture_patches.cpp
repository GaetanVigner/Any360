#include "any360/gfx/texture_patches.h"

#include <algorithm>

namespace any360::gfx {

TexturePatches& TexturePatches::Global() {
  static TexturePatches patches;
  return patches;
}

void TexturePatches::Set(TexturePatch patch) {
  patch.base &= 0x1FFFFFFF;
  patch.scale = std::clamp(patch.scale, 1u, 8u);
  // Regions clipped to the texture (their pixels kept at their own stride: cropped here).
  for (TexturePatch::Region& r : patch.regions) {
    if (r.x >= patch.width || r.y >= patch.height) {
      r.width = r.height = 0;
      r.pixels.clear();
      continue;
    }
    const uint32_t w = std::min(r.width, patch.width - r.x), h = std::min(r.height, patch.height - r.y);
    if ((w != r.width || h != r.height) && r.pixels.size() == size_t(r.width) * r.height * patch.scale * patch.scale) {
      std::vector<uint32_t> cropped(size_t(w) * h * patch.scale * patch.scale);
      for (uint32_t y = 0; y < h * patch.scale; ++y)
        std::copy_n(r.pixels.data() + size_t(y) * r.width * patch.scale, w * patch.scale,
                    cropped.data() + size_t(y) * w * patch.scale);
      r.pixels = std::move(cropped);
    }
    r.width = w, r.height = h;
  }
  std::lock_guard lock(mutex_);
  patch.version = generation_.load(std::memory_order_relaxed) + 1;
  patches_[patch.base] = std::make_shared<const TexturePatch>(std::move(patch));
  count_.store(patches_.size(), std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
}

void TexturePatches::Remove(uint32_t base) {
  std::lock_guard lock(mutex_);
  if (patches_.erase(base & 0x1FFFFFFF)) {
    count_.store(patches_.size(), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }
}

std::shared_ptr<const TexturePatch> TexturePatches::Find(uint32_t base) const {
  std::lock_guard lock(mutex_);
  const auto it = patches_.find(base & 0x1FFFFFFF);
  return it == patches_.end() ? nullptr : it->second;
}

namespace {

// Per byte: floor((a * (256 - w) + b * w) / 256), i.e. a + (b - a) * w / 256 rounded down; two bytes a lane (each
// lane's sum stays below 2^16).
inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t w256) {
  const uint32_t iw = 256 - w256;
  const uint32_t rb = ((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w256) >> 8 & 0x00FF00FFu;
  const uint32_t ga = ((a >> 8 & 0x00FF00FFu) * iw + (b >> 8 & 0x00FF00FFu) * w256) & 0xFF00FF00u;
  return rb | ga;
}

// Where host texel `x` of a `s`-times enlargement samples the guest: texels i0, i1 and the weight of i1 (1/256ths),
// edges clamped. Host texel centre (x + 0.5) / s - 0.5 in guest texels.
struct Tap {
  uint32_t i0, i1, w;
};
inline Tap TapAt(uint32_t x, uint32_t s, uint32_t size) {
  const int f = int(((2 * x + 1) * 256) / (2 * s)) - 128;
  return {uint32_t(std::clamp(f >> 8, 0, int(size) - 1)), uint32_t(std::clamp((f >> 8) + 1, 0, int(size) - 1)),
          f < 0 ? 0u : uint32_t(f & 0xFF)};
}

}  // namespace

void ComposePatched(const sys::RgbaImage& guest, const TexturePatch& patch, sys::RgbaImage& out) {
  const uint32_t s = patch.scale;
  out.width = guest.width * s;
  out.height = guest.height * s;
  out.pixels.resize(size_t(out.width) * out.height);
  if (guest.width == 0 || guest.height == 0) return;
  // Bilinear: each guest row enlarged across once (into the rows `s` host rows share), then rows blended down.
  thread_local std::vector<Tap> taps;
  thread_local std::vector<uint32_t> across[2];
  thread_local uint32_t across_row[2];
  taps.resize(out.width);
  for (uint32_t x = 0; x < out.width; ++x) taps[x] = TapAt(x, s, guest.width);
  across[0].resize(out.width), across[1].resize(out.width);
  across_row[0] = across_row[1] = UINT32_MAX;
  // Guest row `gy` enlarged across, in one of two buffers: the one not holding row `keep`.
  auto row_across = [&](uint32_t gy, uint32_t keep) -> const uint32_t* {
    for (int k = 0; k < 2; ++k)
      if (across_row[k] == gy) return across[k].data();
    const int k = across_row[0] == keep ? 1 : 0;
    const uint32_t* g = guest.pixels.data() + size_t(gy) * guest.width;
    uint32_t* d = across[k].data();
    for (uint32_t x = 0; x < out.width; ++x) d[x] = Lerp(g[taps[x].i0], g[taps[x].i1], taps[x].w);
    across_row[k] = gy;
    return d;
  };
  for (uint32_t y = 0; y < out.height; ++y) {
    const Tap t = TapAt(y, s, guest.height);
    const uint32_t* top = row_across(t.i0, t.i1);
    const uint32_t* bottom = row_across(t.i1, t.i0);
    uint32_t* row = out.pixels.data() + size_t(y) * out.width;
    if (t.w == 0 || t.i0 == t.i1) {
      std::copy_n(top, out.width, row);
    } else {
      for (uint32_t x = 0; x < out.width; ++x) row[x] = Lerp(top[x], bottom[x], t.w);
    }
  }
  for (const TexturePatch::Region& r : patch.regions) {
    const uint32_t w = r.width * s, h = r.height * s;
    if (!w || r.pixels.size() != size_t(w) * h || r.x + r.width > guest.width || r.y + r.height > guest.height) continue;
    for (uint32_t y = 0; y < h; ++y)
      std::copy_n(r.pixels.data() + size_t(y) * w, w, out.pixels.data() + size_t(r.y * s + y) * out.width + r.x * s);
  }
}

void BuildMips(const sys::RgbaImage& level0, std::vector<sys::RgbaImage>& levels) {
  const sys::RgbaImage* above = &level0;
  while (above->width > 1 || above->height > 1) {
    sys::RgbaImage next;
    next.width = std::max(above->width / 2, 1u);
    next.height = std::max(above->height / 2, 1u);
    next.pixels.resize(size_t(next.width) * next.height);
    for (uint32_t y = 0; y < next.height; ++y) {
      const uint32_t y0 = std::min(2 * y, above->height - 1), y1 = std::min(2 * y + 1, above->height - 1);
      for (uint32_t x = 0; x < next.width; ++x) {
        const uint32_t x0 = std::min(2 * x, above->width - 1), x1 = std::min(2 * x + 1, above->width - 1);
        const uint32_t p[4] = {above->at(x0, y0), above->at(x1, y0), above->at(x0, y1), above->at(x1, y1)};
        uint32_t v = 0;
        for (uint32_t sh = 0; sh < 32; sh += 8)
          v |= ((((p[0] >> sh) & 0xFF) + ((p[1] >> sh) & 0xFF) + ((p[2] >> sh) & 0xFF) + ((p[3] >> sh) & 0xFF) + 2) / 4) << sh;
        next.pixels[size_t(y) * next.width + x] = v;
      }
    }
    levels.push_back(std::move(next));
    above = &levels.back();
  }
}

}  // namespace any360::gfx
