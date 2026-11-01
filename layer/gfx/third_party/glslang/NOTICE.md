# glslang SPIR-V builder (vendored)

The subset of Khronos glslang's `SPIRV/` directory that the Xenos shader translator
(`../xenos_shader`) uses to build SPIR-V modules, taken unchanged from the copy bundled in the
ReXGlue SDK 0.10.0 (`thirdparty/glslang`, https://github.com/KhronosGroup/glslang).

Files: `SPIRV/SpvBuilder.h`, `SpvBuilder.cpp`, `spvIR.h`, `InReadableOrder.cpp`, `Logger.h`, `Logger.cpp`,
`spirv.hpp`, `GLSL.std.450.h`, `hex_float.h`, `bitutils.h`.

License: see `LICENSE.txt` (glslang's license file, copied unchanged). These files are under the
Khronos / LunarG BSD-3-Clause and MIT-style terms stated in their own headers and in `LICENSE.txt`.
