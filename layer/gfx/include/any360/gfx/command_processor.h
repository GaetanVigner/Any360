// any360::gfx's command processor: the GPU's front end, behind any360::sys's GpuBackend. It executes what the title's
// D3D runtime writes, the primary ring (kicked through CP_RB_WPTR) and the indirect buffers it calls: register and
// constant writes into the register file, the synchronisation the CPU waits on (WAIT_REG_MEM, MEM_WRITE, the fence
// events, scratch registers, coherency, interrupts), shader microcode loads and bin predication. Draws, EDRAM
// resolves and swaps go to a Renderer. Nothing here depends on a graphics API: the Vulkan renderer is one Renderer.
//
// Memory is the guest's physical memory as the GPU sees it: packets and constants are big-endian; what the GPU
// writes and polls is little-endian, swapped by the endianness in the address's low two bits (as on the console).
// The host is little-endian.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "any360/sys/graphics.h"
#include "registers.h"

namespace any360::gfx {

namespace capture {
class Writer;
}

enum class Endian : uint32_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3 };
uint32_t GpuSwap(uint32_t value, Endian endian);

struct Shader {
  uint32_t type = 0;     // 0 vertex, 1 pixel
  uint32_t address = 0;  // guest physical address of the microcode (inside the packet for an immediate load)
  std::vector<uint32_t> microcode;  // host-order dwords
  uint64_t hash = 0;     // FNV-1a over the microcode: the key of the translated shader cache
};

enum class DrawSource : uint32_t { kDma = 0, kImmediate = 1, kAuto = 2 };

struct DrawCall {
  uint32_t primitive_type = 0;  // VGT_DRAW_INITIATOR bits 0-5
  uint32_t index_count = 0;
  DrawSource source = DrawSource::kAuto;
  bool index_32 = false;
  bool major_mode_explicit = false;
  uint32_t edram_mode = 4;   // RB_MODECONTROL: 4 colour and depth, 5 depth only, 0 ignore
  uint32_t index_base = 0;   // kDma: guest physical address of the indices
  uint32_t index_bytes = 0;  // kDma: their length
  Endian index_endian = Endian::kNone;
  std::vector<uint32_t> immediate;  // kImmediate: the packet's index dwords, host order
  const Shader* vertex_shader = nullptr;
  const Shader* pixel_shader = nullptr;
  const uint32_t* registers = nullptr;  // the register file at the draw (reg::kRegisterCount dwords)
};

class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual void ShaderLoaded(const Shader&) {}
  virtual void Draw(const DrawCall&) {}
  // A draw in EDRAM copy mode: resolve the render target as the RB_COPY_* registers say.
  virtual void Copy(const uint32_t* /*registers*/) {}
  // A swap (XE_SWAP): the front buffer is the texture fetch constant 0 of `registers` describes (VdSwap writes it).
  virtual void Swap(uint32_t /*frontbuffer*/, uint32_t /*width*/, uint32_t /*height*/, const uint32_t* /*registers*/) {}
  // The command processor is about to make GPU progress visible to the CPU (a fence, a memory write, an interrupt,
  // the read pointer): every draw so far must have run, as the CPU may then reuse what they read.
  virtual void Flush() {}
  // The CPU wrote [base, base + size) (COHER_*_HOST): cached copies of that memory are stale.
  virtual void Invalidate(uint32_t /*base*/, uint32_t /*size*/) {}
  // The display gamma ramp at a swap, before Swap: the 256-entry table (DC_LUT_30_COLOR values: 10-bit blue, green,
  // red from bit 0) and the piecewise linear one (128 entries of red, green, blue DC_LUT_PWL_DATA values).
  virtual void GammaRamp(const uint32_t* /*table*/, const uint32_t* /*pwl*/) {}
};

class CommandProcessor : public sys::GpuBackend {
 public:
  // `physical`: the guest's 512 MB of physical memory; an address is masked with 0x1FFFFFFF.
  explicit CommandProcessor(uint8_t* physical);
  ~CommandProcessor() override;

  void SetRenderer(Renderer* renderer) { renderer_ = renderer; }
  // Interrupts the command stream raises (source 1, once per CPU in INTERRUPT's mask) and vertical blanks (source 0,
  // CPU 2). The default runs the title's callback through sys::Graphics().
  using InterruptHandler = std::function<void(uint32_t source, uint32_t cpu)>;
  void SetInterruptHandler(InterruptHandler handler) { interrupt_ = std::move(handler); }
  void SetDisplaySize(uint32_t width, uint32_t height) { display_ = width << 16 | height; }

