// Scenario tests of the command processor against the PM4 semantics: packets written by hand into a fake physical
// memory, executed synchronously (Execute) or by the worker thread, the effects read back from the register file,
// memory and a recording renderer.
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "any360/sys/graphics.h"

namespace {

using namespace any360;
using namespace any360::gfx;
using pm4::Type0;
using pm4::Type1;
using pm4::Type2;
using pm4::Type3;

int g_failures = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

std::vector<uint8_t> g_physical(0x200000);
constexpr uint32_t kRing = 0x10000, kRingLog2 = 10, kWriteBack = 0x8000, kData = 0x20000, kIb = 0x30000;  // 8 KB ring

uint32_t Big(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, g_physical.data() + address, 4);
  return std::byteswap(v);
}
uint32_t Raw(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, g_physical.data() + address, 4);
  return v;
}
void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_physical.data() + address, &value, 4);
}

struct Recorder : Renderer {
  std::vector<DrawCall> draws;
  int copies = 0, swaps = 0, shaders = 0, invalidations = 0;
  uint32_t frontbuffer = 0, width = 0, height = 0, invalid_base = 0, invalid_size = 0;
  uint64_t last_hash = 0;
  void ShaderLoaded(const Shader& s) override { ++shaders, last_hash = s.hash; }
  void Draw(const DrawCall& d) override { draws.push_back(d); }
  void Copy(const uint32_t*) override { ++copies; }
  void Swap(uint32_t f, uint32_t w, uint32_t h, const uint32_t*) override { ++swaps, frontbuffer = f, width = w, height = h; }
  void Invalidate(uint32_t b, uint32_t s) override { ++invalidations, invalid_base = b, invalid_size = s; }
};

// A ring writer: packets appended at the write index, wrapping, then CP_RB_WPTR kicks.
struct Ring {
  CommandProcessor& cp;
  uint32_t write = 0;
  void Put(uint32_t dword) {
    SetBig(kRing + 4 * write, dword);
    write = (write + 1) % cp.ring_dwords();
  }
  void Put(std::initializer_list<uint32_t> dwords) {
    for (uint32_t d : dwords) Put(d);
  }
  void Kick() { cp.WriteRegister(reg::CP_RB_WPTR, write); }
  void Run() {
    Kick();
    CHECK(cp.Execute());
  }
};

void Fresh(CommandProcessor& cp, Ring& ring) {
  std::memset(g_physical.data(), 0, g_physical.size());
  cp.InitializeRingBuffer({}, kRing, kRingLog2);
  cp.EnableReadPointerWriteBack({}, kWriteBack, 6);
  ring.write = 0;
}

