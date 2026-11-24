// Texture patches (any360/gfx/texture_patches.h) without a GPU: the registry (versions, clipping, removal), the
// composed host image against a reference model of bilinear enlargement written texel by texel in floating point, and
// the mip chain against a plain box filter. --bench: a 256x256 page patched at 4x (the composition and its mips, time
// and host bytes and allocations a call).
//   gfx_patch_tests [--bench]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "any360/gfx/glyph_pages.h"
#include "any360/gfx/texture_patches.h"
#include "any360/sys/alloc_meter.h"
#include "any360/sys/json.h"

using namespace any360;
using namespace any360::gfx;

namespace {

int g_failures = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("gfx_patch_tests: FAILED line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

sys::RgbaImage Random(uint32_t w, uint32_t h, uint32_t seed) {
  std::mt19937 rng(seed);
  sys::RgbaImage img{w, h, std::vector<uint32_t>(size_t(w) * h)};
  for (uint32_t& p : img.pixels) p = rng();
  return img;
}

// The reference: each host texel's centre mapped to guest coordinates, the four nearest guest texels (clamped)
// weighted in floating point, each component rounded down as the fixed-point version does within one step.
uint32_t ReferenceTexel(const sys::RgbaImage& g, uint32_t s, uint32_t x, uint32_t y, int component) {
  const double gx = (x + 0.5) / s - 0.5, gy = (y + 0.5) / s - 0.5;
  const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
  const double wx = gx - x0, wy = gy - y0;
  auto c = [&](int xx, int yy) {
    xx = std::clamp(xx, 0, int(g.width) - 1), yy = std::clamp(yy, 0, int(g.height) - 1);
    return double(g.at(uint32_t(xx), uint32_t(yy)) >> (8 * component) & 0xFF);
  };
  const double top = c(x0, y0) + (c(x0 + 1, y0) - c(x0, y0)) * (x0 < 0 ? 0 : wx);
  const double bottom = c(x0, y0 + 1) + (c(x0 + 1, y0 + 1) - c(x0, y0 + 1)) * (x0 < 0 ? 0 : wx);
  return uint32_t(std::lround(top + (bottom - top) * (y0 < 0 ? 0 : wy)));
}

void RegistryTests() {
  TexturePatches patches;
  CHECK(patches.empty() && patches.generation() == 0 && !patches.Find(0x1000));
  TexturePatch p;
  p.base = 0xA0001000;  // a virtual address: kept by its physical one
  p.width = p.height = 64;
  p.scale = 2;
  p.regions.push_back({60, 62, 8, 4, std::vector<uint32_t>(16 * 8, 0xFF0000FFu)});  // runs past both edges
  patches.Set(p);
  auto found = patches.Find(0x1000);
  CHECK(found && !patches.empty() && patches.generation() == 1);
  CHECK(found && found->regions[0].width == 4 && found->regions[0].height == 2);
  CHECK(found && found->regions[0].pixels.size() == 8 * 4);  // cropped to (4 * 2) x (2 * 2)
  const uint64_t v1 = found ? found->version : 0;
  patches.Set(p);
  auto second = patches.Find(0xE0001000);
  CHECK(second && second->version != v1 && patches.generation() == 2);
  CHECK(found && found->version == v1);  // an answer held stays valid as it was
  p.scale = 99;
  patches.Set(p);
  CHECK(patches.Find(0x1000)->scale == 8);  // scale clamped to 1-8
  patches.Remove(0x1000);
  CHECK(patches.empty() && !patches.Find(0x1000) && patches.generation() == 4);
  patches.Remove(0x1000);  // nothing there: no change
  CHECK(patches.generation() == 4);
}

void ComposeTests() {
  for (uint32_t s : {1u, 2u, 3u, 4u}) {
    const sys::RgbaImage guest = Random(13, 9, s);
    TexturePatch p;
    p.width = 13, p.height = 9, p.scale = s;
    p.regions.push_back({3, 2, 4, 3, std::vector<uint32_t>(size_t(4 * s) * 3 * s)});
    for (size_t i = 0; i < p.regions[0].pixels.size(); ++i) p.regions[0].pixels[i] = uint32_t(i * 2654435761u);
    p.regions.push_back({12, 8, 2, 2, std::vector<uint32_t>(size_t(4) * s * s, 1)});  // outside: skipped
    sys::RgbaImage out;
    ComposePatched(guest, p, out);
    CHECK(out.width == 13 * s && out.height == 9 * s);
    uint32_t wrong = 0, off_by_more = 0;
    for (uint32_t y = 0; y < out.height; ++y)
      for (uint32_t x = 0; x < out.width; ++x) {
        const bool in = x >= 3 * s && x < 7 * s && y >= 2 * s && y < 5 * s;
        if (in) {
          wrong += out.at(x, y) != p.regions[0].pixels[(y - 2 * s) * 4 * s + (x - 3 * s)];
          continue;
        }
        for (int c = 0; c < 4; ++c) {
          const int d = int(out.at(x, y) >> (8 * c) & 0xFF) - int(ReferenceTexel(guest, s, x, y, c));
          off_by_more += d < -2 || d > 2;  // fixed point (1/256 weights, two steps) against exact
        }
      }
    std::printf("gfx_patch_tests: compose at %ux: region %u texels wrong, enlargement %u components off by more than 2\n",
                s, wrong, off_by_more);
    CHECK(wrong == 0 && off_by_more == 0);
    // Scale 1: the guest exactly, outside the region.
    if (s == 1) {
      uint32_t diff = 0;
      for (uint32_t y = 0; y < 9; ++y)
        for (uint32_t x = 0; x < 13; ++x)
          if (!(x >= 3 && x < 7 && y >= 2 && y < 5)) diff += out.at(x, y) != guest.at(x, y);
      CHECK(diff == 0);
    }
  }
  // A uniform guest stays uniform at any scale (no edge darkening).
  const sys::RgbaImage flat{8, 8, std::vector<uint32_t>(64, 0x80402010u)};
  TexturePatch p;
  p.width = p.height = 8, p.scale = 4;
  sys::RgbaImage out;
  ComposePatched(flat, p, out);
  uint32_t other = 0;
  for (uint32_t v : out.pixels) other += v != 0x80402010u;
  CHECK(other == 0);
}

void MipTests() {
  const sys::RgbaImage base = Random(10, 6, 7);
  std::vector<sys::RgbaImage> mips;
  BuildMips(base, mips);
  CHECK(mips.size() == 3);  // 5x3, 2x1, 1x1
  CHECK(mips.size() == 3 && mips[0].width == 5 && mips[0].height == 3 && mips[1].width == 2 && mips[1].height == 1 &&
        mips[2].width == 1 && mips[2].height == 1);
  uint32_t wrong = 0;
  for (uint32_t y = 0; y < 3; ++y)
    for (uint32_t x = 0; x < 5; ++x)
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) sum += base.at(2 * x + dx, 2 * y + dy) >> (8 * c) & 0xFF;
        wrong += (mips[0].at(x, y) >> (8 * c) & 0xFF) != (sum + 2) / 4;
      }
  CHECK(wrong == 0);
  std::vector<sys::RgbaImage> none;
  BuildMips(sys::RgbaImage{1, 1, {5}}, none);
  CHECK(none.empty());
}

