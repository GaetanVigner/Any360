#include "any360/gfx/glyph_pages.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "any360/sys/json.h"

namespace any360::gfx {

std::string_view WholeStick(std::string_view control) {
  // The axis's glyph is the stick itself, with no click mark (a "left_stick" is the click: L3 on a PlayStation pad).
  if (control.starts_with("left_")) return "left_x";
  if (control.starts_with("right_")) return "right_x";
  if (control.starts_with("dpad")) return "dpad";
  return control;
}

void FitGlyph(const sys::RgbaImage& src, uint32_t sx, uint32_t sy, uint32_t size, uint32_t* dst, uint32_t dst_stride,
              uint32_t width, uint32_t height) {
  for (uint32_t y = 0; y < height; ++y) std::fill_n(dst + size_t(y) * dst_stride, width, 0u);
  const uint32_t d = std::min(width, height);
  if (!d || !size) return;
  const uint32_t ox = (width - d) / 2, oy = (height - d) / 2;
  for (uint32_t y = 0; y < d; ++y) {
    for (uint32_t x = 0; x < d; ++x) {
      double r = 0, g = 0, b = 0, a = 0, n = 0;
      if (size >= d) {
        // Smaller: the mean of the source texels whose centres fall in this pixel's footprint.
        const uint32_t x0 = x * size / d, x1 = std::max(x0 + 1, (x + 1) * size / d);
        const uint32_t y0 = y * size / d, y1 = std::max(y0 + 1, (y + 1) * size / d);
        for (uint32_t v = y0; v < y1; ++v)
          for (uint32_t u = x0; u < x1; ++u) {
            const uint32_t p = src.at(sx + u, sy + v);
            const double pa = (p >> 24) / 255.0;
            r += (p & 0xFF) * pa, g += (p >> 8 & 0xFF) * pa, b += (p >> 16 & 0xFF) * pa, a += pa, n += 1;
          }
      } else {
        // Larger: bilinear, edges clamped.
        const double fx = (x + 0.5) * size / d - 0.5, fy = (y + 0.5) * size / d - 0.5;
        const int u0 = int(std::floor(fx)), v0 = int(std::floor(fy));
        const double wx = fx - u0, wy = fy - v0;
        for (int j = 0; j < 2; ++j)
          for (int i = 0; i < 2; ++i) {
            const uint32_t u = uint32_t(std::clamp(u0 + i, 0, int(size) - 1)), v = uint32_t(std::clamp(v0 + j, 0, int(size) - 1));
            const double w = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
            const uint32_t p = src.at(sx + u, sy + v);
            const double pa = (p >> 24) / 255.0 * w;
            r += (p & 0xFF) * pa, g += (p >> 8 & 0xFF) * pa, b += (p >> 16 & 0xFF) * pa, a += pa, n += w;
          }
      }
      uint32_t out = 0;
      if (a > 0) {
        const auto c = [](double v) { return uint32_t(std::clamp(std::lround(v), 0L, 255L)); };
        out = c(r / a) | c(g / a) << 8 | c(b / a) << 16 | c(a / n * 255.0) << 24;
      }
      dst[size_t(oy + y) * dst_stride + ox + x] = out;
    }
  }
}

GlyphPages::GlyphPages(std::string glyphs_dir, uint32_t scale) : dir_(std::move(glyphs_dir)), scale_(std::clamp(scale, 1u, 8u)) {}

void GlyphPages::SetPages(std::vector<GlyphPage> pages) {
  pages_ = std::move(pages);
  pages_changed_ = true;
}

GlyphPages::Set* GlyphPages::LoadSet(std::string_view name) {
  auto it = sets_.find(name);
  if (it == sets_.end()) it = sets_.emplace(std::string(name), Set{}).first;
  Set& s = it->second;
  if (s.loaded) return &s;
  if (s.failed) return nullptr;
  auto read = [&](const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  };
  const std::string base = dir_ + "/" + std::string(name);
  const std::vector<uint8_t> index = read(base + ".json");
  sys::json::Value v;
  std::string error;
  if (index.empty() || !sys::json::Parse({reinterpret_cast<const char*>(index.data()), index.size()}, v, &error)) {
    last_error_ = base + ".json: " + (index.empty() ? "missing" : error);
    s.failed = true;
    return nullptr;
  }
  if (const sys::json::Value* controls = v.Find("controls"); controls && controls->is(sys::json::Value::Type::kObject)) {
    for (const auto& [control, place] : controls->object) {
      const sys::json::Value* c = place.Find("column");
      const sys::json::Value* r = place.Find("row");
      if (c && r && c->is(sys::json::Value::Type::kNumber) && r->is(sys::json::Value::Type::kNumber))
        s.cells[control] = {uint32_t(c->number), uint32_t(r->number)};
    }
  }
  const sys::json::Value* atlases = v.Find("atlases");
  for (const char* size : {"128", "64"}) {
    const sys::json::Value* a = atlases ? atlases->Find(size) : nullptr;
    const sys::json::Value* file = a ? a->Find("file") : nullptr;
    const sys::json::Value* cell = a ? a->Find("cell") : nullptr;
    sys::RgbaImage& img = size[0] == '1' ? s.atlas128 : s.atlas64;
    if (!file || !file->is(sys::json::Value::Type::kString) ||
        !sys::DecodePng(read(dir_ + "/" + file->string), img, &error)) {
      last_error_ = base + ".json: atlas " + size + ": " + (file ? error : std::string("not named"));
      s.failed = true;
      return nullptr;
    }
    if (cell && cell->is(sys::json::Value::Type::kNumber)) (size[0] == '1' ? s.cell128 : s.cell64) = uint32_t(cell->number);
  }
  s.loaded = true;
  return &s;
}

bool GlyphPages::Update(uint32_t generation, const GlyphFn& glyph, TexturePatches& patches) {
  if (!pages_changed_ && updated_once_ && generation == generation_) return false;
  generation_ = generation;
  // What each cell shows now: unchanged answers and pages set nothing.
  std::vector<std::string> answers;
  for (const GlyphPage& page : pages_)
    for (const GlyphCell& cell : page.cells) {
      const sys::Glyph g = glyph(cell.user, cell.control);
      std::string_view name = g.control;
      if (cell.form == GlyphCell::Form::kWholeStick) name = WholeStick(name);
      answers.push_back(name.empty() ? std::string() : std::string(g.set) + "/" + std::string(name));
    }
  if (!pages_changed_ && updated_once_ && answers == answers_) return false;
  updated_once_ = true;
  pages_changed_ = false;
  answers_ = answers;

  std::vector<uint32_t> bases;
  size_t index = 0;
  for (const GlyphPage& page : pages_) {
    TexturePatch patch;
    patch.base = page.base;
    patch.width = page.width;
    patch.height = page.height;
    patch.scale = scale_;
    for (const GlyphCell& cell : page.cells) {
      const std::string& answer = answers[index++];
      const size_t slash = answer.find('/');
      if (slash == std::string::npos) continue;  // nothing to show: the title's own art stays
      Set* set = LoadSet(std::string_view(answer).substr(0, slash));
      if (!set) continue;
      const auto place = set->cells.find(std::string_view(answer).substr(slash + 1));
      if (place == set->cells.end()) continue;
      const uint32_t w = cell.width * scale_, h = cell.height * scale_;
      // The smaller atlas when its cells are as large as the drawn glyph (no enlargement), else the larger.
      const bool small = std::min(w, h) <= set->cell64;
      const sys::RgbaImage& atlas = small ? set->atlas64 : set->atlas128;
      const uint32_t c = small ? set->cell64 : set->cell128;
      if ((place->second.first + 1) * c > atlas.width || (place->second.second + 1) * c > atlas.height) continue;
      TexturePatch::Region region{cell.x, cell.y, cell.width, cell.height, std::vector<uint32_t>(size_t(w) * h)};
      FitGlyph(atlas, place->second.first * c, place->second.second * c, c, region.pixels.data(), w, w, h);
      patch.regions.push_back(std::move(region));
    }
    if (patch.regions.empty()) {
      patches.Remove(page.base);
      continue;
    }
    patches.Set(std::move(patch));
    ++patches_set_;
    bases.push_back(page.base & 0x1FFFFFFF);
  }
  // Pages patched before and no longer listed: the title's own again.
  for (uint32_t old : patched_bases_)
    if (std::find(bases.begin(), bases.end(), old) == bases.end()) patches.Remove(old);
  patched_bases_ = std::move(bases);
  return true;
}

}  // namespace any360::gfx
