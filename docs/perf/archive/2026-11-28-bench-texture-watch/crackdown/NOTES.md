# Crackdown with the texture watch in kick order and the target memo (2026-11-28)

The two gains of the bench title checked on Crackdown together: 6841697 + 1bb8e99 (CPU writes to watched textures
applied in kick order, made visible at waits on memory) and e30f379 (targets taken as they were when depth sits below
the scissor). Also in "after": 6eec1c3 (batch rings start small).

- before: Any360 fce30d9 (the parent of 6841697), Crackdown dfab703.
- after: Any360 e30f379 (main), the same Crackdown.
- Route: title screen to the Agency's hall by the scripted input, `12:start,...,50:a,60:b` (the 60 s B press closes
  the supply point's menu, which a faster load lets the 50 s A press open), 130 s a launch, uncapped
  (`--crackdown_frame_rate=0`, `--any360_gpu_vblank_hz=1000`), 1280x720, RTX 5070 Ti, the mask held at 0xFF
  (`--any360_cpu_affinity=0xFF`, checked at the end of every launch). 5 launches a variant, alternating.
- Figures: the game's own swaps (`Crackdown: guest frames`, every 10 s), the first period dropped, the lowest 1%-low
  period of each launch apart as its dip; `summary.txt` (aggregate.py of `../../2026-11-26-any360-vs-rexglue` in the
  Crackdown repository), `runs.txt`, `logs/` (frame lines, errors).

The fifth "after" launch (crackdown_535) is discarded: it logged "swapchain out of date (resize)" at startup and
recreated its swapchain, after which its batch latency stayed at 7-10 ms, every read of a resolve's destination waited,
and its draws spent 4.0-4.7 ms a frame in textures (0.5-0.7 in every other launch). Rule since: a launch whose startup
logs "swapchain out of date" is dropped. Without it (`summary_without_535.txt`, `runs_without_535.txt`):

| variant | launches | average FPS | median frame | 10% low | 1% low | dip 1% low | private MB |
|---|---:|---:|---:|---:|---:|---:|---:|
| before | 5 | 190.3 | 5.21 ms | 164.2 | 135.8 | 86.3 | 2,775 |
| after | 4 | 223.4 | 4.50 ms | 191.3 | 150.3 | 88.7 | 2,463 |
| rank test (z, after against before) | | +2.45 | | +2.45 | +1.71 | | -2.45 |

- Every "after" launch runs faster than every "before" one (209.8-229.9 against 189.6-201.1 FPS): +17% on the
  average, +17% on the 10% low; the 1% low +11%, short of a rank test's 2 with 4 against 5.
- With all ten launches, 535 included (`summary.txt`): 222.1 against 190.3 FPS, z +1.57.
- Private memory: about 300 MB less with "after" (z -2.45 without 535, -2.61 with it).
- Crackdown's uploads stay at 1.5 a frame in both variants (the bench's 8.2 -> 1.0 came from one texture rewritten
  through the CPU view every frame; Crackdown's hall has none like it).
- Image: the Any360 frame screenshots of both variants at the same spot show the same picture (kept outside the
  repository: title data). No stale or late texture seen there, glyphs included; the route does not drive through
  the city at speed, so streaming at speed is not covered by this check.
- Hangs 0, errors 0 in all ten launches.
