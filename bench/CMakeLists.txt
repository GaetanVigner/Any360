# any360_bench: Any360's own benchmark title (docs/BENCH.md), built from layer/sys/CMakeLists.txt when the reference
# SDK is given (its headers give the PowerPC context type the title code is written against).
# any360_bench_gfx holds what reaches any360::gfx (the scene's content and command stream, the GPU side, the guest
# address space); the executable holds the title code, which sees any360::sys only through its exports.
add_library(any360_bench_gfx STATIC src/city.cpp src/gpu.cpp src/guest_space.cpp)
target_include_directories(any360_bench_gfx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../layer/gfx/tests)
target_link_libraries(any360_bench_gfx PRIVATE any360_gfx_vulkan any360_host_diagnostics)
target_link_libraries(any360_bench_gfx PUBLIC any360_sys psapi)
target_compile_options(any360_bench_gfx PRIVATE -Wall -Wextra)
target_compile_definitions(any360_bench_gfx PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(ANY360_SDL3_DIR AND EXISTS "${ANY360_SDL3_DIR}/include/SDL3/SDL.h" AND EXISTS "${ANY360_SDL3_DIR}/lib/SDL3-staticrd.lib")
    target_compile_definitions(any360_bench_gfx PRIVATE ANY360_BENCH_WINDOW=1)
    target_include_directories(any360_bench_gfx SYSTEM PRIVATE "${ANY360_SDL3_DIR}/include")
    target_link_libraries(any360_bench_gfx PRIVATE "${ANY360_SDL3_DIR}/lib/SDL3-staticrd.lib" setupapi winmm imm32 version
                          ole32 oleaut32 user32 gdi32 advapi32 shell32 cfgmgr32 uuid)
endif()

add_executable(any360_bench src/main.cpp src/title.cpp)
target_link_libraries(any360_bench PRIVATE any360_bench_gfx any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                      "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
target_compile_options(any360_bench PRIVATE -Wall -Wextra)
target_compile_definitions(any360_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
foreach(_dll rexruntimerd.dll TracyClientrd.dll)
    add_custom_command(TARGET any360_bench POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       "${ANY360_REFERENCE_SDK}/bin/${_dll}" "$<TARGET_FILE_DIR:any360_bench>")
endforeach()
# The city scene end to end: every frame swapped, no draw failed, the title deterministic (command stream and audio
# hashes), the CPU's texture writes seen through the write watch, the last frame against the expected picture.
add_test(NAME any360_bench_city COMMAND any360_bench --frames 240 --check ${CMAKE_CURRENT_SOURCE_DIR}/expected/city.txt)
