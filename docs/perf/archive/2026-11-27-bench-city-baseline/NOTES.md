# any360_bench city: first baseline (2026-11-27)

The bench title's first scene (docs/BENCH.md), measured as it lands. Raw lines in `runs.txt`.

- Workload a frame: 2,530 draws (about 2,000 in the main view after culling, 512 in the minimap pass), 2 resolves,
  73 K PM4 dwords, one 256 KB 8888 texture rewritten by the CPU (and a DXT1 one every 60 frames), audio at its clock.
- NVIDIA RTX 5070 Ti, whole machine shared with other sessions: 280-330 FPS, median 2.92-3.17 ms (about 8% apart
  between launches), 1% lows 207-252 FPS; CPU about 280% of a core, GPU 8-11%. The frame is bound by the renderer's
  thread (draws 2.7 ms a frame, 1.1 us a draw), not the GPU.
- What stands out for optimisation:
  - texture uploads: 8.5 a frame for one texture the CPU rewrites (0.84 ms a frame in textures): the watch reports
    each 4 KB page as the CPU writes it, so the texture is re-read several times while it is being rewritten;
  - draws: 1.1 us of the renderer's thread a draw (submit 0.49 ms a frame);
  - the command processor: 0.45 ms a frame for 73 K dwords;
  - the title's render thread waits by yielding (`KeDelayExecutionThread` with a zero interval) on the fence and the
    ring: 13 K system calls a frame, almost all of them those yields.
- Private memory: 1,132 MB before the title starts (the renderer's mirror of the 512 MB physical memory and the
  device), 1,367 MB at the end.
- The small configuration (`--any360_cpu_affinity=0x3 --any360_gpu_device=Radeon`, 600 frames, 2026-11-28): 100 and
  157 FPS in two launches, median 7.5 / 5.4 ms, 1% lows 23 / 27 FPS, 65 and 15 frames over 16.7 ms; the process used
  only 33-75% of its 2 processors: threads sleep while frames are late (the title's yields, the renderer's waits).
  Texture uploads fall to 1.0 a frame there (8.5 on 16 processors): the extra uploads come from uploads racing the
  CPU's rewrite.
- Not measured yet: windowed mode.
