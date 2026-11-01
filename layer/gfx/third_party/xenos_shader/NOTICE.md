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
- `src/graphics/pipeline/shader/shader.cpp`, `translator.cpp`, `translator_disasm.cpp`, `spirv.cpp`,
  `spirv_builder.cpp`, `spirv_translator_alu.cpp`, `spirv_translator_fetch.cpp`,
  `spirv_translator_memexport.cpp`, `spirv_translator_rb.cpp`

## Taken with changes (each marked `// any360:` in the file)

- `include/rex/graphics/pipeline/shader/spirv_translator.h`: the `#include <rex/ui/vulkan/device.h>` and the
  `Features(const ui::vulkan::VulkanDevice*)` constructor declaration are removed. Any360 has no ReXGlue
  Vulkan device; the host builds `Features(bool all)` and sets the fields from its own device queries.
- `src/graphics/pipeline/shader/spirv_translator.cpp`: the definition of that constructor is disabled with
  `#if 0 // any360:` ... `#endif // any360`.

## Ours: `shim/include/rex/...`

Stand-ins for SDK infrastructure the translator sources include, holding only what they use:

- `rex/memory.h`: `memory::Reinterpret`, `copy_and_swap` (portable loop), `fourcc_t` / `make_fourcc`
  (from `rex/memory/utils.h`), forward declaration of `memory::Memory`.
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
