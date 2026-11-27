# Crackdown's "ready" natives, classified and bound (2026-11-27)

The roadmap listed 19 functions as native in Any360 but not bound in Crackdown, from the 2026-11-26 count. They were
read one by one: the generated code's callers of each and the natives that replace those callers.

| functions | what they are | done |
|---|---|---|
| D3D_ClearDraw, D3D_ClearRects | helpers of the target clear (sub_826BF8C0), which is bound natively (`ClearTargetOrOriginal`) and does their work itself | counted as covered |
| D3D_CopyPatchedShader, D3D_PatchedShaderCache, D3D_UpdateShaderControl, D3D_LinkShaders, D3D_PatchVertexFetches, D3D_EmptyShaderExports, D3D_RemapShaderExports | helpers of the shader flush (sub_826C6C48, `FlushShadersOrOriginal`), done by it natively (self-test cases `fetch_patch`, `shader_link`) | counted as covered |
| D3D_GpuTimestamp | written by the native swap packet (`SwapPacket`'s EVENT_WRITE_SHD) | counted as covered |
| D3DResource_LockCpu | already bound (`ResourceLockOrOriginal`, 0x826AC8F0) but below the D3D range of `titles/crackdown/modules.json` | range starts at 0x826AC6C8 (evidence in modules.json); counted as ported |
| D3DQuery_GetData, the D3D GPU wait step | held since 2026-11-19 (GetData's faster poll lowered the 1% lows with records on) and Crackdown's own GpuPoll hook | waiting on frame pacing (owner refresh-codbo) |
| GetLastError, SetLastError | measured without gain (docs/NATIVE_FUNCTIONS.md, 2026-11-11) | not worth binding |
| toupper | | bound |
| _finite, _sptype, _add_exp | | still ready: no run calls them |

The four C runtime functions were bound in one build and checked on the city route
(`--any360_xdk_verify=true`, crackdown_512): 187 M calls of every bound function compared with the functions' own
code, 0 different (`verify_last_line.txt`). `toupper` ran 63,383 times; `_finite`, `_sptype` and `_add_exp` were never
called, so they stay unbound (`_add_exp` keeps Crackdown's copy in its decomp sources until a run calls
it). `toupper` is the C locale's, the same instructions as in Test Drive Unlimited where it measured 7.9 -> 7.7 ns
(1.0x): bound, as docs/NATIVE_FUNCTIONS.md keeps functions that neither gain nor lose.

The in-game measure (`--any360_xdk_bench=true`, crackdown_513) crashed 3 s after start, in the D3D scissor native's
bench path (`RunD3dStateSlow` -> `ApplyScissor` -> `ScissorPacketsCall`, a read of guest address 0): a bug of the bench
mode for the D3D state setters, not of these functions; the normal and verify modes run (`bench_mode_crash.txt`).
