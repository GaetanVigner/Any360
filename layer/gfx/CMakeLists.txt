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
