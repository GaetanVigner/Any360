// The city scene's content and command stream: procedural geometry, textures and shaders, and the PM4 of a frame the
// way an XDK D3D runtime writes it (registers, constants, fetch constants, indexed draws, resolves). Pure data
// generation over guest memory: no thread, no system call, no GPU. The title code (title.cpp) owns the memory and
// decides when each piece runs.
#pragma once

#include <cstdint>
#include <vector>

namespace any360::bench {

constexpr uint32_t kTextureSize = 256, kMinimapSize = 256, kTextureCount = 16;
constexpr uint32_t kBoxVertices = 24, kBoxIndices = 36, kQuadVertices = 4, kQuadIndices = 6;
constexpr uint32_t kVertexFloats = 6;  // position xyzw, texcoord uv
constexpr uint32_t kMatrixWords = 16;
constexpr uint32_t kMinimapObjects = 512;

enum class TextureKind : uint32_t { k8888, kDxt1, kDxt5 };
TextureKind KindOf(uint32_t texture);
uint32_t TextureBytes(uint32_t texture);

// Guest memory the title allocated for the scene: physical addresses (the GPU's) and the host pointer of physical 0.
struct CityMemory {
  uint8_t* physical = nullptr;
  uint32_t geometry = 0;  // box, ground quad, billboard quad vertices, then their indices
  uint32_t textures[kTextureCount] = {};
  uint32_t minimap = 0;  // the minimap pass's resolve destination, sampled by the billboard
  uint32_t front = 0;    // the front buffer
  uint32_t rects = 0;    // the resolve rectangles (main, minimap)
};
uint32_t GeometryBytes();
constexpr uint32_t kRectBytes = 64;

struct CityOptions {
  uint32_t objects = 2000;
  uint32_t width = 1280, height = 720;
};

// Per object, written by the workers each frame: visible (main view), the main and minimap matrices (rows of
// clip = M * position), big-endian words as a title keeps them, ready to copy into SET_CONSTANT packets.
struct ObjectSlot {
  uint32_t visible;  // big-endian 0/1
  uint32_t texture;  // big-endian
  uint32_t tint;     // big-endian packed RGBA8, expanded by the render thread
  uint32_t pad;
  uint32_t main[kMatrixWords];
  uint32_t minimap[kMatrixWords];
};
static_assert(sizeof(ObjectSlot) == 144);

class City {
 public:
  explicit City(const CityOptions& options);
  const CityOptions& options() const { return options_; }

  // Writes the meshes, every texture and the resolve rectangles into guest memory.
  void Build(const CityMemory& m) const;
  // Objects [begin, end) for `frame`: visibility and matrices into `slots` (host pointer to guest memory).
  void Animate(uint32_t frame, uint32_t begin, uint32_t end, ObjectSlot* slots) const;
  // The CPU's texture writes of `frame` (a dynamic texture every frame, a compressed one every 60): the byte ranges
  // written, as physical [begin, end) pairs.
  void Stream(uint32_t frame, const CityMemory& m, std::vector<uint32_t>& written) const;
  // The frame's PM4 into `out` (host pointer to the command buffer, big-endian dwords); returns its dwords. `slots`
  // as Animate left them.
  uint32_t WriteFrame(uint32_t frame, const CityMemory& m, const ObjectSlot* slots, uint8_t* out) const;
  // An upper bound of WriteFrame's dwords.
  uint32_t MaxFrameDwords() const;
  // The swap's front buffer fetch constant (6 dwords, the front buffer's virtual address), as a title gives VdSwap.
  void FrontBufferFetch(uint32_t front_virtual, uint32_t out[6]) const;

  // Audio: 256 samples of 6 channels (channel after channel, big-endian floats) starting at sample `first`.
  static void Mix(uint64_t first, uint8_t* out);

 private:
  CityOptions options_;
  uint32_t grid_ = 0;
  std::vector<uint32_t> vs_, ps_tint_, ps_plain_;
};

}  // namespace any360::bench
