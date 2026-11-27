#include "city.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/textures.h"
#include "ucode_encode.h"

namespace any360::bench {

namespace {

using gfx::pm4::Type0;
using gfx::pm4::Type3;
namespace pm4 = gfx::pm4;
namespace t = gfx::test;
namespace uc = rex::graphics::ucode;
using rex::graphics::xenos::VertexFormat;

constexpr uint32_t kIndexOffset = 1024;  // indices after the vertices in the geometry block
constexpr uint32_t kBoxFirst = 0, kGroundFirst = kBoxVertices, kBillboardFirst = kBoxVertices + kQuadVertices;
constexpr uint32_t kBoxIndexFirst = 0, kGroundIndexFirst = kBoxIndices, kBillboardIndexFirst = kBoxIndices + kQuadIndices;
constexpr uint32_t kMainColorTile = 0, kMainDepthTile = 720, kMinimapTile = 1440;
constexpr float kSpacing = 10.0f;

uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
uint32_t Be(uint32_t v) { return std::byteswap(v); }
void StoreBe32(uint8_t* p, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 4);
}
void StoreBe16(uint8_t* p, uint16_t v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 2);
}

struct Mat {
  float m[4][4] = {};
  static Mat Identity() {
    Mat r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
    return r;
  }
  Mat operator*(const Mat& b) const {
    Mat r;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += m[i][k] * b.m[k][j];
        r.m[i][j] = s;
      }
    }
    return r;
  }
  void Apply(const float p[4], float out[4]) const {
    for (int i = 0; i < 4; ++i) out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] * p[3];
  }
  void Store(uint32_t* words) const {
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) words[4 * i + j] = Be(Bits(m[i][j]));
    }
  }
};

Mat World(float x, float y, float z, float sx, float sy, float sz) {
  Mat r;
  r.m[0][0] = sx, r.m[1][1] = sy, r.m[2][2] = sz, r.m[3][3] = 1;
  r.m[0][3] = x, r.m[1][3] = y, r.m[2][3] = z;
  return r;
}

Mat LookAt(const float eye[3], const float at[3]) {
  float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  auto normalize = [](float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; ++i) v[i] /= l;
  };
  normalize(z);
  const float up[3] = {0, 1, 0};
  float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
  normalize(x);
  const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  Mat r;
  const float* axes[3] = {x, y, z};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) r.m[i][j] = axes[i][j];
    r.m[i][3] = -(axes[i][0] * eye[0] + axes[i][1] * eye[1] + axes[i][2] * eye[2]);
  }
  r.m[3][3] = 1;
  return r;
}

Mat Perspective(float fov_y, float aspect, float near_z, float far_z) {
  const float f = 1.0f / std::tan(fov_y / 2), q = far_z / (far_z - near_z);
  Mat r;
  r.m[0][0] = f / aspect, r.m[1][1] = f, r.m[2][2] = q, r.m[2][3] = -q * near_z, r.m[3][2] = 1;
  return r;
}

// A big-endian PM4 stream over guest memory.
struct Pm4 {
  uint8_t* p;
  uint32_t n = 0;
  void Put(uint32_t v) { StoreBe32(p + 4 * n++, v); }
  void Put(std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) Put(w);
  }
  void Raw(const uint32_t* big_endian, uint32_t count) {
    std::memcpy(p + 4 * n, big_endian, 4 * count);
    n += count;
  }
  void Reg(uint32_t index, uint32_t value) { Put({Type0(index, 1), value}); }
  void Shader(uint32_t type, const std::vector<uint32_t>& words) {
    Put({Type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(words.size())), type, uint32_t(words.size())});
    for (uint32_t w : words) Put(w);
  }
};

uint32_t TextureFormat(TextureKind kind) { return kind == TextureKind::k8888 ? 6 : kind == TextureKind::kDxt1 ? 18 : 20; }
uint32_t TextureEndian(TextureKind kind) { return kind == TextureKind::k8888 ? 2 : 1; }

void TextureFetch(Pm4& s, uint32_t base, TextureKind kind, uint32_t size, bool clamp) {
  const uint32_t wrap = clamp ? (2u << 10 | 2u << 13) : 0;
  s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | wrap | (size / 32) << 22 | 1u << 31,
         TextureFormat(kind) | TextureEndian(kind) << 6 | base, (size - 1) | (size - 1) << 13,
         0x688u << 1 | 1u << 19 | 1u << 21, 0, 1u << 9});
}