void RingAndRegisters() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Fresh(cp, ring);
  CHECK(cp.ring_dwords() == 2048);
  CHECK(cp.reg(reg::CP_RB_BASE) == kRing && cp.reg(reg::CP_RB_CNTL) == kRingLog2 && cp.reg(reg::CP_RB_RPTR_ADDR) == kWriteBack);
  // Type 0: a run of registers, then one register written three times (the last value stays); type 1; type 2.
  ring.Put({Type0(0x2000, 3), 0x11, 0x22, 0x33, Type0(0x2100, 3, true), 1, 2, 3, Type1(0x100, 0x200), 0xAA, 0xBB, Type2()});
  // Nothing runs before the write pointer moves.
  CHECK(cp.Execute() && cp.reg(0x2000) == 0);
  ring.Run();
  CHECK(cp.reg(0x2000) == 0x11 && cp.reg(0x2001) == 0x22 && cp.reg(0x2002) == 0x33);
  CHECK(cp.reg(0x2100) == 3 && cp.reg(0x2101) == 0);
  CHECK(cp.reg(0x100) == 0xAA && cp.reg(0x200) == 0xBB);
  // The read pointer: in the register, through MMIO, and written back big-endian.
  CHECK(cp.read_pointer() == 12 && cp.ReadRegister(reg::CP_RB_RPTR) == 12 && Big(kWriteBack) == 12 && cp.Idle());
  // A packet across the end of the ring.
  CommandProcessor wrap(g_physical.data());
  Ring w{wrap};
  Fresh(wrap, w);
  w.write = 2046;
  wrap.WriteRegister(reg::CP_RB_WPTR, 2046);
  CHECK(wrap.Execute());  // empty ring between 0 and 2046: zeros are type-0 writes of register 0
  w.Put({Type0(0x2005, 2), 0x55, 0x66, Type3(pm4::NOP, 1), 0});
  w.Run();
  CHECK(wrap.reg(0x2005) == 0x55 && wrap.reg(0x2006) == 0x66 && wrap.read_pointer() == 3 && Big(kWriteBack) == 3);
  // The waits for reads and prefetches to complete pass at once (nothing is in flight), their payload skipped.
  w.Put({Type3(pm4::WAIT_UNTIL_READ, 1), 0x1234, Type3(pm4::WAIT_IB_PFD_COMPLETE, 1), 0x5678, Type0(0x2007, 1), 0x77});
  w.Run();
  CHECK(wrap.reg(0x2007) == 0x77 && wrap.opcode_counts()[pm4::WAIT_UNTIL_READ] == 1 &&
        wrap.opcode_counts()[pm4::WAIT_IB_PFD_COMPLETE] == 1);
  // MMIO reads: the display constants and the register file.
  CHECK(cp.ReadRegister(reg::D1MODE_VIEWPORT_SIZE) == (1280u << 16 | 720u) && cp.ReadRegister(reg::RB_EDRAM_TIMING) == 0x08100748);
  CHECK(cp.ReadRegister(reg::D1MODE_INTERRUPT_STATUS) == 1 && cp.ReadRegister(0x2001) == 0x22);
  cp.SetDisplaySize(1920, 1080);
  CHECK(cp.ReadRegister(reg::D1MODE_VIEWPORT_SIZE) == (1920u << 16 | 1080u));
}

