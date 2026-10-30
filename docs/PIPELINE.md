# The Any360 pipeline: decompile, replace, recompile

How a title goes from a running ReXGlue recompilation to native code, one function or one module at a time, with the
recompiled game as the oracle. The driver is [pipeline/any360.py](../pipeline/any360.py); each title describes where
its files are in `titles/<title>/pipeline.json` (nothing of the game enters this repository).

```text
 decrypted XEX image ──► Ghidra project (PowerPC:BE:64, image base 0x82000000)
        │                    seeded with the recompiler's 41k/82k function starts, auto-analysed
        │                                   │
 generated code ──► inventory ──────────────┼──► decompile <functions | module>  ──►  <title>/src/decomp/raw/*.c
 (DEFINE_REX_FUNC,  functions-<title>.json  │                                              │ clean-up with recovered types
  // mnemonics)     status N/V/L/R          │                                              ▼
        │                                   └──► bind <functions> ──► <module>_bindings.cpp: BIND(address, handler)
 port sources ──► translated so far                                      handler calls `original` until the body is in
 (BIND / REX_HOOK / REX_FUNC)                                                              │
                                                                         verify ──► sandbox golden test: handler == original
                                                                                           │
                                                           recompile the title: generated code + native replacements
```

## Steps

| Step | Command | Produces |
|---|---|---|
| inventory | `python pipeline/any360.py <title> inventory` | `docs/data/functions-<title>.json` (every function: address, instructions, state N native / V verified / L library / R recompiled, handler, file), `docs/coverage/inventory-<title>.json` (modules, translated functions, imports) |
| import | `python pipeline/any360.py <title> import` | the Ghidra project: `BinaryLoader` at the image base, `SeedFunctions.java` creates and names every function the recompiler found (so cross-references and the decompiler work on the same function boundaries as the generated code), then Ghidra's auto-analysis |
| decompile | `python pipeline/any360.py <title> decompile sub_826B9C08 826C3750 "XDK D3D runtime"` | `Decompile.java` writes the C of each function (or of every function of a module from `titles/<title>/modules.json`) to `<decomp_dir>/raw/` |
| bind | `python pipeline/any360.py <title> bind 826B9C08 --module renderer --purpose "..."` | `<decomp_dir>/<module>/<module>_bindings.cpp`: one handler per function that calls the original, one `BIND(address, handler)` per function; `goldens.txt` lists what to verify |
| verify | `python pipeline/any360.py <title> verify` | runs the title's sandbox golden tests (`verify_command`): each bound function on recorded inputs against the recompiled original |
| status | `python pipeline/any360.py <title> status` | the counts per module; `tools/progress_page.py` draws them |

The binding mechanism is the one the Crackdown port uses today: `CRACKDOWN_BIND(address, handler)` makes the
generated `sub_address` jump to `handler(ctx, base, original)`; the handler can call the original, so a function is
bound first (no behaviour change), then its decompiled body replaces the call, then the golden test proves them equal.
Any360's `ANY360_BIND` (planned, `src/any360/bind.h`) is the same macro without the title's name.

## What the oracle gives

- **Function boundaries and names**: the recompiler's analysis (`DEFINE_REX_FUNC`) seeds Ghidra, so a decompiled
  function is exactly the function the generated code has, and `sub_X` names match on both sides.
- **Reference behaviour**: the guest sandbox (Crackdown `tests/guest_sandbox.h`) runs a recompiled function on seeded
  inputs in a lazily committed 4 GB guest space and hashes its outputs; the same inputs through the native handler
  must hash the same. Today for leaf functions (293 D3D runtime leaves in the Crackdown goldens); non-leaf functions
  need callees still recompiled and recorded memory snapshots, the next step of the sandbox.
- **Call graph and modules**: the inventory's module ranges start from what is known (XDK D3D runtime, CRT, import
  thunks); Ghidra's analysis over the seeded functions adds vtables, RTTI class names and strings, which name the
  1 MB areas (`code 82Xxxxxx`) the inventory shows until then.

## Which functions first

1. **Library ranges** are not decompiled: the XDK D3D runtime becomes `any360::gfx`, the CRT the host CRT, Havok a
   shared decompiled copy. Recognising them by signature (hash of normalised bytes → role) is what makes this
   reusable across titles.
2. **Leaf functions with goldens** (math, string, container helpers): cheapest to decompile and verify, and they are
   the dependencies of everything else.
3. **The functions the port already binds** (Crackdown: 103, frame pacing, physics steps, render passes, audio;
   Simpsons: 27, clock, ring waits, outlines): their handlers exist, their decompiled bodies document what the hook
   changed.
4. **Modules by call-graph closure**: a subsystem whose functions only call each other and the libraries (the
   inventory's instruction counts weight the treemap; the call graph from Ghidra gives the closure).

## First run on Crackdown (2026-10-30)

`import` on the 23 MB decrypted image: 41,476 functions seeded (the recompiler's 41,219 plus 257 call targets the
generated code names as helpers: the XDK's `__savegprlr_N` / `__restfpr_N` register save and restore routines),
0 failed, auto-analysis 205 s. Two things the first attempt taught, now in `SeedFunctions.java`:

- the save/restore helpers are entered at several points and end in `blr`; Ghidra's "Non-Returning Functions -
  Discovered" analyzer marked them non-returning and every function that called one was cut after its prologue (an
  8-byte `sub_826B9C08` instead of the D3D draw function). The analyzer is switched off and the helpers are created as
  returning functions;
- functions must be seeded before analysis (`-preScript`), or the analysis finds only the entry-point-reachable code.

`decompile sub_82130000 sub_824A1708 sub_826B9C08 sub_8225AB30` then gives readable C whose control flow matches the
generated code (the loop of `sub_82130000` calling `param_4` `param_3` times). Left to improve: the helpers appear as
calls (`helper_82ABDF18()`) whose register effects the decompiler cannot see, so a clean-up pass should treat them as
prologue/epilogue no-ops (a custom calling convention or a `__savegprlr` signature set); and VMX128 encodings are not
decoded by the stock `A2ALT` language (log: "Unable to resolve constructor" on vector code), which needs Ghidra's Xenon
processor extension; until then those functions decompile with gaps and the generated C++ is the reference for them.

## Any title

`titles/<title>/pipeline.json` is all the pipeline needs: image, image base, generated code, port sources, bind
macro, Ghidra path and language, project and output directories. The language `PowerPC:BE:64:A2ALT-32addr` covers
the Xenon's 64-bit PowerPC with 32-bit addressing and AltiVec; VMX128-specific encodings decompile with Ghidra's
Xenon extension when installed (the recompiled C++ of those instructions is in the generated code either way).
