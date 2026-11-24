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

A first route (137 s, with jumps) fell off now and then on replay (one of two runs fell ~70 s in: Mirror's Edge's
physics follows each frame's time, the replay the clock). A second, safe route (route 2: ~70 s of play) followed the
recording in every replay checked: three replays' draws a frame within ~2% of the recording's at every 10 s report, the
frames at 111 s the same place and view ([route2_record_vs_replays_111s.jpg](route2_record_vs_replays_111s.jpg)); 11 of
12 A/B launches after. Record short, safe routes.
