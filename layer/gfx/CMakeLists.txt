# any360::gfx: the GPU side of stage 1 (docs/DESIGN.md). `any360_gfx` is portable C++23 over any360_sys: the command
# processor (PM4 execution behind sys::GpuBackend) and the Renderer interface the native Vulkan renderer implements.
# Built from layer/sys/CMakeLists.txt, which adds this directory; `gfx_cp_tests` needs no reference runtime.
add_library(any360_gfx STATIC src/command_processor.cpp src/capture.cpp src/async_renderer.cpp src/frame_counter.cpp)
target_include_directories(any360_gfx PUBLIC include)
target_link_libraries(any360_gfx PUBLIC any360_sys any360_xdk)  # any360_xdk: the D3D records' format (d3d_records.h)
target_compile_options(any360_gfx PRIVATE -Wall -Wextra)
target_compile_definitions(any360_gfx PRIVATE $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS>)

add_executable(gfx_cp_tests tests/gfx_cp_tests.cpp)
target_link_libraries(gfx_cp_tests PRIVATE any360_gfx)
target_compile_options(gfx_cp_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_cp_tests COMMAND gfx_cp_tests)

# The frame counter (--any360_fps_overlay): its statistics, text and bitmap against reference models; --bench times
# what it costs a frame, on and off.
add_executable(gfx_overlay_tests tests/gfx_overlay_tests.cpp)
target_link_libraries(gfx_overlay_tests PRIVATE any360_gfx_vulkan $<$<BOOL:${WIN32}>:psapi>)
target_compile_options(gfx_overlay_tests PRIVATE -Wall -Wextra)
target_compile_definitions(gfx_overlay_tests PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN>)
add_test(NAME gfx_overlay_tests COMMAND gfx_overlay_tests)

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
            ${ANY360_XS}/src/graphics/pipeline/shader/interpreter.cpp
            ${ANY360_GLSLANG}/SPIRV/SpvBuilder.cpp
            ${ANY360_GLSLANG}/SPIRV/InReadableOrder.cpp
            ${ANY360_GLSLANG}/SPIRV/Logger.cpp)
# The shim and vendored include trees hold disjoint headers; SYSTEM keeps upstream warnings out of our targets.
target_include_directories(any360_xenos_shader SYSTEM PUBLIC ${ANY360_XS}/shim/include ${ANY360_XS}/include
                           ${ANY360_GLSLANG} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt/include)
# Our fmt copy lives in an inline namespace of its own, so a title that links its own fmt (another 12.x, say) keeps
# both apart instead of mixing two definitions of the same symbols.
target_compile_definitions(any360_xenos_shader PUBLIC FMT_HEADER_ONLY
                           "FMT_BEGIN_NAMESPACE=namespace fmt { inline namespace any360_v12 {" "FMT_END_NAMESPACE=}}"
                           $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS>)
# Third-party code: upstream warnings are not ours to fix.
target_compile_options(any360_xenos_shader PRIVATE -w)

add_executable(gfx_shader_tests tests/gfx_shader_tests.cpp)
target_link_libraries(gfx_shader_tests PRIVATE any360_xenos_shader)
target_compile_options(gfx_shader_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_shader_tests COMMAND gfx_shader_tests)

# any360_gfx_vulkan: the Vulkan side of the renderer. The loader is opened at run time (VK_NO_PROTOTYPES), so the
# build needs only the vendored Khronos headers (third_party/vulkan-headers) and no Vulkan SDK.
add_library(any360_gfx_vulkan STATIC src/vulkan/device.cpp src/vulkan/renderer.cpp src/vulkan/textures.cpp
            src/vulkan/presenter.cpp src/vulkan/gamma_shader.cpp src/vulkan/depth_shader.cpp src/vulkan/depth_word_shader.cpp src/vulkan/draw_extent.cpp
            src/vulkan/scale_shader.cpp
            src/vulkan/write_back.cpp)
target_link_libraries(any360_gfx_vulkan PUBLIC any360_gfx any360_xenos_shader)
target_include_directories(any360_gfx_vulkan PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include)
# The Vulkan idiom {sType} leaves the other fields zero on purpose.
target_compile_options(any360_gfx_vulkan PRIVATE -Wall -Wextra)
target_compile_options(any360_gfx_vulkan PUBLIC -Wno-missing-field-initializers)
if(NOT WIN32)
    target_link_libraries(any360_gfx_vulkan PUBLIC ${CMAKE_DL_LIBS})