// The pass's fixed state: surfaces, viewport, scissors, depth and blending.
void PassState(Pm4& s, uint32_t pitch, uint32_t height, uint32_t color_tile, uint32_t depth_tile, bool depth) {
  s.Put({Type0(0x2000, 3), pitch, color_tile, depth_tile});  // RB_SURFACE_INFO, RB_COLOR_INFO (k_8_8_8_8), RB_DEPTH_INFO (24_8)
  s.Reg(0x2208, 4);                                          // RB_MODECONTROL: colour and depth
  s.Put({Type0(0x2100, 4), 0xFFFFFF, 0, 0, 0});
  s.Reg(0x2206, 0x3F | 1u << 10);  // PA_CL_VTE_CNTL
  s.Put({Type0(0x210F, 6), Bits(pitch / 2.0f), Bits(pitch / 2.0f), Bits(-float(height) / 2.0f), Bits(height / 2.0f),
         Bits(1), Bits(0)});
  s.Put({Type0(0x200E, 2), 0, height << 16 | pitch});
  s.Put({Type0(0x2080, 3), 0, 0, height << 16 | pitch});
  // RB_DEPTHCONTROL (z test and write, less-equal), RB_BLENDCONTROL0 (one, zero).
  s.Put({Type0(0x2200, 2), depth ? (1u << 1 | 1u << 2 | 3u << 4) : 0u, 0x00010001});
  s.Put({Type0(0x2209, 3), 0x00010001, 0x00010001, 0x00010001});
  s.Reg(0x2104, 0xF);
  s.Reg(0x2205, 0);
}

void Resolve(Pm4& s, uint32_t rect, uint32_t dest, uint32_t pitch, uint32_t height, bool clear_depth, uint32_t clear_color) {
  s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, rect | 3, 2u | 6u << 2});
  s.Put({Type0(0x2318, 4), 0u | 1u << 8 | (clear_depth ? 1u << 9 : 0u), dest, pitch | height << 16, 2u | 6u << 7});
  s.Reg(0x231E, clear_color);
  s.Reg(0x231D, 0xFFFFFF00);
  s.Reg(0x2208, 6);
  s.Put({Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 8});
  s.Reg(0x2208, 4);
}

void Draw(Pm4& s, const uint32_t* matrix, uint32_t tint, uint32_t index_base, uint32_t count) {
  s.Put({Type3(pm4::SET_CONSTANT, 17), 0u << 16 | 0});
  s.Raw(matrix, kMatrixWords);
  s.Put({Type3(pm4::SET_CONSTANT, 5), 0u << 16 | 1024, Bits((tint & 0xFF) / 255.0f), Bits(((tint >> 8) & 0xFF) / 255.0f),
         Bits(((tint >> 16) & 0xFF) / 255.0f), Bits(1)});
  s.Put({Type3(pm4::DRAW_INDX, 4), 0, 4u | 0u << 6 | count << 16, index_base, count | 1u << 30});
}

// The texel of a facade: a grid of windows, lit or dark by cell, on a wall colour.
uint32_t Facade(uint32_t x, uint32_t y, uint32_t seed) {
  const uint32_t cell = Hash((x / 32) * 977 + (y / 32) * 131 + seed);
  const bool frame = x % 32 < 6 || y % 32 < 8;
  const uint32_t wall = Hash(seed * 7 + 1);
  uint32_t r = 70 + (wall & 63), g = 70 + ((wall >> 8) & 63), b = 80 + ((wall >> 16) & 63);
  if (!frame) {
    const bool lit = (cell & 3) != 0;
    r = lit ? 200 + (cell >> 8 & 55) : 25, g = lit ? 180 + (cell >> 16 & 60) : 30, b = lit ? 90 + (cell >> 24 & 60) : 45;
  }
  const uint32_t noise = Hash(x * 7919 + y * 104729 + seed) & 15;
  return std::min(255u, r + noise) | std::min(255u, g + noise) << 8 | std::min(255u, b + noise) << 16 | 0xFFu << 24;
}

