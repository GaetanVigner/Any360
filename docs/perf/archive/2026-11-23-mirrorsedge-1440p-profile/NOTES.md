# 2026-11-23: Mirror's Edge at 1440p, host MSAA 4x, frame rate uncapped: where the time goes

Launch: `mirrorsedge.exe --any360_gpu_scale=2 --any360_gpu_msaa=4 --any360_gpu_vblank_hz=1000
--any360_profile_after=90 --any360_profile_seconds=30 --any360_hitch_trace_ms=100`, cores 0-7, RTX 5070 Ti, Any360
89b5bc6 + the texture cache walk fix. First level, running through it. [profile.txt](profile.txt) /
[profile.csv](profile.csv): 30 s of samples of every thread; [game_log_stats.txt](game_log_stats.txt): the game
log's per-frame report over the same minutes.

## Where it stands

- **34-40 FPS** (median 22.7-26 ms, 10% low 23.6-25.6 FPS, 1% low 22-24 FPS, slowest frame 43-73 ms).
- **The GPU is not the limit**: 4.8-5.1 ms of GPU work a frame (draws 2.9-3.2, resolves 0.9-1.0, other 0.6).
- **The renderer's thread is**: the report says "the command processor's thread bound"; per frame it spends
  **about 20 ms in resolves**, 4.4-7.7 ms recording draws (textures 1.9-2.2), 1.5 ms submitting.
- Process: 4.6 GB private, 999 textures, 1653 pipelines; 2.35 cores busy on average.

## The list, largest first

| # | What | Cost measured | Where | Lead |
|---|---|---|---|---|
| 1 | Resolves of a k_16_16_16_16 target (EDRAM format 5, 16-bit fixed point; the host draws it as R16G16B16A16_SNORM) to k_16_16_16_16 textures (format 26) done on the CPU: read back, converted texel by texel (`lroundf`), written to guest memory, then uploaded again as a texture | ~20 ms a frame on the renderer's thread; in the profile `VulkanRenderer::Copy` 5.7 s, `lroundf` 4.6 s, `roundf` 0.9 s, `TiledOffset2D` 0.4 s, `SwapElement` 0.25 s of 30 s | "CPU colour target format 5 (host 92) to format 26": 328x184 once a frame, 8x8 (x272 over 5 s), 512x512, 128x128, 32x32, 16x16 (the exposure and bloom chain); 134BC000 322x182 uploaded again 573 times | A GPU path for these resolves (the SNORM-to-guest conversion in a compute pass, kept on the GPU and sampled in place or copied like the other GPU resolves) |
| 2 | The game's threads spinning in D3D's waits for the GPU | sub_827FF5D0 10.0 s + sub_82813C40 7.5 s own time (0.58 of a core): the D3D runtime's busy wait (`db16cyc` loop re-reading the GPU's progress at device+10896) | waits for ring space / GPU progress | Mostly a consequence of 1 (the GPU side is late); then a native wait (sleep on the renderer's progress) instead of the spin, generic for every title using this D3D build |
| 3 | Readbacks and GPU object churn | 6.4-6.8 colour + 6-7 depth resolves read back a frame; **9-10 buffers and 2 images created a frame (320-350 MB)**; 12-16 batches submitted a frame | `Copy`'s read-back path | Pool the staging buffers and images (RAM is limited on the target devices); fewer flushes once 1 keeps resolves on the GPU |
| 4 | Vulkan driver CPU time | `vkGetInstanceProcAddr` 3.25 s (the driver's own code, no symbols), `DrvPresentBuffers` 0.36 s, `OpenAdapter10` 0.25 s | submits and presents | Fewer submits (3); check per-draw driver calls |
| 5 | Texture view lookups | `unordered_map<array<uint32,8>, FastView>` 2.86 s | `TextureCache::View` fast path | A cheaper key/hash, or the last views a draw used cached by fetch constant |
| 6 | Shader lookups | `LoadedShader` map 1.42 s + `Shader` map 0.58 s | `CommandProcessor` per draw | Keep the bound shader's pointer while its address and hash are unchanged |
| 7 | Command processor and draw recording | `ExecutePacket` 1.58 s, `MarkWritten` 1.57 s, `VulkanRenderer::Draw` 1.13 s, `memmove` 0.86 s, `RecordJob` 0.43 s, `MemoryChanged` 0.27 s, `EnableAccessCallbacks` 0.2 s | renderer thread | After 1: measure again (record threads, MarkWritten's per-draw cost) |
| 8 | Game code and audio | sub_82373398 1.37 s, sub_8278B508 1.0 s, `SwitchToFiber` 1.8 s, `XmaWriteRegister` 0.69 s, `KeDelayExecutionThread` 0.26 s | game threads | Later: identify (fibers: the game's job system), the XMA register path's cost |
| 9 | Texture uploads | 5.2-5.6 a frame, 0.77 ms loading; the 16_16_16_16 resolve destination re-uploaded each frame | `LoadLevel` 0.48 s | Goes with 1 |

The order to work in: 1 (the bulk of the renderer thread's frame), then measure again; 3 with it; then 2, 5, 6.
