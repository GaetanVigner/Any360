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

| variant | average FPS | median frame | 10% low | 1% low | dip 1% low | private MB |
|---|---:|---:|---:|---:|---:|---:|
| before | 190.3 | 5.21 ms | 164.2 | 135.8 | 86.3 | 2,775 |
| after | 222.1 | 4.52 ms | 190.3 | 145.1 | 86.9 | 2,466 |
| rank test (z, after against before) | +1.57 | | +1.57 | +0.94 | | -2.61 |

- Four of the five "after" launches run faster than every "before" one (209.8-229.9 against 189.6-201.1 FPS). The
  fifth (crackdown_535) spent 4.0-4.7 ms a frame in textures for its whole run (0.5-0.7 ms in every other launch of
  both variants) at 183.7 FPS, with a 1% low of 72.5; the z values stay under 2 because of it. It is the only launch
  with 0.8 texture uploads a frame instead of 1.5. Not explained yet: one launch in five, to watch for.
- Private memory: about 300 MB less with "after" (z -2.61).
- Crackdown's uploads stay at 1.5 a frame in both variants (the bench's 8.2 -> 1.0 came from one texture rewritten
  through the CPU view every frame; Crackdown's hall has none like it).
- Image: the Any360 frame screenshots of both variants at the same spot show the same picture (kept outside the
  repository: title data). No stale or late texture seen there, glyphs included; the route does not drive through
  the city at speed, so streaming at speed is not covered by this check.
- Hangs 0, errors 0 in all ten launches.