uint16_t To565(uint32_t rgba) {
  return uint16_t(((rgba & 0xFF) >> 3) << 11 | ((rgba >> 8 & 0xFF) >> 2) << 5 | (rgba >> 16 & 0xFF) >> 3);
}

void WriteTexture(uint8_t* physical, uint32_t base, uint32_t texture, uint32_t seed) {
  const TextureKind kind = KindOf(texture);
  uint8_t* out = physical + base;
  if (kind == TextureKind::k8888) {
    for (uint32_t y = 0; y < kTextureSize; ++y) {
      for (uint32_t x = 0; x < kTextureSize; ++x) {
        const uint32_t at = uint32_t(gfx::vk::TiledOffset2D(int32_t(x), int32_t(y), kTextureSize, 2));
        StoreBe32(out + at, Facade(x, y, seed));
      }
    }
    return;
  }
  const uint32_t blocks = kTextureSize / 4, block_bytes = kind == TextureKind::kDxt1 ? 8 : 16;
  for (uint32_t by = 0; by < blocks; ++by) {
    for (uint32_t bx = 0; bx < blocks; ++bx) {
      // Endpoints from the facade at the block's corners, indices from a hash: a PC-order block, then each 16-bit
      // word swapped (the 8-in-16 endianness titles store DXT blocks in).
      uint8_t block[16] = {};
      uint8_t* color = block;
      if (kind == TextureKind::kDxt5) {
        const uint32_t h = Hash(bx * 31 + by * 17 + seed);
        block[0] = uint8_t(255 - (by * 3)), block[1] = uint8_t(h & 0x7F);
        for (int i = 2; i < 8; ++i) block[i] = uint8_t(Hash(h + i));
        color = block + 8;
      }
      uint16_t c0 = To565(Facade(bx * 4, by * 4, seed)), c1 = To565(Facade(bx * 4 + 3, by * 4 + 3, seed ^ 0x55));
      if (c0 < c1) std::swap(c0, c1);
      const uint32_t indices = Hash(bx * 131 + by * 7 + seed * 3);
      color[0] = uint8_t(c0), color[1] = uint8_t(c0 >> 8), color[2] = uint8_t(c1), color[3] = uint8_t(c1 >> 8);
      for (int i = 0; i < 4; ++i) color[4 + i] = uint8_t(indices >> (8 * i));
      for (uint32_t i = 0; i < block_bytes; i += 2) std::swap(block[i], block[i + 1]);
      const uint32_t at = uint32_t(gfx::vk::TiledOffset2D(int32_t(bx), int32_t(by), blocks, kind == TextureKind::kDxt1 ? 3 : 4));
      std::memcpy(out + at, block, block_bytes);
    }
  }
}

}  // namespace

TextureKind KindOf(uint32_t texture) {
  return texture < 8 ? TextureKind::kDxt1 : texture < 12 ? TextureKind::kDxt5 : TextureKind::k8888;
}

uint32_t TextureBytes(uint32_t texture) {
  const uint32_t texels = kTextureSize * kTextureSize;
  switch (KindOf(texture)) {
    case TextureKind::kDxt1: return texels / 2;
    case TextureKind::kDxt5: return texels;
    default: return texels * 4;
  }
}

uint32_t GeometryBytes() { return 4096; }

