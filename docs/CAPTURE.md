# Frame capture and replay

A frame capture records what a GPU front end ran, so that `any360::gfx` can replay a title's real frames offline.
Hand-written PM4 shows that a feature works. Only the title's own frames show which features it needs, how often, and
how close our image is to the reference's.

**A capture holds the title's data** (its memory, shaders, textures and frames). It stays on the machine that made it
and never goes into a repository; `.gitignore` excludes `*.a360cap`.

## The format

The format is defined in `layer/gfx/include/any360/gfx/capture.h`. Everything is little-endian. A file is the magic
`A360CAP\0`, a u32 version (1), then a sequence of chunks, each `{u32 type, u32 0, u64 payload bytes, payload}`.

| Chunk | Payload |
|---|---|
| `kInfo` (1) | UTF-8 text: the producer, the title, where in the game |
| `kMemory` (2) | u32: the size of guest physical memory the capture covers |
| `kRegisters` (3) | u32 count, then count u32 values: the register file when the capture starts |
| `kShaders` (7) | Optional. u32 vertex dwords, u32 pixel dwords, then the active shaders' microcode in host order, when the capture starts |
| `kPages` (4) | u32 count, then count entries of `{u32 page index, 4096 bytes}`: the physical pages that changed since the last kick. At the first kick, this is every non-zero page |
| `kKick` (5) | u32 ring base (physical), u32 ring dwords, u32 read index, u32 write index: the ring span the GPU then ran |
| `kFrame` (6) | Optional. u32 width, u32 height, then RGBA8 pixels (R in the low byte): the reference's presented frame, after the kick that swapped |

Changes to memory are found by hashing each 4 KB page at every kick, split across threads. For 512 MB this costs
tens of milliseconds per kick: a capture is meant to cover a few frames, not to run all the time.

## Replaying

```
gfx_replay CAPTURE [--gpu NAME] [--out DIR]
```

`gfx_replay` runs the capture through `CommandProcessor` and `VulkanRenderer` over a fresh 512 MB of guest memory.
It restores the registers and the shaders, applies each page chunk, and replays each kick. When the replay reaches a
wait that the CPU would have satisfied, it passes the wait and counts it.

It prints:

- the kicks, pages and frames in the capture;
- the renderer's counts: draws, skipped, failed, resolves, shaders, pipelines, render targets and texture uploads;
- **the draws not drawn, by reason** (`VulkanRenderer::failures()`), for example `MSAA surfaces (not yet)` or
  `texture 3: format 50`;
- **the PM4 packets executed, by opcode** (`CommandProcessor::opcode_counts()`);
- for each reference frame: the share of pixels off by more than 8 in red, green or blue, and the mean difference.

With `--out DIR`, `gfx_replay` also writes `frame_N_ours.png` and `frame_N_reference.png`.

The format and the replay are tested end to end. `gfx_render_tests` captures a frame of two triangles, a resolve and
a swap, then replays it into fresh memory with a fresh command processor and renderer. The replayed frame must
match the captured frame exactly. `test.ps1` then runs `gfx_replay` on that same capture.

## Producing a capture

Any front end can produce a capture through `capture::Writer`. Writing one takes three calls:

```cpp
capture::Writer writer;
writer.Open(path, physical_base, physical_bytes, register_file, register_count, info);  // when capturing starts
writer.Shaders(active_vertex_microcode, active_pixel_microcode);                         // optional
// For every kick, before it runs:
writer.Kick(ring_base, ring_dwords, read_index, write_index);
// After each swap, optional:
writer.Frame(width, height, rgba);
writer.Close();                                                                         // after N frames
```

`any360::gfx`'s own `CommandProcessor` has `SetCapture(&writer)` and records its kicks.

### In ReXGlue (the reference)

To capture under a title's ReXGlue build, the title's repository patches the SDK's `src/graphics/command_processor.cpp`.
For Crackdown, that repository is CrackdownRewrite, alongside its existing `gpu_draw_trace` cvars.

| Point | Where in the SDK | Call |
|---|---|---|
| Start | The command processor thread, between two kicks, when a capture cvar is set | `Open(path, memory_->TranslatePhysical(0), 512 MB, register_file_->values, RegisterFile::kRegisterCount, info)`; optionally `Shaders(...)` with the active shaders' `ucode_data()` |
| Kick | The start of `CommandProcessor::ExecutePrimaryBuffer(read_index, write_index)` | `Kick(primary_buffer_ptr_, primary_buffer_size_ / 4, read_index, write_index)` |
| Swap | After `IssueSwap(...)` returns, in `XE_SWAP`'s handler | `CaptureGuestOutput(image)` on the presenter (RGBX, rows at `image.stride`), packed to `width * 4` per row, then `Frame(...)` |
| End | After the requested number of swaps | `Close()` |

Run the reference at resolution scale 1 so that its frame and ours have the same size. A capture that starts in the
middle of play lacks only what was loaded before it: shaders loaded earlier come from `kShaders`, and textures and
buffers come from the first page chunk.
