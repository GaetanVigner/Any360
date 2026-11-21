// xboxkrnl.exe's LZX decompression interface (LDI, step 12: implemented by Xenia Canary, stubbed by the reference):
// LDICreateDecompression, LDIDecompress, LDIDestroyDecompression. A title decodes LZX chunks with it (the format of
// CAB files' LZX folders and of the XDK's XMemCompress): each call takes one chunk, its own bitstream, and gives one
// frame of up to 32 KiB; the window, the repeated offsets, the trees and the block in progress carry from one call to
// the next, as Xenia's xboxkrnl_ldi.cc and ldi_decompressor.cc drive it. The decoder is Any360's own, written from
// the format (Microsoft's LZX DELTA specification, [MS-PATCH], with plain LZX's block header and window sizes): Xenia
// links libmspack's, which is LGPL.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "guest.h"

namespace any360::sys {

// One LZX stream: a window of 32 KiB to 2 MiB (15 to 21 bits) and the decoder's state.
class LzxDecoder {
 public:
  static constexpr uint32_t kFrameSize = 0x8000;
  static constexpr uint32_t kMinWindowBits = 15, kMaxWindowBits = 21;

  LzxDecoder();
  ~LzxDecoder();
  LzxDecoder(const LzxDecoder&) = delete;
  LzxDecoder& operator=(const LzxDecoder&) = delete;

  // A new stream with a window of 2^window_bits bytes (the window's memory is kept when it is large enough); false
  // outside 15 to 21 bits.
  bool Init(uint32_t window_bits);
  uint32_t window_bits() const { return window_bits_; }
  // Back to the start of a stream (the window's contents are forgotten).
  void Reset();
  // Decodes one chunk (its own bitstream, `size` bytes) into a frame of `frame` bytes (1 to 32 KiB). false on a
  // corrupt chunk, or one that asks for more than it holds; the stream should then be reset.
  bool Decompress(const uint8_t* source, size_t size, uint8_t* destination, uint32_t frame);

 private:
  struct Tree;
  struct Reader;
  bool ReadLengths(Reader& in, uint8_t* lengths, uint32_t first, uint32_t last);
  bool ReadBlockHeader(Reader& in);
  template <bool kAligned>
  bool DecodeRun(Reader& in, uint32_t run);

  std::vector<uint8_t> window_;
  uint32_t window_bits_ = 0, window_size_ = 0, main_symbols_ = 0;
  uint32_t window_posn_ = 0, frame_posn_ = 0, frame_ = 0;
  uint64_t output_ = 0;  // bytes given before this frame
  uint32_t r0_ = 1, r1_ = 1, r2_ = 1;
  uint32_t block_type_ = 0, block_length_ = 0, block_remaining_ = 0;
  bool header_read_ = false, intel_started_ = false, pad_pending_ = false;
  int32_t intel_filesize_ = 0, intel_curpos_ = 0;
  // How far the main and length trees' code lengths were written since the last reset: Reset clears only that much
  // (nothing for a stream of uncompressed blocks only, less for a small window).
  uint32_t main_dirty_ = 0, length_dirty_ = 0;
  std::unique_ptr<Tree> main_, length_, aligned_, pretree_;
};

// The handles LDICreateDecompression gives: the guest address of a context that starts with the magic "LDIC"
// (0x4349444C): the title's scratch memory, or the context its allocator gave. The window and the decoder's state
// are the host's; contexts are kept in a registry under one lock. Registry entries and the decoders of destroyed
// contexts are kept for the next ones: no host allocation once a title has had as many contexts of a size.
class LdiContexts {
 public:
  // The title's allocator and freer are guest functions: call(function, argument) runs one on the calling thread and
  // gives its r3.
  using GuestCall = uint32_t (*)(uint32_t function, uint32_t argument, void* user);

  static constexpr uint32_t kMagic = 0x4349444C, kContextSize = 0x2FF8, kWindowExtra = 0x105,
                            kSourceGrowth = 0x1800;
  // MDI_ERROR_* results.
  static constexpr uint32_t kNoError = 0, kNotEnoughMemory = 1, kBadParameters = 2, kBufferOverflow = 3, kFailed = 4;

  // LDICreateDecompression (data block max in, LZXDECOMPRESS config {window size, CPU type}, alloc, free, scratch,
  // source buffer min out, handle out): the largest source a chunk needs (data block max + 0x1800) written when asked;
  // without a handle pointer that is all (success). The window size must be a power of two of 2^12 to 2^22 bytes
  // (else MDI_ERROR_NOT_ENOUGH_MEMORY, as Xenia); one the decoder does not cover (under 32 KiB or over 2 MiB) gives a
  // context whose every decompression fails. With an allocator, the context (0x2FF8 bytes) and then the window
  // (window size + 0x105) are allocated through it, the first freed when the second fails; without one, the scratch
  // memory is the context. The handle is written, 0 on failure.
  uint32_t Create(Guest g, uint32_t data_block_max_ptr, uint32_t config, uint32_t alloc, uint32_t free, uint32_t scratch,
                  uint32_t source_min_ptr, uint32_t handle_ptr, GuestCall call, void* user);
  // LDIDecompress (handle, source, source size, destination, destination size in/out): a chunk into one frame of
  // *size bytes. MDI_ERROR_BAD_PARAMETERS for an unknown handle or no size pointer, MDI_ERROR_BUFFER_OVERFLOW for a
  // size of 0 or past the context's data block max, MDI_ERROR_FAILED for a size over 32 KiB, a range that wraps the
  // address space, a decoder-less context or a corrupt chunk; the size written back is 0 on failure.
  uint32_t Decompress(Guest g, uint32_t handle, uint32_t source, uint32_t source_size, uint32_t destination,
                      uint32_t size_ptr);
  // LDIDestroyDecompression: the context forgotten and its magic cleared, then, when the title's allocator made it,
  // the window and the context freed through its freer (outside the lock). MDI_ERROR_BAD_PARAMETERS for an unknown
  // handle.
  uint32_t Destroy(Guest g, uint32_t handle, GuestCall call, void* user);
  // LDIResetDecompression: the context's decoder back to the start of a stream (its window position, repeated
  // offsets, trees and Intel E8 state), the window kept; MDI_ERROR_BAD_PARAMETERS for an unknown handle.
  uint32_t ResetDecompression(Guest g, uint32_t handle);

  // Every context forgotten (a new title).
  void Reset();
  size_t size() const;

 private:
  struct Context {
    uint32_t handle = 0, window_size = 0, data_block_max = 0, free = 0, context_alloc = 0, window_alloc = 0;
    std::unique_ptr<LzxDecoder> decoder;  // none for a window the decoder does not cover
  };
  Context* Find(Guest g, uint32_t handle);

  mutable std::mutex mutex_;
  std::vector<Context> contexts_;  // a few at most: searched in order
  std::vector<std::unique_ptr<LzxDecoder>> spare_;
};
LdiContexts& Ldi();

}  // namespace any360::sys
