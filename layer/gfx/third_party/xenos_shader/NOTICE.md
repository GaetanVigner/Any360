# Xenos shader translator (vendored)

The Xenos (Xbox 360 GPU) microcode analyzer and the microcode-to-SPIR-V translator, taken from the
ReXGlue SDK 0.10.0 source tree (`rexglue-sdk-src`), which derives them from the Xenia project.
Built by `layer/gfx/CMakeLists.txt` as the static library `any360_xenos_shader`.

License: BSD 3-Clause, reproduced in `LICENSE` next to this file (copied unchanged from the SDK's
`LICENSE`):

    Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>
    Portions of this software are derived from the Xenia project:
    Copyright (c) 2022, Ben Vanik and Xenia project contributors
    https://xenia.jp

Each vendored file also keeps its original header.

## Taken unchanged (same relative paths as in the SDK)

- `include/rex/assert.h`, `platform.h`, `math.h`, `types.h`, `literals.h`, `string/buffer.h`
- `include/rex/graphics/xenos.h`, `registers.h`, `register_table.inc`, `format/ucode.h`
- `include/rex/graphics/pipeline/shader/shader.h`, `translator.h`, `spirv.h`, `spirv_builder.h`
- `src/core/string_buffer.cpp`
- `src/graphics/format/ucode.cpp`
- `include/rex/graphics/pipeline/shader/interpreter.h`, `src/graphics/pipeline/shader/interpreter.cpp` (the
  microcode interpreter any360::gfx runs a vertex shader on the CPU with, to know how far down a draw without
  clipping reaches: `layer/gfx/src/vulkan/draw_extent.cpp`)
- `src/graphics/pipeline/shader/shader.cpp`, `translator.cpp`, `translator_disasm.cpp`, `spirv.cpp`,
  `spirv_builder.cpp`, `spirv_translator_alu.cpp`, `spirv_translator_fetch.cpp`,
  `spirv_translator_memexport.cpp`, `spirv_translator_rb.cpp`

## Taken with changes (each marked `// any360:` in the file)

- `include/rex/graphics/pipeline/shader/spirv_translator.h`: the `#include <rex/ui/vulkan/device.h>` and the
  `Features(const ui::vulkan::VulkanDevice*)` constructor declaration are removed. Any360 has no ReXGlue
  Vulkan device; the host builds `Features(bool all)` and sets the fields from its own device queries.
- `src/graphics/pipeline/shader/spirv_translator.cpp`: the definition of that constructor is disabled with
  `#if 0 // any360:` ... `#endif // any360`. Calls and returns are implemented (the reference left them
  unimplemented: a called function never ran): a stack of up to four return addresses, a call jumps like a jump
  after pushing the address of the instruction after it, a return pops it (outside a function it goes on).
- `src/graphics/pipeline/shader/translator.cpp`: the instruction after a call or a return is a label (where the
  return goes back to).
- `include/rex/graphics/pipeline/shader/spirv_translator.h`: the call and return overrides and the call stack
  variables.
- Alpha to coverage with guest samples as pixels (`SetSamplesAsPixels`, in `spirv_translator.h`,
  `spirv_translator.cpp` and `spirv_translator_rb.cpp`): the reference sets one bit a host sample in gl_SampleMask;
  Any360 keeps each guest sample in its own host pixels, so the fragment's guest pixel and sample come from its
  position and an uncovered fragment is discarded, with the reference's thresholds and dither offsets; no sample
  mask output is declared then.
- `spirv_translator.cpp` `StoreResult`: a position export in the memexport compute translation
  (`HostVertexShaderType::kMemExportCompute`, which has no gl_PerVertex output) is dropped instead of crashing the
  translation; the reference's Vulkan backend never used that translation.
- `spirv_translator_fetch.cpp`: `getBCF` (kGetTextureBorderColorFrac), unimplemented in the reference (a
  translation error), is worked out from the coordinates along getWeights' path: the magnification filter's taps
  outside the texture on axes clamped to the border, at the base level.
- `spirv_translator.h` `Features::implicit_lod_sampling` and `spirv_translator_fetch.cpp`: with it (off by default,
  the reference's translation), a pixel shader's computed-LOD fetch without register gradients samples with the
  implicit LOD and the fetch's LOD as the bias, instead of the coarse derivatives scaled by 2^LOD as explicit
  gradients: the same level, fewer texture address cycles (docs/GFX.md, "Implicit-LOD sampling").
- `spirv_translator.h` `Features::zero_multiply_by_comparisons`, `IsEitherZero`, and `spirv_translator_alu.cpp`
  (`ZeroIfAnyOperandIsZero`, the vector mul/mad path): with it (off by default, the reference's form), the Xenos
  multiply rule is `(a == 0 || b == 0) ? 0 : a * b` instead of `min(|a|, |b|) == 0 ? 0 : a * b`: the same results,
  fewer instructions where the driver does not fold the reference's form into a native multiply (docs/GFX.md, "The
  multiply rule by comparisons").
- `spirv_translator.h` `Features::dot_product_rule_on_zero_or_nan` and `spirv_translator_alu.cpp` (dp4, dp3,
  dp2add): with it (off by default), a dot product sums plain products, and only invocations whose sum is zero or
  NaN compute it again with the multiply rule on each product (a branch the others skip); the results agree with the
  rule's (docs/GFX.md, "Dot products: the rule where it can change the sum").

## Ours: `shim/include/rex/...`

Stand-ins for SDK infrastructure the translator sources include, holding only what they use:

- `rex/memory.h`: `memory::Reinterpret`, `copy_and_swap` (portable loop), `fourcc_t` / `make_fourcc`
  (from `rex/memory/utils.h`); `memory::Memory` reduced to `physical_membase()` over a base the caller gives (the
  interpreter's vertex fetches read guest memory there).
- `rex/graphics/register_file.h`: `RegisterFile` as a view over a register array the caller keeps (any360::gfx's)
  with the SDK's accessors (`operator[]`, `Get`, `GetVertexFetch`), for the interpreter.
- `rex/string.h`: standard headers only (nothing of it is used).
- `rex/filesystem.h`: `filesystem::OpenFile`.
- `rex/logging.h`: `REXGPU_ERROR` prints to stderr; the other `REXGPU_*` levels are no-ops.
- `rex/graphics/flags.h`: `REXCVAR_GET(dump_shaders)` frozen to its default (empty: no ucode dumps).
- `rex/graphics/util/draw.h`: `draw_util::kD3D10StandardSamplePositions2x/4x` (values from `draw.cpp`).
- `rex/graphics/pipeline/render_target/cache.h`: `RenderTargetCache` reduced to the pixel shader interlock
  color format flags and `AddPSIColorFormatFlags`, copied verbatim.

## Dependencies (vendored next to this directory)

- `../glslang/SPIRV`: the glslang SPIR-V builder (`spv::Builder`), see `../glslang/NOTICE.md`.
- `../fmt`: {fmt} 12.1, used header-only (`FMT_HEADER_ONLY`), see `../fmt/NOTICE.md`.

No Vulkan or SPIRV-Headers headers are needed: glslang's `SPIRV/spirv.hpp` and `SPIRV/GLSL.std.450.h`
provide the SPIR-V enums.