endif()

# The resolve write-back against its first version (no GPU needed); --bench times both.
add_executable(gfx_write_back_tests tests/gfx_write_back_tests.cpp)
target_link_libraries(gfx_write_back_tests PRIVATE any360_gfx_vulkan)
target_compile_options(gfx_write_back_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_write_back_tests COMMAND gfx_write_back_tests)

# On a real driver: the translated shaders compiled into pipelines, an offscreen target drawn and read back.
add_executable(gfx_vulkan_tests tests/gfx_vulkan_tests.cpp)
target_link_libraries(gfx_vulkan_tests PRIVATE any360_gfx_vulkan any360_xenos_shader)
target_compile_options(gfx_vulkan_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_vulkan_tests COMMAND gfx_vulkan_tests)

# End to end: hand-written PM4 through the command processor into the Vulkan renderer, pixels read back.
add_executable(gfx_render_tests tests/gfx_render_tests.cpp)
target_link_libraries(gfx_render_tests PRIVATE any360_gfx_vulkan)
target_compile_options(gfx_render_tests PRIVATE -Wall -Wextra)
add_test(NAME gfx_render_tests COMMAND gfx_render_tests)
# The same scenes with every draw marked for the GPU and described (ANY360_GPU_CHECKPOINTS, the lost-device
# diagnostic): the diagnostic must not change a pixel.
add_test(NAME gfx_render_tests_checkpoints COMMAND gfx_render_tests)
set_tests_properties(gfx_render_tests_checkpoints PROPERTIES ENVIRONMENT "ANY360_GPU_CHECKPOINTS=1")

# Thousands of frames (textures changing and coming and going, a resolve, a swap): nothing the renderer holds grows
# without bound, the process's memory stops growing; CPU and GPU time a frame reported as a baseline.
add_executable(gfx_resource_tests tests/gfx_resource_tests.cpp)
target_link_libraries(gfx_resource_tests PRIVATE any360_gfx_vulkan $<$<BOOL:${WIN32}>:psapi>)
target_compile_options(gfx_resource_tests PRIVATE -Wall -Wextra)
target_compile_definitions(gfx_resource_tests PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN>)
add_test(NAME gfx_resource_tests COMMAND gfx_resource_tests)

# Replays a frame capture (capture.h) and reports what its frames need: gfx_replay CAPTURE [--gpu NAME] [--out DIR].
add_executable(gfx_replay tests/gfx_replay.cpp)
target_link_libraries(gfx_replay PRIVATE any360_gfx_vulkan $<$<BOOL:${WIN32}>:winmm psapi>)
target_compile_options(gfx_replay PRIVATE -Wall -Wextra)
target_compile_definitions(gfx_replay PRIVATE $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN>)

# A window showing the whole chain every frame (PM4 -> command processor -> renderer -> resolve -> swap -> present).
# The window comes from SDL3 (the platform layer's choice); until SDL3 is vendored, its headers and static library
# are taken from the reference SDK install when it has them. Run by hand: gfx_window_demo [--frames N] [--gpu NAME].
set(ANY360_SDL3_DIR "${ANY360_REFERENCE_SDK}" CACHE PATH "An install with include/SDL3 and lib/SDL3-static*.lib")
if(WIN32 AND ANY360_SDL3_DIR AND EXISTS "${ANY360_SDL3_DIR}/include/SDL3/SDL.h" AND EXISTS "${ANY360_SDL3_DIR}/lib/SDL3-staticrd.lib")
    add_executable(gfx_window_demo tests/gfx_window_demo.cpp)
    target_include_directories(gfx_window_demo SYSTEM PRIVATE "${ANY360_SDL3_DIR}/include")
    target_link_libraries(gfx_window_demo PRIVATE any360_gfx_vulkan "${ANY360_SDL3_DIR}/lib/SDL3-staticrd.lib"
                          setupapi winmm imm32 version ole32 oleaut32 user32 gdi32 advapi32 shell32 cfgmgr32 uuid)
    target_compile_options(gfx_window_demo PRIVATE -Wall -Wextra)
endif()