void FitTests() {
  CHECK(WholeStick("left_x+") == "left_x" && WholeStick("left_stick") == "left_x" && WholeStick("right_y") == "right_x" &&
        WholeStick("dpad_up") == "dpad");
  CHECK(WholeStick("south") == "south" && WholeStick("key:space") == "key:space" && WholeStick("key:wasd") == "key:wasd");
  CHECK(WholeStick("mouse:x") == "mouse:move" && WholeStick("mouse:y") == "mouse:move");
  // Same size: the cell's texels exactly (opaque ones; transparent ones as transparent black).
  sys::RgbaImage src = Random(32, 16, 3);
  for (uint32_t& p : src.pixels) p |= 0xFF000000u;
  std::vector<uint32_t> dst(16 * 16, 0x12345678u);
  FitGlyph(src, 16, 0, 16, dst.data(), 16, 16, 16);
  uint32_t diff = 0;
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) diff += dst[y * 16 + x] != src.at(16 + x, y);
  CHECK(diff == 0);
  // Wider than tall: centred, the sides transparent; half the size: each pixel the mean of 2x2.
  std::vector<uint32_t> wide(12 * 8, 0x12345678u);
  FitGlyph(src, 0, 0, 16, wide.data(), 12, 12, 8);
  uint32_t sides = 0, means = 0;
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 12; ++x) {
      const uint32_t v = wide[y * 12 + x];
      if (x < 2 || x >= 10) {
        sides += v != 0;
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) sum += src.at(2 * (x - 2) + dx, 2 * y + dy) >> (8 * c) & 0xFF;
        const int d = int(v >> (8 * c) & 0xFF) - int((sum + 2) / 4);
        means += d < -1 || d > 1;
      }
      means += (v >> 24) != 0xFF;
    }
  CHECK(sides == 0 && means == 0);
  // A transparent texel's colour does not bleed into its neighbours (alpha-weighted).
  sys::RgbaImage edge{2, 2, {0xFF0000FFu, 0x00FFFFFFu, 0xFF0000FFu, 0x00FFFFFFu}};  // red opaque | white transparent
  uint32_t one = 0;
  FitGlyph(edge, 0, 0, 2, &one, 1, 1, 1);
  CHECK((one & 0xFFFFFF) == 0x0000FF && (one >> 24) == 128);
}

