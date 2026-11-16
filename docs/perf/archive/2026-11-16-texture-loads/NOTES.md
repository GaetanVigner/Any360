Texture loads, 2026-11-16 (Crackdown, scripted route into the city, minimized).

1. Tiled 1 and 2-byte texels grouped (8 blocks a 16 or 8-byte load): untile_ab10.txt (before/after, 5 each) and
   untile_ab12.txt (before, after, after under the name crackdown.exe, 4 each) with their frame_lows outputs.
   Inconclusive: the one 28 ms frame of the dip came and went in both binaries (old 6 of 9 launches, new 5 of
   13), and the textures uploaded in game turned out to be linear, which that change did not touch.
2. slowest_frames.txt: the slowest frame of each 10-second report with what the frame cost (command processor
   busy time, uploads and their loading, images created, staging buffers, pipelines) and the uploads by loading
   time. In the city the 22-28 ms frames have the command processor busy 1.4-3 ms: the stall is the title's or
   any360::sys's, not the renderer's. The k_8 1280x720 textures re-uploaded there are linear, 3.1-3.7 ms each.
3. Linear rows 16 bytes at a time: linear_ab10.txt (before = the build with the diagnostics, 5 each) and
   linear_frame_lows.txt.