void Memory() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Fresh(cp, ring);
  // MEM_WRITE: little-endian as the GPU writes, unless the address asks for a swap.
  ring.Put({Type3(pm4::MEM_WRITE, 3), kData | 2, 0x12345678, 0x9ABCDEF0, Type3(pm4::MEM_WRITE, 2), (kData + 8) | 0, 0x12345678});
  ring.Put({Type3(pm4::MEM_WRITE, 2), (kData + 12) | 1, 0x11223344, Type3(pm4::MEM_WRITE, 2), (kData + 16) | 3, 0x11223344});
  ring.Run();
  CHECK(Big(kData) == 0x12345678 && Big(kData + 4) == 0x9ABCDEF0 && Raw(kData + 8) == 0x12345678);
  CHECK(Raw(kData + 12) == 0x22114433 && Raw(kData + 16) == 0x33441122);
  // Fences: EVENT_WRITE_SHD writes its value, or the vertical blank counter with bit 31 of the initiator.
  cp.Vblank();
  cp.Vblank();
  ring.Put({Type3(pm4::EVENT_WRITE_SHD, 3), 0x7, (kData + 20) | 2, 0xF00D, Type3(pm4::EVENT_WRITE_SHD, 3), 0x80000007,
            (kData + 24) | 2, 0xF00D});
  ring.Put({Type3(pm4::EVENT_WRITE, 1), 0x15});
  ring.Run();
  CHECK(Big(kData + 20) == 0xF00D && Big(kData + 24) == 2 && cp.counter() == 2 && cp.reg(reg::VGT_EVENT_INITIATOR) == 0x15);
  // REG_TO_MEM, REG_RMW (immediate masks, then masks from registers).
  ring.Put({Type0(0x1810, 3), 0xFF00FF00, 0x0F0F0F0F, 0x00000001, Type3(pm4::REG_TO_MEM, 2), 0x1810, (kData + 28) | 2});
  ring.Put({Type3(pm4::REG_RMW, 3), 0x1810, 0xFFFF0000, 0x000000AA, Type3(pm4::REG_RMW, 3), 0xC0000000u | 0x1811, 0x1810, 0x1812});
  ring.Run();
  CHECK(Big(kData + 28) == 0xFF00FF00);
  CHECK(cp.reg(0x1810) == 0xFF0000AA && cp.reg(0x1811) == ((0x0F0F0F0Fu & 0xFF0000AAu) | 1u));
  // COND_WRITE: to memory when the polled memory matches, to a register otherwise not at all.
  SetBig(kData + 32, 5);
  ring.Put({Type3(pm4::COND_WRITE, 6), 0x113, (kData + 32) | 2, 5, 0xFFFFFFFF, (kData + 36) | 2, 0xBEEF});
  ring.Put({Type3(pm4::COND_WRITE, 6), 0x014, (kData + 32) | 2, 5, 0xFFFFFFFF, 0x2020, 0xBEEF});
  ring.Put({Type3(pm4::COND_WRITE, 6), 0x003, 0x1810, 0xAA, 0xFF, 0x2021, 0xCAFE});
  ring.Run();
  CHECK(Big(kData + 36) == 0xBEEF && cp.reg(0x2020) == 0 && cp.reg(0x2021) == 0xCAFE);
  // Scratch registers mirrored to memory for the bits of SCRATCH_UMSK.
  ring.Put({Type0(reg::SCRATCH_UMSK, 2), 0x5, kData + 0x40, Type0(reg::SCRATCH_REG0, 3), 0xA0, 0xA1, 0xA2});
  ring.Run();
  CHECK(Big(kData + 0x40) == 0xA0 && Big(kData + 0x44) == 0 && Big(kData + 0x48) == 0xA2 && cp.reg(reg::SCRATCH_REG0 + 1) == 0xA1);
  // EVENT_WRITE_EXT: the whole surface as extents; EVENT_WRITE_ZPD: a finished query reports samples, an open one zero.
  ring.Put({Type3(pm4::EVENT_WRITE_EXT, 2), 0x1A, (kData + 0x60) | 1});
  const uint32_t query = kData + 0x80;
  SetBig(query + 16, 0xFFFFFEED);
  SetBig(query + 20, 0xFFFFFEED);
  SetBig(query + 0x24, 0x1234);  // past the counts: untouched
  ring.Put({Type0(reg::RB_SAMPLE_COUNT_ADDR, 1), query, Type3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
  ring.Run();
  CHECK(Big(kData + 0x60) == (0u << 16 | (8192u >> 3)) && Big(kData + 0x64) == (0u << 16 | (8192u >> 3)) && Big(kData + 0x68) == 1);
  CHECK(Raw(query) == 1000 && Raw(query + 16) == 1000 && Raw(query + 20) == 0 && Big(query + 0x24) == 0x1234);
  SetBig(query + 4, 7);
  ring.Put({Type3(pm4::EVENT_WRITE_ZPD, 1), 0x15});
  ring.Run();
  CHECK(Raw(query) == 0 && Raw(query + 4) == 0 && Raw(query + 16) == 0);
}

void Constants() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Fresh(cp, ring);
  // SET_CONSTANT into each space: ALU, fetch, bool, loop, context registers.
  ring.Put({Type3(pm4::SET_CONSTANT, 3), 0 << 16 | 4, 0x3F800000, 0x40000000});
  ring.Put({Type3(pm4::SET_CONSTANT, 2), 1 << 16 | 6, 0xF1});
  ring.Put({Type3(pm4::SET_CONSTANT, 2), 2 << 16 | 1, 0xB1});
  ring.Put({Type3(pm4::SET_CONSTANT, 2), 3 << 16 | 2, 0x1F});
  ring.Put({Type3(pm4::SET_CONSTANT, 2), 4 << 16 | 0x208, 6});
  ring.Put({Type3(pm4::SET_CONSTANT2, 3), 0x2300, 0x77, 0x78, Type3(pm4::SET_SHADER_CONSTANTS, 2), 0x4100, 0x99});
  // LOAD_ALU_CONSTANT from memory (big-endian there).
  for (uint32_t i = 0; i < 4; ++i) SetBig(kData + 4 * i, 0xC0 + i);
  ring.Put({Type3(pm4::LOAD_ALU_CONSTANT, 3), kData, 0 << 16 | 0x40, 4});
  ring.Run();
  CHECK(cp.reg(reg::kAluConstants + 4) == 0x3F800000 && cp.reg(reg::kAluConstants + 5) == 0x40000000);
  CHECK(cp.reg(reg::kFetchConstants + 6) == 0xF1 && cp.reg(reg::kBoolConstants + 1) == 0xB1 && cp.reg(reg::kLoopConstants + 2) == 0x1F);
  CHECK(cp.reg(reg::RB_MODECONTROL) == 6 && cp.reg(0x2300) == 0x77 && cp.reg(0x2301) == 0x78 && cp.reg(0x4100) == 0x99);
  CHECK(cp.reg(reg::kAluConstants + 0x40) == 0xC0 && cp.reg(reg::kAluConstants + 0x43) == 0xC3);
  // Bin predication: a predicated packet runs only when BIN_SELECT & BIN_MASK has a bit set.
  ring.Put({Type3(pm4::SET_BIN_MASK, 2), 0, 0x1, Type3(pm4::SET_BIN_SELECT_LO, 1), 0x2});
  ring.Put({Type3(pm4::SET_CONSTANT, 2, true), 4 << 16 | 0x10, 0xDEAD});
  ring.Put({Type3(pm4::SET_BIN_SELECT_LO, 1), 0x1, Type3(pm4::SET_CONSTANT, 2, true), 4 << 16 | 0x11, 0xFEED});
  ring.Put({Type3(pm4::SET_BIN_MASK_HI, 1), 0x8, Type3(pm4::SET_BIN_MASK_LO, 1), 0, Type3(pm4::SET_BIN_SELECT_HI, 1), 0x8});
  ring.Put({Type3(pm4::SET_CONSTANT, 2, true), 4 << 16 | 0x12, 0xF00D});
  ring.Run();
  CHECK(cp.reg(0x2010) == 0 && cp.reg(0x2011) == 0xFEED && cp.reg(0x2012) == 0xF00D);
}

