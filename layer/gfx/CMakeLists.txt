# any360::gfx: the GPU side of stage 1 (docs/DESIGN.md). `any360_gfx` is portable C++23 over any360_sys: the command
# processor (PM4 execution behind sys::GpuBackend) and the Renderer interface the native Vulkan renderer implements.
# Built from layer/sys/CMakeLists.txt, which adds this directory; `gfx_cp_tests` needs no reference runtime.
add_library(any360_gfx STATIC src/command_processor.cpp)
target_include_directories(any360_gfx PUBLIC include)
target_link_libraries(any360_gfx PUBLIC any360_sys)
target_compile_options(any360_gfx PRIVATE -Wall -Wextra)

add_executable(gfx_cp_tests tests/gfx_cp_tests.cpp)
target_link_libraries(gfx_cp_tests PRIVATE any360_gfx)
target_compile_options(gfx_cp_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_cp_tests COMMAND gfx_cp_tests)

# any360_xenos_shader: the Xenos microcode -> SPIR-V shader translator, vendored from the ReXGlue SDK (derived from
# Xenia; BSD-3, third_party/xenos_shader/LICENSE and NOTICE.md) with the glslang SPIR-V builder and header-only fmt.
# Vendored files keep their upstream paths; third_party/xenos_shader/shim/ holds our stand-ins for the SDK
# infrastructure the translator includes (logging, cvars, memory, render target cache...). Needs no Vulkan headers.
set(ANY360_XS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenos_shader)
set(ANY360_GLSLANG ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glslang)
add_library(any360_xenos_shader STATIC
            ${ANY360_XS}/src/core/string_buffer.cpp
            ${ANY360_XS}/src/graphics/format/ucode.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/shader.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/translator.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/translator_disasm.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_builder.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_translator.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
            ${ANY360_XS}/src/graphics/pipeline/shader/spirv_translator_rb.cpp
            ${ANY360_GLSLANG}/SPIRV/SpvBuilder.cpp
            ${ANY360_GLSLANG}/SPIRV/InReadableOrder.cpp
            ${ANY360_GLSLANG}/SPIRV/Logger.cpp)
# The shim and vendored include trees hold disjoint headers; SYSTEM keeps upstream warnings out of our targets.
target_include_directories(any360_xenos_shader SYSTEM PUBLIC ${ANY360_XS}/shim/include ${ANY360_XS}/include
                           ${ANY360_GLSLANG} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt/include)
target_compile_definitions(any360_xenos_shader PUBLIC FMT_HEADER_ONLY
                           $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS>)
# Third-party code: upstream warnings are not ours to fix.
target_compile_options(any360_xenos_shader PRIVATE -w)

add_executable(gfx_shader_tests tests/gfx_shader_tests.cpp)
target_link_libraries(gfx_shader_tests PRIVATE any360_xenos_shader)
target_compile_options(gfx_shader_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_shader_tests COMMAND gfx_shader_tests)

# any360_gfx_vulkan: the Vulkan side of the renderer. The loader is opened at run time (VK_NO_PROTOTYPES), so the
# build needs only the vendored Khronos headers (third_party/vulkan-headers) and no Vulkan SDK.
add_library(any360_gfx_vulkan STATIC src/vulkan/device.cpp)
target_include_directories(any360_gfx_vulkan PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include)
# The Vulkan idiom {sType} leaves the other fields zero on purpose.
target_compile_options(any360_gfx_vulkan PRIVATE -Wall -Wextra)
target_compile_options(any360_gfx_vulkan PUBLIC -Wno-missing-field-initializers)
if(NOT WIN32)
    target_link_libraries(any360_gfx_vulkan PUBLIC ${CMAKE_DL_LIBS})
endif()

# On a real driver: the translated shaders compiled into pipelines, an offscreen target drawn and read back.
add_executable(gfx_vulkan_tests tests/gfx_vulkan_tests.cpp)
target_link_libraries(gfx_vulkan_tests PRIVATE any360_gfx_vulkan any360_xenos_shader)
target_compile_options(gfx_vulkan_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_vulkan_tests COMMAND gfx_vulkan_tests)