City::City(const CityOptions& options) : options_(options) {
  grid_ = std::max(1u, uint32_t(std::ceil(std::sqrt(double(options_.objects)))));
  {
    std::vector<uint32_t>& w = vs_;
    t::PushCfPair(w, t::CfAlloc(uc::AllocType::kVsPosition, 0), t::CfExec(uc::ControlFlowOpcode::kExec, 2, 6, 0b0101));
    t::PushCfPair(w, t::CfAlloc(uc::AllocType::kVsInterpolators, 0), t::CfExec(uc::ControlFlowOpcode::kExecEnd, 8, 1, 0));
    t::PushVertexFetch(w, 1, 0, 0, VertexFormat::k_32_32_32_32_FLOAT, kVertexFloats, 0);
    t::PushVertexFetch(w, 2, 0, 0, VertexFormat::k_32_32_FLOAT, kVertexFloats, 4);
    for (uint32_t i = 0; i < 4; ++i) {
      t::Alu a;
      a.vector = uc::AluVectorOpcode::kDp4;
      a.vector_mask = 1u << i;
      a.dest = uint32_t(uc::ExportRegister::kVSPosition);
      a.is_export = true;
      a.src1 = {1, true};
      a.src2 = {i, false};
      t::PushAlu(w, a);
    }
    t::PushAluVector(w, uc::AluVectorOpcode::kMax, uint32_t(uc::ExportRegister::kVSInterpolator0), true, 0b1111, {2, true},
                     {2, true});
  }
  {
    std::vector<uint32_t>& w = ps_tint_;
    t::PushCfPair(w, t::CfAlloc(uc::AllocType::kPsColors, 0), t::CfExec(uc::ControlFlowOpcode::kExecEnd, 1, 2, 0b0001));
    t::PushTextureFetch2D(w, 1, 0, 1);
    t::PushAluVector(w, uc::AluVectorOpcode::kMul, uint32_t(uc::ExportRegister::kPSColor0), true, 0b1111, {1, true},
                     {0, false});
  }
  ps_plain_ = t::BuildTexturedPixelShader(1);
}

void City::Build(const CityMemory& m) const {
  uint8_t* geometry = m.physical + m.geometry;
  // Vertices: the unit box (x, z in [-0.5, 0.5], y in [0, 1]) face by face, the ground quad (xz), the billboard
  // quad (xy, y in [0, 1]).
  struct V {
    float x, y, z, u, v;
  };
  std::vector<V> vertices;
  const float faces[6][4][3] = {
      {{0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {0.5f, 1, 0.5f}, {0.5f, 1, -0.5f}},
      {{-0.5f, 0, 0.5f}, {-0.5f, 0, -0.5f}, {-0.5f, 1, -0.5f}, {-0.5f, 1, 0.5f}},
      {{0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}, {-0.5f, 1, 0.5f}, {0.5f, 1, 0.5f}},
      {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 1, -0.5f}, {-0.5f, 1, -0.5f}},
      {{-0.5f, 1, -0.5f}, {0.5f, 1, -0.5f}, {0.5f, 1, 0.5f}, {-0.5f, 1, 0.5f}},
      {{-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}}};
  const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  for (const auto& face : faces) {
    for (int i = 0; i < 4; ++i) vertices.push_back({face[i][0], face[i][1], face[i][2], uv[i][0], uv[i][1]});
  }
  const float ground[4][3] = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}};
  for (int i = 0; i < 4; ++i) vertices.push_back({ground[i][0], ground[i][1], ground[i][2], uv[i][0] * 16, uv[i][1] * 16});
  const float board[4][3] = {{-0.5f, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {-0.5f, 1, 0}};
  for (int i = 0; i < 4; ++i) vertices.push_back({board[i][0], board[i][1], board[i][2], uv[i][0], uv[i][1]});
  for (size_t i = 0; i < vertices.size(); ++i) {
    const V& v = vertices[i];
    const float f[kVertexFloats] = {v.x, v.y, v.z, 1, v.u, v.v};
    for (uint32_t c = 0; c < kVertexFloats; ++c) StoreBe32(geometry + 4 * (kVertexFloats * i + c), Bits(f[c]));
  }
  uint8_t* indices = geometry + kIndexOffset;
  uint32_t n = 0;
  for (uint16_t f = 0; f < 6; ++f) {
    for (uint16_t i : {0, 1, 2, 0, 2, 3}) StoreBe16(indices + 2 * n++, uint16_t(kBoxFirst + 4 * f + i));
  }
  for (uint16_t i : {0, 1, 2, 0, 2, 3}) StoreBe16(indices + 2 * n++, uint16_t(kGroundFirst + i));
  for (uint16_t i : {0, 1, 2, 0, 2, 3}) StoreBe16(indices + 2 * n++, uint16_t(kBillboardFirst + i));
  for (uint32_t i = 0; i < kTextureCount; ++i) WriteTexture(m.physical, m.textures[i], i, i * 1013 + 7);
  const float rects[2][6] = {{0, 0, float(options_.width), 0, float(options_.width), float(options_.height)},
                             {0, 0, float(kMinimapSize), 0, float(kMinimapSize), float(kMinimapSize)}};
  for (uint32_t r = 0; r < 2; ++r) {
    for (uint32_t i = 0; i < 6; ++i) StoreBe32(m.physical + m.rects + 32 * r + 4 * i, Bits(rects[r][i]));
  }
}

