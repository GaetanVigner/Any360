#include "any360/gfx/frame_counter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace any360::gfx {

FrameTimeSummary SummarizeFrameTimes(std::span<float> ms) {
  FrameTimeSummary s;
  if (ms.empty()) return s;
  std::sort(ms.begin(), ms.end());
  const auto at = [&](double q) { return double(ms[size_t(q * double(ms.size() - 1) + 0.5)]); };
  s.frames = uint32_t(ms.size());
  s.median_ms = at(0.5), s.p90_ms = at(0.9), s.p99_ms = at(0.99);
  return s;
}

FrameTimeSummary FrameTimeWindow::Summary() {
  std::copy_n(ring_.begin(), count_, scratch_.begin());  // the ring's order does not matter: they are sorted
  return SummarizeFrameTimes(std::span<float>(scratch_.data(), count_));
}

std::string FormatOverlay(const OverlayFigures& f) {
  char text[192];
  int n = std::snprintf(text, sizeof(text), "%.0f FPS %.1f ms (max %.1f)", f.fps, f.frame_ms, f.max_ms);
  const FrameTimeSummary& w = f.window;
  if (w.frames && w.median_ms > 0 && w.p90_ms > 0 && w.p99_ms > 0 && n > 0 && size_t(n) < sizeof(text)) {
    n += std::snprintf(text + n, sizeof(text) - size_t(n), "\nmedian %.1f ms, 10%% low %.0f, 1%% low %.0f FPS",
                       w.median_ms, 1e3 / w.p90_ms, 1e3 / w.p99_ms);
  }
  if ((f.gpu_ms >= 0 || f.cp_idle_pct >= 0) && n > 0 && size_t(n) < sizeof(text)) {
    n += std::snprintf(text + n, sizeof(text) - size_t(n), "\n");
    if (f.gpu_ms >= 0 && size_t(n) < sizeof(text)) {
      n += std::snprintf(text + n, sizeof(text) - size_t(n), "GPU %.1f ms%s", f.gpu_ms, f.cp_idle_pct >= 0 ? " " : "");
    }
    if (f.cp_idle_pct >= 0 && size_t(n) < sizeof(text)) {
      std::snprintf(text + n, sizeof(text) - size_t(n), "CP idle %.0f%%", f.cp_idle_pct);
    }
  }
  return text;
}

namespace {
// 5 x 7 glyphs, a row a byte (bit 4 the leftmost pixel).
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}}, {'d', {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F}},
    {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}}, {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}},
    {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}}, {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'s', {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}}, {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},
    {'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
};
const Glyph* Find(char c) {
  for (const Glyph& g : kFont) {
    if (g.c == c) return &g;
  }
  return nullptr;
}
}  // namespace

bool OverlayHasGlyph(char c) { return c == ' ' || c == '\n' || Find(c); }

OverlaySize MeasureOverlay(std::string_view text, uint32_t scale) {
  if (text.empty()) return {};
  size_t widest = 0, line = 0, lines = 1;
  for (char c : text) {
    if (c == '\n') {
      ++lines, line = 0;
    } else {
      widest = std::max(widest, ++line);
    }
  }
  return {uint32_t(widest * 6 + 2) * scale, uint32_t(1 + 8 * lines) * scale};
}

void RasterizeOverlay(std::string_view text, uint32_t scale, uint32_t ink, uint32_t paper, uint32_t* out, uint32_t width,
                      uint32_t height) {
  std::fill(out, out + size_t(width) * height, paper);
  uint32_t column = 0, line = 0;
  for (char c : text) {
    if (c == '\n') {
      ++line, column = 0;
      continue;
    }
    const Glyph* glyph = Find(c);
    const uint32_t cell = column++;
    if (!glyph) continue;
    for (uint32_t row = 0; row < 7; ++row) {
      for (uint32_t col = 0; col < 5; ++col) {
        if (!(glyph->rows[row] >> (4 - col) & 1)) continue;
        const uint32_t x0 = (1 + cell * 6 + col) * scale, y0 = (1 + line * 8 + row) * scale;
        for (uint32_t y = y0; y < y0 + scale && y < height; ++y) {
          for (uint32_t x = x0; x < x0 + scale && x < width; ++x) out[size_t(y) * width + x] = ink;
        }
      }
    }
  }
}

void CompositeOverlayRgba(std::string_view text, uint32_t* pixels, uint32_t width, uint32_t height) {
  const uint32_t scale = OverlayScale(height);
  const OverlaySize size = MeasureOverlay(text, scale);
  const uint32_t w = std::min(size.width, width), h = std::min(size.height, height);
  if (!w || !h) return;
  // Rasterized into a band of its own (reused), then copied row by row: the image's rows are `width` apart.
  thread_local std::vector<uint32_t> band;
  band.resize(size_t(w) * h);
  RasterizeOverlay(text, scale, OverlayInk(false), kOverlayPaper, band.data(), w, h);
  for (uint32_t y = 0; y < h; ++y) std::copy_n(&band[size_t(y) * w], w, &pixels[size_t(y) * width]);
}

}  // namespace any360::gfx
