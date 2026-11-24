// Glyph pages (docs/GLYPHS.md, section 4): the pad glyphs a title keeps as cells of its own texture pages (a button
// font's pages, an atlas of prompts) redrawn from Any360's glyph sets (assets/glyphs) for each player's bindings and
// device, as texture patches (texture_patches.h). The title adapter names the pages (their base addresses, read from
// the title's objects) and what each cell shows (a control of the title's controller, for a user); this answers which
// glyph through the glyph query, fits its art inside the cell at the patch's scale and sets the patches again whenever
// an answer changes.
//
//   GlyphPages pages(assets_dir);
//   pages.SetPages({{base, 256, 256, {{0, 0, 32, 32, 0, XboxControl::kA}, ...}}});
//   pages.Update(PlayerGlyphGeneration(), PlayerGlyph, TexturePatches::Global());   // each frame: cheap when unchanged
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "any360/gfx/texture_patches.h"
#include "any360/sys/input_glyphs.h"
#include "any360/sys/png.h"

namespace any360::gfx {

struct GlyphCell {
  uint32_t x = 0, y = 0, width = 0, height = 0;  // the cell, guest texels of its page
  uint32_t user = 0;                             // whose bindings and device the glyph follows
  sys::XboxControl control = sys::XboxControl::kA;
  // kAsBound: the glyph of what the control is bound to; kWholeStick: the whole stick (or pad) that control is on (a
  // title's "move" prompt drawn for a stick's axis).
  enum class Form : uint8_t { kAsBound, kWholeStick } form = Form::kAsBound;
};

struct GlyphPage {
  uint32_t base = 0;               // the page texture's base address
  uint32_t width = 0, height = 0;  // the page's size, texels
  std::vector<GlyphCell> cells;
};

// The whole stick a control is on, as the sets draw it: "left_x+", "left_y", "left_stick" -> "left_x" (the stick with no
// click mark; "left_stick" is the click, L3 on a PlayStation pad); "right_..." -> "right_x"; "dpad_..." -> "dpad";
// others unchanged.
std::string_view WholeStick(std::string_view control);

// Draws the art of `src`'s square cell (x, y, size) fitted in the rectangle (x, y, width, height) of `dst`, centred,
// aspect kept, resampled by area (alpha-weighted, so edges do not darken); the rest of the rectangle transparent.
void FitGlyph(const sys::RgbaImage& src, uint32_t sx, uint32_t sy, uint32_t size, uint32_t* dst, uint32_t dst_stride,
              uint32_t width, uint32_t height);

class GlyphPages {
 public:
  using GlyphFn = std::function<sys::Glyph(uint32_t, sys::XboxControl)>;
  // `glyphs_dir`: assets/glyphs (<set>.json, <set>_128.png, <set>_64.png). `scale`: host texels a guest texel.
  explicit GlyphPages(std::string glyphs_dir, uint32_t scale = 4);

  // The pages to patch (replacing the previous list; pages no longer listed lose their patch at the next Update).
  void SetPages(std::vector<GlyphPage> pages);
  // Sets the patches again when the pages changed or `generation` moved since the last call and an answer of `glyph`
  // changed; true when it set any. A cell whose glyph has no art (nothing bound of the family's kind, a set or name
  // missing) keeps the title's own.
  bool Update(uint32_t generation, const GlyphFn& glyph, TexturePatches& patches);

  // Diagnostics: the last problem met (a set that would not load), and patches set so far.
  const std::string& last_error() const { return last_error_; }
  uint32_t patches_set() const { return patches_set_; }

 private:
  struct Set {
    bool loaded = false, failed = false;
    sys::RgbaImage atlas128, atlas64;
    uint32_t cell128 = 128, cell64 = 64;
    std::map<std::string, std::pair<uint32_t, uint32_t>, std::less<>> cells;  // control name -> (column, row)
  };
  Set* LoadSet(std::string_view name);

  std::string dir_;
  uint32_t scale_;
  std::vector<GlyphPage> pages_;
  std::vector<uint32_t> patched_bases_;
  bool pages_changed_ = false;
  bool updated_once_ = false;
  uint32_t generation_ = 0;
  std::vector<std::string> answers_;  // "set/control" of each cell at the last patch, in page order
  std::map<std::string, Set, std::less<>> sets_;
  std::string last_error_;
  uint32_t patches_set_ = 0;
};

}  // namespace any360::gfx