void City::Animate(uint32_t frame, uint32_t begin, uint32_t end, ObjectSlot* slots) const {
  const float extent = grid_ * kSpacing, angle = float(frame) * 0.003f, radius = extent * 0.55f;
  const float eye[3] = {std::cos(angle) * radius, 60.0f + 30.0f * std::sin(angle * 2.3f), std::sin(angle) * radius};
  const float at[3] = {0, 10, 0};
  const Mat view_projection =
      Perspective(1.0f, float(options_.width) / float(options_.height), 1.0f, 4.0f * extent) * LookAt(eye, at);
  Mat top = Mat::Identity();
  const float half = extent / 2;
  top.m[0][0] = 1 / half;
  top.m[1][1] = 0, top.m[1][2] = 1 / half;
  top.m[2][2] = 0, top.m[2][1] = 1.0f / 400, top.m[2][3] = 0.5f;
  for (uint32_t i = begin; i < end; ++i) {
    ObjectSlot& s = slots[i];
    const uint32_t h = Hash(i * 2654435761u + 17);
    const float x = (float(i % grid_) - grid_ / 2.0f) * kSpacing, z = (float(i / grid_) - grid_ / 2.0f) * kSpacing;
    const float w = 4.0f + float(h & 3), height = 4.0f + float((h >> 4) % 40);
    const Mat world = World(x, 0, z, w, height, w);
    const Mat main = view_projection * world;
    const float center[4] = {0, 0.5f, 0, 1};
    float c[4];
    main.Apply(center, c);
    const bool visible = c[3] > 1.0f && std::fabs(c[0]) <= 1.2f * c[3] + 30 && std::fabs(c[1]) <= 1.2f * c[3] + 30;
    s.visible = Be(visible ? 1 : 0);
    s.texture = Be((h >> 10) % kTextureCount);
    s.tint = Be(0xFF000000u | (160 + (h >> 14 & 95)) | (160 + (h >> 19 & 95)) << 8 | (160 + (h >> 24 & 95)) << 16);
    main.Store(s.main);
    (top * world).Store(s.minimap);
  }
}

void City::Stream(uint32_t frame, const CityMemory& m, std::vector<uint32_t>& written) const {
  const uint32_t dynamic = 12 + frame % 4;
  WriteTexture(m.physical, m.textures[dynamic], dynamic, dynamic * 1013 + 7 + frame);
  written.push_back(m.textures[dynamic]);
  written.push_back(m.textures[dynamic] + TextureBytes(dynamic));
  if (frame % 60 == 30) {
    const uint32_t compressed = (frame / 60) % 8;
    WriteTexture(m.physical, m.textures[compressed], compressed, compressed * 1013 + 7 + frame);
    written.push_back(m.textures[compressed]);
    written.push_back(m.textures[compressed] + TextureBytes(compressed));
  }
}

uint32_t City::MaxFrameDwords() const { return (2 * options_.objects + 8) * 40 + 8192; }

