// The frame counter Any360 draws over any title's picture (--any360_fps_overlay), and the frame time statistics it
// shares with the binding's periodic report ("frame times: median ..., 10% low ..., 1% low ..."). No Vulkan here: the
// figures, the text and its bitmap are plain data, drawn by vk::Presenter onto the swapchain image and composited
// into screenshots; tested in gfx_overlay_tests.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace any360::gfx {

// The median frame time and the 90th / 99th percentile ones: their frame rates are the "10% low" and "1% low" (the
// stutter an average hides). Percentiles by nearest rank on the sorted times, index q * (n - 1) rounded.
struct FrameTimeSummary {
  uint32_t frames = 0;
  double median_ms = 0, p90_ms = 0, p99_ms = 0;
};
// Sorts `ms` in place. Empty: frames 0, the rest 0.
FrameTimeSummary SummarizeFrameTimes(std::span<float> ms);

// The last kWindow frame times (a fixed ring: no allocation per frame), for the overlay's median and lows over the
// last seconds rather than over its half-second refresh (too few frames for a 1% low).
class FrameTimeWindow {
 public:
  static constexpr uint32_t kWindow = 512;
  void Add(float ms) {
    ring_[next_] = ms;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
  }
  uint32_t count() const { return count_; }
  void Clear() { next_ = count_ = 0; }
  // Over the times held (copied to a scratch array of its own: the ring keeps its order).
  FrameTimeSummary Summary();

 private:
  std::array<float, kWindow> ring_{}, scratch_{};
  uint32_t next_ = 0, count_ = 0;
};

// What the overlay shows, refreshed twice a second: the frame rate and mean frame time of the last half second with
// its slowest frame, the median and lows of the window, and (when known) the GPU's time a frame and the command
// processor's idle share.
struct OverlayFigures {
  double fps = 0, frame_ms = 0, max_ms = 0;
  FrameTimeSummary window;
  double gpu_ms = -1;       // < 0: not shown
  double cp_idle_pct = -1;  // < 0: not shown
};
// The overlay's text, a line per '\n' (every character in the overlay font: OverlayHasGlyph).
std::string FormatOverlay(const OverlayFigures& figures);

// The overlay's bitmap: a 5 x 7 font in cells of 6 x 8 font pixels, a font pixel of border, each font pixel `scale`
// output pixels, ink on a paper band. Characters outside the font are left blank (OverlayHasGlyph tells).
bool OverlayHasGlyph(char c);
// The font scale for an image `height` pixels tall: one font pixel per 540 lines, 1 to 4 (1 at 720p, 2 at 1080p and
// 1440p, 4 at 2160p: the band under 5% of the picture's height and about a quarter of its width).
inline uint32_t OverlayScale(uint32_t height) {
  const uint32_t s = height / 540;
  return s < 1 ? 1 : s > 4 ? 4 : s;
}
struct OverlaySize {
  uint32_t width = 0, height = 0;
};
// The band's size for `text` at `scale` (the widest line), before any clipping.
OverlaySize MeasureOverlay(std::string_view text, uint32_t scale);
// Writes the band into `out` (width x height, row-major, `width` pixels a row), clipped to that size: `paper`
// everywhere, `ink` where the glyphs are. The pixel values are written as given (the caller picks the byte order).
void RasterizeOverlay(std::string_view text, uint32_t scale, uint32_t ink, uint32_t paper, uint32_t* out, uint32_t width,
                      uint32_t height);
// The overlay's colours: yellow ink on dark grey paper, in RGBA8 (R in the low byte) or BGRA8 (B in the low byte).
inline uint32_t OverlayInk(bool bgra) { return bgra ? 0xFFFFFF40u : 0xFF40FFFFu; }
constexpr uint32_t kOverlayPaper = 0xFF202020u;
// Draws `text` over the top-left corner of an RGBA8 image (R in the low byte), at the scale for its height, as the
// presenter does on the window: for screenshots, which read the renderer's frame and not the swapchain's.
void CompositeOverlayRgba(std::string_view text, uint32_t* pixels, uint32_t width, uint32_t height);

}  // namespace any360::gfx