void IndirectAndShaders() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Recorder recorder;
  cp.SetRenderer(&recorder);
  Fresh(cp, ring);
  // An indirect buffer calling a second one.
  const uint32_t ib2 = kIb + 0x100;
  const uint32_t ib1_words[] = {Type0(0x2030, 1), 0x31, Type3(pm4::INDIRECT_BUFFER_PFD, 2), ib2, 2};
  for (uint32_t i = 0; i < 5; ++i) SetBig(kIb + 4 * i, ib1_words[i]);
  SetBig(ib2, Type0(0x2031, 1));
  SetBig(ib2 + 4, 0x32);
  ring.Put({Type3(pm4::INDIRECT_BUFFER, 2), 0xA0000000u | kIb, 5, Type0(0x2032, 1), 0x33});
  ring.Run();
  CHECK(cp.reg(0x2030) == 0x31 && cp.reg(0x2031) == 0x32 && cp.reg(0x2032) == 0x33);
  // Shaders: IM_LOAD from memory (type in the address's low bits), IM_LOAD_IMMEDIATE from the packet.
  for (uint32_t i = 0; i < 3; ++i) SetBig(kData + 4 * i, 0x5000 + i);
  ring.Put({Type3(pm4::IM_LOAD, 2), kData | 0, 3, Type3(pm4::IM_LOAD_IMMEDIATE, 4), 1, 2, 0x6000, 0x6001});
  ring.Run();
  CHECK(recorder.shaders == 2);
  CHECK(cp.shader(0).type == 0 && cp.shader(0).address == kData && cp.shader(0).microcode == std::vector<uint32_t>({0x5000, 0x5001, 0x5002}));
  CHECK(cp.shader(1).type == 1 && cp.shader(1).microcode == std::vector<uint32_t>({0x6000, 0x6001}) && recorder.last_hash == cp.shader(1).hash);
  CHECK(cp.shader(0).hash != cp.shader(1).hash && cp.shader(0).hash != 0);
  // An immediate load whose microcode wraps around the ring's end (after type-2 no-ops up to it): gathered.
  while (ring.write != cp.ring_dwords() - 4) ring.Put(0x80000000u);
  ring.Put({Type3(pm4::IM_LOAD_IMMEDIATE, 6), 1, 4, 0x7000, 0x7001, 0x7002, 0x7003});
  ring.Run();
  CHECK(cp.shader(1).microcode == std::vector<uint32_t>({0x7000, 0x7001, 0x7002, 0x7003}));
  // Draws by EDRAM mode: colour+depth draws, copy resolves, ignore does nothing.
  const uint32_t dma_initiator = 6u << 16 | 0u << 6 | 4;            // 6 indices, DMA, 16-bit, triangle list
  const uint32_t auto_initiator = 4u << 16 | 2u << 6 | 1u << 11 | 8;  // 4 indices, auto, rect list
  ring.Put({Type0(reg::RB_MODECONTROL, 1), 4, Type3(pm4::DRAW_INDX, 4), 0, dma_initiator, 0xA0000000u | 0x40000, 2u << 30 | 3});
  ring.Put({Type3(pm4::DRAW_INDX_2, 1), auto_initiator, Type3(pm4::DRAW_INDX_2, 3), 3u << 16 | 1u << 6 | 4, 0x00010002, 0x00030000});
  ring.Put({Type0(reg::RB_MODECONTROL, 1), 6, Type3(pm4::DRAW_INDX_2, 1), auto_initiator});
  ring.Put({Type0(reg::RB_MODECONTROL, 1), 0, Type3(pm4::DRAW_INDX_2, 1), auto_initiator});
  ring.Run();
  CHECK(recorder.draws.size() == 4 && recorder.copies == 1);  // mode 0 is a draw too: the renderer judges it
  if (recorder.draws.size() == 4) {
    CHECK(recorder.draws[3].edram_mode == 0 && recorder.draws[0].edram_mode == 4);
    const DrawCall& d = recorder.draws[0];
    CHECK(d.primitive_type == 4 && d.index_count == 6 && d.source == DrawSource::kDma && !d.index_32);
    CHECK(d.index_base == 0x40000 && d.index_bytes == 6 && d.index_endian == Endian::k8in32);
    CHECK(d.vertex_shader == &cp.shader(0) && d.pixel_shader == &cp.shader(1) && d.registers == cp.registers());
    const DrawCall& a = recorder.draws[1];
    CHECK(a.primitive_type == 8 && a.index_count == 4 && a.source == DrawSource::kAuto && a.index_32);
    const DrawCall& im = recorder.draws[2];
    CHECK(im.source == DrawSource::kImmediate && im.index_count == 3 && im.immediate == std::vector<uint32_t>({0x00010002, 0x00030000}));
  }
  CHECK(cp.reg(reg::VGT_DMA_BASE) == (0xA0000000u | 0x40000) && cp.reg(reg::VGT_DRAW_INITIATOR) == auto_initiator);
  // The a2xx packets Mesa's freedreno driver emits: DRAW_INDX_BIN (DRAW_INDX with two binning dwords after the
  // initiator, ignored), WAIT_REG_EQ / WAIT_REG_GTE on a register that already holds the value (register, reference,
  // mask, interval), LOAD_CONSTANT_CONTEXT (registers from memory at the address plus the offset), SET_SHADER_BASES.
  const size_t draws_before = recorder.draws.size();
  ring.Put({Type0(reg::RB_MODECONTROL, 1), 4, Type3(pm4::DRAW_INDX_BIN, 6), 0, dma_initiator, 0x11, 0x22,
            0xA0000000u | 0x44000, 2u << 30 | 3});
  SetBig(kData + 4 * 0x10, 0xABCD);
  ring.Put({Type0(0x2011, 1), 5, Type3(pm4::WAIT_REG_EQ, 4), 0x2011, 5, 0xFF, 1, Type3(pm4::WAIT_REG_GTE, 4), 0x2011, 3, 0xFF, 1,
            Type3(pm4::LOAD_CONSTANT_CONTEXT, 3), kData, 4u << 16 | 0x10, 1, Type3(pm4::SET_SHADER_BASES, 1), 0x80000180,
            Type0(0x2012, 1), 0x99});
  ring.Run();
  CHECK(recorder.draws.size() == draws_before + 1);
  if (recorder.draws.size() == draws_before + 1) {
    const DrawCall& bin = recorder.draws.back();
    CHECK(bin.primitive_type == 4 && bin.index_count == 6 && bin.source == DrawSource::kDma && bin.index_base == 0x44000 &&
          bin.index_bytes == 6);
  }
  CHECK(cp.reg(0x2010) == 0xABCD && cp.reg(0x2012) == 0x99 && cp.opcode_counts()[pm4::WAIT_REG_EQ] == 1 &&
        cp.opcode_counts()[pm4::WAIT_REG_GTE] == 1 && cp.opcode_counts()[pm4::SET_SHADER_BASES] == 1);
  // XE_SWAP: the front buffer to present.
  ring.Put({Type3(pm4::XE_SWAP, 64), 0x53574150, 0xBE000000u | 0x100000, 1280, 720});
  for (int i = 0; i < 60; ++i) ring.Put(Type2());
  ring.Run();
  CHECK(recorder.swaps == 1 && recorder.frontbuffer == 0x1E100000 && recorder.width == 1280 && recorder.height == 720);
  // Interrupts: one per CPU in the mask, source 1.
  std::vector<std::pair<uint32_t, uint32_t>> interrupts;
  cp.SetInterruptHandler([&](uint32_t source, uint32_t cpu) { interrupts.push_back({source, cpu}); });
  ring.Put({Type3(pm4::INTERRUPT, 1), 0x5});
  ring.Run();
  CHECK(interrupts.size() == 2 && interrupts[0] == std::make_pair(1u, 0u) && interrupts[1] == std::make_pair(1u, 2u));
}