uint32_t City::WriteFrame(uint32_t frame, const CityMemory& m, const ObjectSlot* slots, uint8_t* out) const {
  Pm4 s{out};
  const uint32_t box_indices = m.geometry + kIndexOffset + 2 * kBoxIndexFirst;
  auto vertices = [&] { s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, m.geometry | 3, 2u | (32 * kVertexFloats) << 2}); };
  // The minimap: the first objects seen from above into a 256x256 target, resolved to a texture.
  PassState(s, kMinimapSize, kMinimapSize, kMinimapTile, kMainDepthTile, false);
  s.Shader(0, vs_);
  s.Shader(1, ps_tint_);
  vertices();
  const uint32_t stride = std::max(1u, options_.objects / kMinimapObjects);
  for (uint32_t texture = 0; texture < kTextureCount; ++texture) {
    bool bound = false;
    for (uint32_t i = 0; i < options_.objects; i += stride) {
      if (Be(slots[i].texture) != texture) continue;
      if (!bound) TextureFetch(s, m.textures[texture], KindOf(texture), kTextureSize, false), bound = true;
      Draw(s, slots[i].minimap, Be(slots[i].tint), box_indices, kBoxIndices);
    }
  }
  Resolve(s, m.rects + 32, m.minimap, kMinimapSize, kMinimapSize, false, 0xFF402010);
  // The main view: ground, buildings by texture, the billboard showing the minimap; resolved to the front buffer.
  const uint32_t width = options_.width, height = options_.height;
  PassState(s, width, height, kMainColorTile, kMainDepthTile, true);
  vertices();
  {
    const float extent = grid_ * kSpacing, angle = float(frame) * 0.003f, radius = extent * 0.55f;
    const float eye[3] = {std::cos(angle) * radius, 60.0f + 30.0f * std::sin(angle * 2.3f), std::sin(angle) * radius};
    const float at[3] = {0, 10, 0};
    const Mat vp = Perspective(1.0f, float(width) / float(height), 1.0f, 4.0f * extent) * LookAt(eye, at);
    uint32_t ground[kMatrixWords], board[kMatrixWords];
    (vp * World(0, 0, 0, extent * 1.5f, 1, extent * 1.5f)).Store(ground);
    (vp * World(0, 70, 0, 80, 80, 1)).Store(board);
    s.Shader(1, ps_plain_);
    TextureFetch(s, m.textures[0], KindOf(0), kTextureSize, false);
    Draw(s, ground, 0xFFFFFFFF, m.geometry + kIndexOffset + 2 * kGroundIndexFirst, kQuadIndices);
    TextureFetch(s, m.minimap, TextureKind::k8888, kMinimapSize, true);
    Draw(s, board, 0xFFFFFFFF, m.geometry + kIndexOffset + 2 * kBillboardIndexFirst, kQuadIndices);
  }
  s.Shader(1, ps_tint_);
  for (uint32_t texture = 0; texture < kTextureCount; ++texture) {
    bool bound = false;
    for (uint32_t i = 0; i < options_.objects; ++i) {
      if (!slots[i].visible || Be(slots[i].texture) != texture) continue;
      if (!bound) TextureFetch(s, m.textures[texture], KindOf(texture), kTextureSize, false), bound = true;
      Draw(s, slots[i].main, Be(slots[i].tint), box_indices, kBoxIndices);
    }
  }
  Resolve(s, m.rects, m.front, width, height, true, 0xFF9A7A50);
  return s.n;
}

void City::FrontBufferFetch(uint32_t front_virtual, uint32_t out[6]) const {
  const uint32_t w = options_.width, h = options_.height;
  out[0] = 2u | 2u << 10 | 2u << 13 | (w / 32) << 22 | 1u << 31;
  out[1] = 6u | 2u << 6 | front_virtual;
  out[2] = (w - 1) | (h - 1) << 13;
  out[3] = 0x688u << 1;
  out[4] = 0;
  out[5] = 1u << 9;
}

void City::Mix(uint64_t first, uint8_t* out) {
  static const float kScale[5] = {261.63f, 293.66f, 329.63f, 392.00f, 440.00f};
  constexpr uint32_t kNote = 12000;  // samples a note at 48 kHz
  for (uint32_t j = 0; j < 256; ++j) {
    const uint64_t sample = first + j;
    const uint32_t note = uint32_t(sample / kNote), into = uint32_t(sample % kNote);
    const float envelope = std::exp(-float(into) / 4000.0f);
    const float bass = kScale[Hash(note / 4) % 5] / 4, lead = kScale[Hash(note) % 5] * float(1 + (note & 1));
    const double seconds = double(sample) / 48000.0;
    const float a = 0.25f * envelope * float(std::sin(6.283185307179586 * lead * seconds));
    const float b = 0.15f * float(std::sin(6.283185307179586 * bass * seconds));
    const float pan = 0.5f + 0.4f * float(std::sin(double(note) * 0.7));
    const float channels[6] = {a * (1 - pan) + b, a * pan + b, 0.5f * a, b, 0.2f * a, 0.2f * a};
    for (uint32_t c = 0; c < 6; ++c) StoreBe32(out + 4 * (c * 256 + j), Bits(channels[c]));
  }
}

}  // namespace any360::bench
