# Crackdown and The Simpsons Game: what each needs from Any360

Generated numbers: [coverage/crackdown.md](coverage/crackdown.md), [coverage/simpsons.md](coverage/simpsons.md),
[coverage/COMPARISON-crackdown-vs-simpsons.md](coverage/COMPARISON-crackdown-vs-simpsons.md) (2026-10-30). Crackdown
measured on its `generated/default` (ReXGlue v0.10.0 codegen with the port's codegen patches) against the port's
patched v0.10.0 SDK tree; The Simpsons Game on the generated code committed in its repository (ReXGlue v0.8.0, vendored
fork) against that fork, at commit `4ff637bc`.

## The two recomps side by side

| | Crackdown (2007, Realtime Worlds) | The Simpsons Game (2007, EA Redwood Shores) |
|---|---|---|
| ReXGlue | v0.10.0 pinned, 120 patches re-applied at build | v0.8.0 vendored and modified in place (`tools/rexglue-sdk`) |
| Recompiled functions | 41,219 (177 units) | 81,874 (90 units) |
| Functions the port replaces | 103 (`CRACKDOWN_BIND`, `REX_HOOK_RAW`) | 27 (`REX_FUNC`) |
| Kernel / XAM imports | 248: 223 implemented, 25 stubs | 317: 270 implemented, 47 stubs |
| Instructions (static) | 2,668,462 in 340 mnemonics, all handled | 2,942,906 in 338 mnemonics, all handled |
| VMX / VMX128 share | 3.5% | 3.2% |
| Floating point share | 8.1% | 6.6% |
| Renderer direction | game-side draw records from D3D hooks → Vulkan native passes (shadow atlas native) | PM4 decoded in the plugin, Vulkan backend converted piece by piece; D3D11 backend added for Windows |
| Function discovery | `find-missing-functions.py` (pointer and vtable scan of the image) | `LD_PRELOAD` trap library + hand-kept `functions_patch.toml` |

## Kernel and XAM

225 imports are common to both titles and the SDK implements almost all of them. The differences:

- **The Simpsons engine talks to the XMA decoder through the kernel** (`XMAInitializeContext`, `XMASetInputBuffer*`,
  `XMAGetOutputBuffer*Offset`, 14 calls), where Crackdown's audio goes through the XAudio runtime inside the game.
  The XMA context patches of the Crackdown set (0007 synchronisation, 0050 acquire loads, 0096 stall guards) were
  written against XAudio's use of the same contexts; they are the first SDK patches to validate on the Simpsons build.
- **Storage and content**: `XamContent*` (device enumeration, thumbnails, license mask, `XamContentInstall` is a stub),
  `ExAllocatePool*`, `NtReadFileScatter`, `NtQueueApcThread`, `ObOpenObjectByName`. Crackdown instead uses the `StfsCreateDevice`
  / `StfsControlDevice` pair and the `XamTask*` scheduler.
- **Stubs**: both titles import Xbox Live (`NetDll_*`, `XNetLogon*`), the Simpsons title twice as much (overlapped
  Winsock, QoS, DNS) plus 13 dashboard dialogs (`XamShow*UI`). None of these needs a native implementation for a
  single-player port; the layer's job is to make the stubs return what makes the title take its offline path, as the
  Crackdown port did for `XamEnumerate` (patch 0060).
- **One GPU-side stub only Simpsons hits**: `VdQuerySystemCommandBuffer` / `VdSetSystemCommandBuffer`. The XDK uses the
  system command buffer for the ring's initial state; if the title reads it back, the plugin must provide it.
- Nothing is missing outright in either SDK tree: the Simpsons fork's 0.8.0 kernel and stock 0.10.0 provide the same set
  (2,590 vs 2,591 export definitions), so moving the Simpsons recomp to the pinned 0.10.0 costs nothing on the kernel side.

## CPU instructions

Every mnemonic either title's code uses has a builder in the recompiler; neither generated tree contains a
`REX_UNIMPLEMENTED` marker. Of ReXGlue's 1,842 opcode ids, 497 have builders: the rest are POWER, server and privileged
forms a game never compiles to.

The families match within a couple of percent: a third loads and stores, a quarter integer, a fifth branches. The
Simpsons code uses 12 mnemonics Crackdown does not, and they are a coherent group: **saturating integer VMX**
(`vaddshs` 212, `vaddsws` 193, `vsubsws` 159, `vpkshus` 152, `vpkuwus` 12) and element loads (`lvewx` 278), the kind
of code a software audio mixer or an animation decompressor compiles to. All handled, none yet covered by the sandbox
golden tests that checked `vrsqrtefp` and the FMA forms bit-exact for Crackdown: that is the first test to add when the
sandbox moves into the layer. Crackdown's 15 own mnemonics are rare (`bso`/`bns` overflow branches, `frsqrte`, record
forms of VMX compares); its `twllei`/`twlgei` trap instructions (816) and `mtmsrd`/`mfmsr` (311) are the XDK's own.

The lone `a` reported for the Simpsons tree is a comment artefact: its codegen writes prose comments into the output
and one line begins with the English word, which is also a POWER mnemonic. The tool ignores comments followed by
another comment; that line is followed by code.

## GPU

Not measured statically. The Crackdown render pass map (29 passes, 86% of GPU draws reproduced exactly by the records,
the shadow atlas drawn natively and equal to the emulated depth for static geometry) has no Simpsons counterpart yet;
its repository documents the facts its renderer relies on (`frame_trace_semantics.md`: memory records after the packet
that reads them, tiled and endian-swapped resolve destinations, 20e4 depth with bias 15, post-process chains needing
convergence runs) and they match what Crackdown measured. The planned GPU census (DESIGN.md 4.2) runs the same
`gpu_frame_stats` / draw trace on both builds and gives the first per-feature table.

## What this means for Any360

1. The kernel and CPU sides are not where a second title costs effort at stage 0: Any360 carries the SDK patch series
   and the sandbox tests, and the Simpsons title adds saturating VMX goldens and an XMA validation run. At stage 1 the
   317 imports are the specification of `any360::sys`, and 225 of them are shared.
2. The renderer is, and so is stage 2: twice the functions to decompile. The two projects built different record
   sources on the same facts; Any360's record model and `HleFrameAbi` are the common form, and the XDK binding by
   signature is what lets the Crackdown hooks run on the Simpsons XDK build (its project already matched 174 XDK
   functions by signature) and what recognises the libraries not to decompile.
3. The Simpsons repository's vendored SDK fork is the one structural obstacle: its renderer work must become patches on
   the pinned baseline before the two titles can share one series.