void Waits() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Recorder recorder;
  cp.SetRenderer(&recorder);
  Fresh(cp, ring);
  // COHER_STATUS_HOST: the CPU asks for coherency, the wait on the status bit sees it done, the renderer invalidates.
  ring.Put({Type0(reg::COHER_SIZE_HOST, 3), 0x1000, 0x50000, 0x80000000});
  ring.Put({Type3(pm4::WAIT_REG_MEM, 5), 0x3, reg::COHER_STATUS_HOST, 0, 0x80000000, 0x10, Type0(0x2040, 1), 1});
  ring.Run();
  CHECK(cp.reg(0x2040) == 1 && cp.reg(reg::COHER_STATUS_HOST) == 0);
  CHECK(recorder.invalidations == 1 && recorder.invalid_base == 0x50000 && recorder.invalid_size == 0x1000);
  // The worker: a wait on memory holds the stream until the CPU writes the value.
  cp.Start();
  const uint32_t fence = kData + 0x100, marker = kData + 0x104;
  ring.Put({Type3(pm4::WAIT_REG_MEM, 5), 0x13, fence | 2, 5, 0xFFFFFFFF, 0x100, Type3(pm4::MEM_WRITE, 2), marker | 2, 0x600D});
  ring.Kick();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(Big(marker) == 0 && !cp.Idle());
  SetBig(fence, 5);
  for (int i = 0; i < 200 && !cp.Idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(Big(marker) == 0x600D && cp.Idle() && Big(kWriteBack) == ring.write);
  // Comparisons: >= on a register the stream writes.
  ring.Put({Type0(0x2041, 1), 9, Type3(pm4::WAIT_REG_MEM, 5), 0x5, 0x2041, 9, 0xFF, 0, Type0(0x2042, 1), 2});
  ring.Kick();
  for (int i = 0; i < 200 && !cp.Idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(cp.reg(0x2042) == 2);
  // Stopping releases a wait that never ends.
  ring.Put({Type3(pm4::WAIT_REG_MEM, 5), 0x0, 0x2041, 0, 0, 0x100});
  ring.Kick();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  cp.Stop();
  CHECK(!cp.Idle());
}

void Kernel() {
  // Through any360::sys: VdInitializeRingBuffer and the MMIO kick reach the command processor; the vblank counter moves.
  CommandProcessor cp(g_physical.data());
  std::memset(g_physical.data(), 0, g_physical.size());
  sys::Graphics().Reset();
  sys::Graphics().SetBackend(&cp);
  sys::Graphics().InitializeRingBuffer({}, kRing, kRingLog2);
  sys::Graphics().EnableReadPointerWriteBack({}, kWriteBack, 6);
  SetBig(kRing, Type0(0x2050, 1));
  SetBig(kRing + 4, 0x51);
  CHECK(sys::IsGpuMmio(0x7FC80714) && sys::GpuRegisterIndex(0x7FC80714) == reg::CP_RB_WPTR);
  sys::Graphics().WriteRegister(sys::GpuRegisterIndex(0x7FC80714), 2);
  CHECK(cp.Execute() && cp.reg(0x2050) == 0x51 && sys::Graphics().ReadRegister(reg::CP_RB_RPTR) == 2 && Big(kWriteBack) == 2);
  cp.SetInterruptHandler([](uint32_t, uint32_t) {});
  sys::Graphics().StartVblank(1000.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  sys::Graphics().StopVblank();
  CHECK(cp.counter() > 5);
  sys::Graphics().Reset();
}

// What the reference command processor does that the packet descriptions alone do not say.
void ReferenceDetails() {
  CommandProcessor cp(g_physical.data());
  Ring ring{cp};
  Recorder recorder;
  cp.SetRenderer(&recorder);
  Fresh(cp, ring);
  // A zero dword is padding; a swap moves the counter; a predicated swap never runs; a write to COHER_STATUS_HOST
  // keeps the request pending (bit 31) whatever the value.
  ring.Put({0, 0, Type0(0x2060, 1), 0x61, Type3(pm4::XE_SWAP, 4), 0x53574150, 0x100000, 640, 480});
  ring.Put({Type3(pm4::XE_SWAP, 4, true), 0x53574150, 0x200000, 640, 480, Type0(reg::COHER_STATUS_HOST, 1), 0x3000000});
  ring.Run();
  CHECK(cp.reg(0x2060) == 0x61 && cp.reg(0) == 0 && recorder.swaps == 1 && recorder.frontbuffer == 0x100000 && cp.counter() == 1);
  CHECK(cp.reg(reg::COHER_STATUS_HOST) == 0x83000000);
  // A visibility query that kills pixels draws nothing.
  ring.Put({Type0(reg::RB_MODECONTROL, 1), 4, Type0(reg::PA_SC_VIZ_QUERY, 1), 0x81, Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 4});
  ring.Put({Type0(reg::PA_SC_VIZ_QUERY, 1), 0x1, Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 4});
  ring.Run();
  CHECK(recorder.draws.size() == 1);
  // An opcode no title emits stops the rest of what was kicked; the read pointer still moves to the write pointer.
  ring.Put({Type3(0x7E, 2), 1, 2, Type0(0x2061, 1), 0x62});
  ring.Run();
  CHECK(cp.reg(0x2061) == 0 && cp.read_pointer() == ring.write);
  ring.Put({Type0(0x2062, 1), 0x63});
  ring.Run();
  CHECK(cp.reg(0x2062) == 0x63);
}

// The display gamma ramp, kept as the reference keeps it from the DC_LUT_* writes (through MMIO here; PM4 register
// writes reach it the same way), and handed to the renderer before each swap.
void GammaRamp() {
  CommandProcessor cp(g_physical.data());
  // The defaults: linear.
  CHECK(cp.gamma_table()[255] == (0x3FFu | 0x3FFu << 10 | 0x3FFu << 20) && cp.gamma_table()[0] == 0);
  cp.WriteRegister(reg::DC_LUT_RW_MODE, 0);
  cp.WriteRegister(reg::DC_LUT_WRITE_EN_MASK, 7);
  // A whole entry (DC_LUT_30_COLOR), then one red, green, blue at a time (DC_LUT_SEQ_COLOR: 16-bit values, the top
  // 10 bits kept); each moves the index on.
  cp.WriteRegister(reg::DC_LUT_RW_INDEX, 5);
  cp.WriteRegister(reg::DC_LUT_30_COLOR, 0x3FFu | 0x155u << 10 | 0x2AAu << 20);
  cp.WriteRegister(reg::DC_LUT_SEQ_COLOR, 0x100u << 6);
  cp.WriteRegister(reg::DC_LUT_SEQ_COLOR, 0x040u << 6);
  cp.WriteRegister(reg::DC_LUT_SEQ_COLOR, 0x3FFu << 6);
  CHECK(cp.gamma_table()[5] == (0x3FFu | 0x155u << 10 | 0x2AAu << 20));
  CHECK(cp.gamma_table()[6] == (0x3FFu | 0x040u << 10 | 0x100u << 20) && cp.reg(reg::DC_LUT_RW_INDEX) == 7);
  // Disabled components keep their value.
  cp.WriteRegister(reg::DC_LUT_WRITE_EN_MASK, 0b100);  // red only
  cp.WriteRegister(reg::DC_LUT_RW_INDEX, 5);
  cp.WriteRegister(reg::DC_LUT_30_COLOR, 0);
  CHECK(cp.gamma_table()[5] == (0x3FFu | 0x155u << 10));
  // The PWL ramp: three values an entry (red, green, blue), bit 7 of the index ignored, bits 0-5 of each half zero.
  cp.WriteRegister(reg::DC_LUT_RW_MODE, 1);
  cp.WriteRegister(reg::DC_LUT_WRITE_EN_MASK, 7);
  cp.WriteRegister(reg::DC_LUT_RW_INDEX, 0x80 | 3);
  for (uint32_t v : {0x0200FFFFu, 0x04001234u, 0x0600ABCDu}) cp.WriteRegister(reg::DC_LUT_PWL_DATA, v);
  CHECK(cp.gamma_pwl()[9] == 0x0200FFC0u && cp.gamma_pwl()[10] == 0x04001200u && cp.gamma_pwl()[11] == 0x0600ABC0u);
  CHECK(cp.reg(reg::DC_LUT_RW_INDEX) == (0x80u | 4));
  // A swap hands the ramp to the renderer first.
  struct RampRecorder : Renderer {
    uint32_t entry6 = 0, swaps = 0;
    void GammaRamp(const uint32_t* table, const uint32_t*) override { entry6 = table[6]; }
    void Swap(uint32_t, uint32_t, uint32_t, const uint32_t*) override { swaps += entry6 != 0; }
  } recorder;
  Ring ring{cp};
  cp.SetRenderer(&recorder);
  Fresh(cp, ring);
  ring.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, 0x100000, 640, 480});
  ring.Run();
  CHECK(recorder.swaps == 1 && recorder.entry6 == (0x3FFu | 0x040u << 10 | 0x100u << 20));
}

}  // namespace

int main() {
  ReferenceDetails();
  GammaRamp();
  RingAndRegisters();
  Memory();
  Constants();
  IndirectAndShaders();
  Waits();
  Kernel();
  std::printf("gfx_cp_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
