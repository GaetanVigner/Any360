# 2026-11-24: recorded runs (input record and replay)

So that A/B measures can run a player's route without the player (`any360/sys/input.h` `InputRecording`,
`--any360_input_record`, `--any360_input_replay`, `--any360_input_record_from`, `--any360_input_record_seconds`;
docs/SYS_BINDING.md).

In-game check, Mirror's Edge (1440p, host MSAA 4x): the owner played from the launch, through the menus (the level
chosen from the level selection, not the campaign's continue) and into the level, recorded from 0 s (4857 changes,
185 KB, the last at 137 s). Played back minimized with no player, the frame at 157 s after the launch, next to the
recorded run's at 157.7 s ([record_vs_replay_157s.jpg](record_vs_replay_157s.jpg)): the same place on the same
rooftop, the same tutorial prompt; the camera ~4% of the width apart (a slightly different heading after 2.5 minutes
of menus, loading and play).