  // GpuBackend.
  void InitializeRingBuffer(sys::Guest g, uint32_t base, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(sys::Guest g, uint32_t address, uint32_t block_log2) override;
  void WriteRegister(uint32_t index, uint32_t value) override;
  uint32_t ReadRegister(uint32_t index) override;
  void Vblank() override;

  // Executes the ring from the read pointer to the write pointer; false when stopped in a wait.
  bool Execute();
  void ExecuteIndirect(uint32_t address, uint32_t dwords);
  // A worker thread executing the ring whenever the write pointer moves.
  void Start();
  void Stop();
  bool Idle() const { return !busy_ && read_index_ == write_index_; }

  uint32_t reg(uint32_t index) const;
  const uint32_t* registers() const { return registers_.data(); }
  uint32_t counter() const { return counter_; }
  uint32_t read_pointer() const { return read_index_; }
  uint32_t ring_dwords() const { return ring_dwords_; }
  const Shader& shader(uint32_t type) const { return shaders_[type & 1]; }

  // Frame captures (capture.h): each kick of the ring is recorded into `writer` before it runs (null: none).
  void SetCapture(capture::Writer* writer) { capture_ = writer; }
  // Replay of a capture: the register file and the shaders as it starts, then each kick it recorded. A wait that is
  // not met when reached (on the CPU, absent from a replay) passes, counted in replay_skipped_waits().
  void Restore(const uint32_t* registers, uint32_t count);
  void RestoreShader(uint32_t type, std::vector<uint32_t> microcode);
  bool Replay(uint32_t ring_base, uint32_t ring_dwords, uint32_t read, uint32_t write);
  uint32_t replay_skipped_waits() const { return replay_skipped_waits_; }
  // The display gamma ramp the title wrote through DC_LUT_* (the reference's defaults until it does).
  const uint32_t* gamma_table() const { return gamma_table_.data(); }
  const uint32_t* gamma_pwl() const { return gamma_pwl_.data(); }
  void RestoreGammaRamp(const uint32_t* table, const uint32_t* pwl);
  // Type-3 packets executed, by opcode: what a title's frames use.
  const std::array<uint64_t, 128>& opcode_counts() const { return opcode_counts_; }

 private:
  struct Reader {
    uint32_t base, capacity, offset, remaining;  // dwords, wrapping at capacity
  };
  uint32_t Next(Reader& r) const;
  bool ExecutePacket(Reader& r, int depth);
  bool ExecuteType3(Reader& r, uint32_t packet, int depth);
  void SetRegister(uint32_t index, uint32_t value);
  void GammaRampWrite(uint32_t index);  // after a DC_LUT_* register write
  bool Wait(uint32_t wait_info, uint32_t poll, uint32_t reference, uint32_t mask, uint32_t interval);
  bool Test(uint32_t wait_info, uint32_t poll, uint32_t reference, uint32_t mask, bool coherent);
  void MakeCoherent();
  void Draw(Reader& r, uint32_t opcode, uint32_t end);
  void LoadShader(uint32_t type, uint32_t address, uint32_t dwords);
  void Interrupt(uint32_t source, uint32_t cpu);

  uint8_t* Host(uint32_t address) const { return physical_ + (address & 0x1FFFFFFF); }
  uint32_t LoadRaw(uint32_t address) const;
  void StoreRaw(uint32_t address, uint32_t value);
  uint32_t LoadBig(uint32_t address) const;
  void StoreBig(uint32_t address, uint32_t value);

  uint8_t* physical_;
  Renderer* renderer_ = nullptr;
  InterruptHandler interrupt_;
  std::vector<uint32_t> registers_;
  uint32_t display_ = 1280u << 16 | 720u;
  uint32_t ring_base_ = 0, ring_dwords_ = 0, write_back_ = 0, write_back_block_log2_ = 0;
  std::atomic<uint32_t> read_index_{0}, write_index_{0}, counter_{0};
  std::atomic<bool> busy_{false}, stopping_{false};
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;
  Shader shaders_[2];
  capture::Writer* capture_ = nullptr;
  bool replaying_ = false;
  uint32_t replay_skipped_waits_ = 0;
  std::array<uint64_t, 128> opcode_counts_{};
  std::array<uint32_t, 256> gamma_table_{};
  std::array<uint32_t, 128 * 3> gamma_pwl_{};
  uint32_t gamma_component_ = 0;  // the next red, green or blue of DC_LUT_SEQ_COLOR / DC_LUT_PWL_DATA
  std::mutex mutex_;
  std::condition_variable kicked_;
  std::thread worker_;
};

}  // namespace any360::gfx