#ifdef ANY360_GLYPHS_DIR
// Every name the glyph query can answer for a pad family has art in that family's set: each physical control of
// its pad (the touchpad on the PlayStation 4 and 5 only), the axis halves, and what WholeStick turns them into.
void CoverageTests() {
  for (const char* family : {"xbox360", "xbox_one", "ps3", "ps4", "ps5", "switch", "steam_controller", "steam_deck"}) {
    std::ifstream f(std::string(ANY360_GLYPHS_DIR) + "/" + family + ".json", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    sys::json::Value v;
    CHECK(sys::json::Parse(text, v));
    const sys::json::Value* controls = v.Find("controls");
    CHECK(controls != nullptr);
    if (!controls) continue;
    const bool touchpad = std::strcmp(family, "ps4") == 0 || std::strcmp(family, "ps5") == 0;
    uint32_t missing = 0;
    for (uint32_t c = 0; c < uint32_t(sys::PadControl::kCount); ++c) {
      if (sys::PadControl(c) == sys::PadControl::kTouchpad && !touchpad) continue;
      const std::string_view name = sys::Name(sys::PadControl(c));
      for (const std::string_view n : {name, WholeStick(name)}) {
        if (!controls->Find(n)) {
          std::printf("  %s: no art for %.*s\n", family, int(n.size()), n.data());
          ++missing;
        }
      }
    }
    CHECK(missing == 0);
  }
}

void PagesTests() {
  GlyphPages pages(ANY360_GLYPHS_DIR, 4);
  TexturePatches patches;
  sys::Glyph answer{sys::ControllerFamily::kPs5, "ps5", "east"};
  uint32_t asked = 0;
  auto glyph = [&](uint32_t user, sys::XboxControl control) {
    ++asked;
    sys::Glyph g = answer;
    if (control == sys::XboxControl::kLeftX) g.control = "left_x";
    if (user == 1) g.control = "";  // nothing bound of the family's kind
    return g;
  };
  GlyphPage page{0x14E1D000, 256, 256, {}};
  page.cells.push_back({0, 0, 32, 32, 0, sys::XboxControl::kA});                                     // 128 atlas
  page.cells.push_back({0, 32, 16, 16, 0, sys::XboxControl::kB});                                    // 64 atlas
  page.cells.push_back({64, 33, 42, 32, 0, sys::XboxControl::kLeftX, GlyphCell::Form::kWholeStick});  // the stick
  page.cells.push_back({100, 100, 32, 32, 1, sys::XboxControl::kA});                                 // unbound: kept
  pages.SetPages({page});
  CHECK(pages.Update(1, glyph, patches));
  auto p = patches.Find(0x14E1D000);
  CHECK(p && p->scale == 4 && p->regions.size() == 3 && pages.last_error().empty());
  if (!pages.last_error().empty()) std::printf("  %s\n", pages.last_error().c_str());
  // The glyph drawn: ps5's east (a circle) from the 128 atlas, at the cell's size: opaque at the centre, clear at
  // the corner; the stick centred in its wider cell with clear sides.
  if (p && p->regions.size() == 3) {
    const auto& a = p->regions[0];
    CHECK(a.pixels.size() == 128 * 128 && (a.pixels[64 * 128 + 64] >> 24) == 0xFF && (a.pixels[0] >> 24) == 0);
    const auto& stick = p->regions[2];
    CHECK(stick.pixels.size() == 168 * 128 && (stick.pixels[64 * 168 + 2] >> 24) == 0 && (stick.pixels[64 * 168 + 84] >> 24) != 0);
  }
  const uint64_t v1 = p ? p->version : 0;
  // Nothing moved: nothing asked, nothing set.
  asked = 0;
  CHECK(!pages.Update(1, glyph, patches) && asked == 0);
  // The generation moved, the answers did not: asked, nothing set.
  CHECK(!pages.Update(2, glyph, patches) && asked == 4 && patches.Find(0x14E1D000)->version == v1);
  // Another family: set again.
  answer = {sys::ControllerFamily::kXbox360, "xbox360", "south"};
  CHECK(pages.Update(3, glyph, patches) && patches.Find(0x14E1D000)->version != v1);
  // A set that does not exist: the cells keep the title's art, and the problem is reported.
  answer = {sys::ControllerFamily::kGeneric, "no_such_set", "south"};
  CHECK(pages.Update(4, glyph, patches) && !patches.Find(0x14E1D000) && !pages.last_error().empty());
  // Pages gone: their patches removed.
  answer = {sys::ControllerFamily::kPs5, "ps5", "east"};
  CHECK(pages.Update(5, glyph, patches) && patches.Find(0x14E1D000));
  pages.SetPages({});
  CHECK(pages.Update(5, glyph, patches) && !patches.Find(0x14E1D000) && patches.empty());
}
#endif

void Bench() {
  const sys::RgbaImage page = Random(256, 256, 1);
  TexturePatch p;
  p.width = p.height = 256, p.scale = 4;
  for (uint32_t i = 0; i < 15; ++i) p.regions.push_back({(i % 8) * 32, (i / 8) * 32, 32, 32, std::vector<uint32_t>(128 * 128, i)});
  sys::RgbaImage out;
  std::vector<sys::RgbaImage> mips;
  ComposePatched(page, p, out);
  const int runs = 20;
  sys::AllocCounts before = sys::ThreadAllocations();
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < runs; ++i) ComposePatched(page, p, out);
  auto t1 = std::chrono::steady_clock::now();
  sys::AllocCounts spent = sys::ThreadAllocations() - before;
  std::printf("bench ComposePatched(256x256 page, 15 regions, 4x -> 1024x1024): %.0f us/call, %llu bytes and %llu "
              "allocations/call (output reused)\n",
              std::chrono::duration<double, std::micro>(t1 - t0).count() / runs, (unsigned long long)(spent.bytes / runs),
              (unsigned long long)(spent.count / runs));
  before = sys::ThreadAllocations();
  t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < runs; ++i) {
    mips.clear();
    BuildMips(out, mips);
  }
  t1 = std::chrono::steady_clock::now();
  spent = sys::ThreadAllocations() - before;
  std::printf("bench BuildMips(1024x1024 -> 10 levels): %.0f us/call, %llu bytes and %llu allocations/call\n",
              std::chrono::duration<double, std::micro>(t1 - t0).count() / runs, (unsigned long long)(spent.bytes / runs),
              (unsigned long long)(spent.count / runs));
}

}  // namespace

int main(int argc, char** argv) {
  RegistryTests();
  ComposeTests();
  MipTests();
  FitTests();
#ifdef ANY360_GLYPHS_DIR
  CoverageTests();
  PagesTests();
#endif
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
  std::printf("gfx_patch_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
