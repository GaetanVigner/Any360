// A sheet of a glyph page as every pad family would see it (for reviewing an adapter's cells and the sets' art
// side by side, docs/GLYPHS.md section 4): the page as the title draws it, enlarged 4x, then the same page patched
// by GlyphPages for each family, through the glyph query with the default bindings. Rows cropped to the cells.
//   gfx_glyph_sheet <page.png> <cells.json> <out.png> [glyphs dir]
// cells.json: [{"x":0,"y":0,"w":32,"h":32,"control":"a","whole":false}, ...] (controls by the bindings file's names).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "any360/gfx/glyph_pages.h"
#include "any360/gfx/png.h"
#include "any360/sys/json.h"

using namespace any360;

namespace {

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: gfx_glyph_sheet <page.png> <cells.json> <out.png> [glyphs dir]\n");
    return 2;
  }
#ifdef ANY360_GLYPHS_DIR
  const std::string dir = argc > 4 ? argv[4] : ANY360_GLYPHS_DIR;
#else
  if (argc < 5) return 2;
  const std::string dir = argv[4];
#endif
  sys::RgbaImage page;
  std::string error;
  if (!sys::DecodePng(ReadAll(argv[1]), page, &error)) {
    std::printf("%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const std::vector<uint8_t> cells_text = ReadAll(argv[2]);
  sys::json::Value cells;
  if (!sys::json::Parse({reinterpret_cast<const char*>(cells_text.data()), cells_text.size()}, cells, &error) ||
      !cells.is(sys::json::Value::Type::kArray)) {
    std::printf("%s: %s\n", argv[2], error.empty() ? "not an array" : error.c_str());
    return 1;
  }
  gfx::GlyphPage glyph_page{0x1000, page.width, page.height, {}};
  uint32_t top = page.height, bottom = 0;
  for (const sys::json::Value& c : cells.array) {
    auto num = [&](const char* k) { const sys::json::Value* v = c.Find(k); return v ? uint32_t(v->number) : 0u; };
    const sys::json::Value* control = c.Find("control");
    const auto parsed = control ? sys::ParseXboxControl(control->string) : std::nullopt;
    if (!parsed) {
      std::printf("%s: unknown control\n", argv[2]);
      return 1;
    }
    const sys::json::Value* whole = c.Find("whole");
    gfx::GlyphCell cell{num("x"), num("y"), num("w"), num("h"), 0, *parsed,
                        whole && whole->boolean ? gfx::GlyphCell::Form::kWholeStick : gfx::GlyphCell::Form::kAsBound};
    top = std::min(top, cell.y);
    bottom = std::max(bottom, cell.y + cell.height);
    glyph_page.cells.push_back(cell);
  }
  if (glyph_page.cells.empty()) return 1;
  const uint32_t scale = 4, band_top = top * scale, band_rows = (bottom - top) * scale, width = page.width * scale;
  const sys::ControllerFamily families[] = {
      sys::ControllerFamily::kXbox360, sys::ControllerFamily::kXboxOne, sys::ControllerFamily::kPs3,
      sys::ControllerFamily::kPs4,     sys::ControllerFamily::kPs5,     sys::ControllerFamily::kSwitch,
      sys::ControllerFamily::kSteamController, sys::ControllerFamily::kSteamDeck};
  std::vector<uint32_t> sheet;
  // Over a checkerboard, as an image viewer would not show transparency.
  auto add_band = [&](const sys::RgbaImage& img) {
    for (uint32_t y = 0; y < band_rows; ++y) {
      const uint32_t* row = img.pixels.data() + size_t(band_top + y) * width;
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t p = row[x], a = p >> 24, bg = ((x / 16 + y / 16) & 1) ? 0xC8u : 0xF0u;
        uint32_t out = 0xFF000000u;
        for (uint32_t s = 0; s < 24; s += 8) out |= (((p >> s & 0xFF) * a + bg * (255 - a)) / 255) << s;
        sheet.push_back(out);
      }
    }
    sheet.insert(sheet.end(), size_t(width) * 4, 0xFF808080u);  // a grey rule between rows
  };
  // The title's own page, enlarged as a patch with no regions would show it.
  sys::RgbaImage out;
  gfx::TexturePatch none;
  none.width = page.width, none.height = page.height, none.scale = scale;
  gfx::ComposePatched(page, none, out);
  add_band(out);
  for (sys::ControllerFamily family : families) {
    sys::GlyphQuery query;
    query.user(0).Connected(1, family);
    gfx::GlyphPages pages(dir, scale);
    pages.SetPages({glyph_page});
    gfx::TexturePatches patches;
    pages.Update(1, [&](uint32_t user, sys::XboxControl control) {
      return query.For(user, control, sys::GlyphQuery::Clock::now());
    }, patches);
    const auto patch = patches.Find(0x1000);
    if (!patch) {
      std::printf("%.*s: nothing patched (%s)\n", int(sys::Name(family).size()), sys::Name(family).data(),
                  pages.last_error().c_str());
      return 1;
    }
    gfx::ComposePatched(page, *patch, out);
    add_band(out);
    std::printf("%.*s: %zu cells\n", int(sys::Name(family).size()), sys::Name(family).data(), patch->regions.size());
  }
  const uint32_t height = uint32_t(sheet.size() / width);
  if (!gfx::WritePng(argv[3], width, height, sheet.data())) return 1;
  std::printf("wrote %s (%ux%u)\n", argv[3], width, height);
  return 0;
}
