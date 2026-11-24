# any360::sys: the native system layer (stage 1 of docs/DESIGN.md). `any360_sys` is portable C++23 with no
# dependency; `any360_sys_ppc` adds the PowerPC ABI adapters a static recompilation binds (needs the reference SDK's
# headers for PPCContext); `sys_io_tests` runs every adapter against the reference runtime's implementation.
#
#   cmake -S layer/sys -B out/sys -G Ninja -DANY360_REFERENCE_SDK=<ReXGlue SDK install (include/, lib/, bin/)>
#   cmake --build out/sys && ctest --test-dir out/sys --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(any360_sys LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The glyph sets (assets/glyphs) beside a title's executable, in glyphs/, where the graphics binding looks for them
# (any360/host/rexglue.h, GraphicsOptions::glyphs_dir): any360_copy_glyphs(<title target>) after each build.
set(ANY360_GLYPH_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/glyphs" CACHE INTERNAL "Any360's glyph sets")
function(any360_copy_glyphs target)
  file(GLOB _any360_glyphs "${ANY360_GLYPH_ASSETS}/*.json" "${ANY360_GLYPH_ASSETS}/*_64.png" "${ANY360_GLYPH_ASSETS}/*_128.png")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/glyphs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_any360_glyphs} "$<TARGET_FILE_DIR:${target}>/glyphs"
    VERBATIM)
endfunction()

add_library(any360_sys STATIC src/rtl.cpp src/info.cpp src/config.cpp src/error_tables.cpp src/video.cpp src/net.cpp
            src/threading.cpp src/format.cpp src/objects.cpp src/scheduler.cpp src/precise_sleep.cpp src/memory.cpp src/files.cpp src/host_times.cpp src/xam.cpp src/win32.cpp src/input.cpp src/input_bindings.cpp src/input_glyphs.cpp src/input_players.cpp src/json.cpp src/png.cpp src/crypt.cpp src/crypt_blocks.cpp src/crypt_host.cpp src/ldi.cpp
            src/graphics.cpp src/audio.cpp src/variables.cpp src/host_sockets.cpp)
target_include_directories(any360_sys PUBLIC include)
if(WIN32)
  target_link_libraries(any360_sys PUBLIC ws2_32)  # the host's sockets (src/host_sockets.cpp)
  target_link_libraries(any360_sys PUBLIC bcrypt)  # the host's hashes, CNG (src/crypt_host.cpp)
endif()
# std::thread and the condition variables (the scheduler, objects): libpthread on older glibc (Ubuntu 20.04's 2.31).
find_package(Threads REQUIRED)
target_link_libraries(any360_sys PUBLIC Threads::Threads)
target_compile_options(any360_sys PRIVATE -Wall -Wextra)
# Host heap allocations counted per thread (include/any360/sys/alloc_meter.h): the global operator new / delete of
# every executable that links any360_sys or any360_xdk (the benches', the tests', the titles').
add_library(any360_alloc_meter STATIC src/alloc_meter.cpp)
target_include_directories(any360_alloc_meter PUBLIC include)
target_compile_options(any360_alloc_meter PRIVATE -Wall -Wextra)
target_link_libraries(any360_sys PUBLIC any360_alloc_meter)

# The object manager's scenario tests need no reference runtime.
enable_testing()
add_executable(sys_object_tests tests/sys_object_tests.cpp tests/watched_memory.cpp)
target_link_libraries(sys_object_tests PRIVATE any360_sys)
target_compile_options(sys_object_tests PRIVATE -Wall -Wextra)
add_test(NAME sys_object_tests COMMAND sys_object_tests)
# The input bindings (docs/GLYPHS.md): the JSON reader, the launcher's file, layer 1's mapper; --bench times a sample.
add_executable(sys_input_tests tests/sys_input_tests.cpp)
target_link_libraries(sys_input_tests PRIVATE any360_sys)
target_compile_options(sys_input_tests PRIVATE -Wall -Wextra)
target_compile_definitions(sys_input_tests PRIVATE ANY360_GLYPHS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/glyphs")
add_test(NAME sys_input_tests COMMAND sys_input_tests)
add_executable(sys_png_tests tests/sys_png_tests.cpp)
target_link_libraries(sys_png_tests PRIVATE any360_sys)
target_compile_options(sys_png_tests PRIVATE -Wall -Wextra)
target_compile_definitions(sys_png_tests PRIVATE ANY360_GLYPHS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/glyphs")
add_test(NAME sys_png_tests COMMAND sys_png_tests)
# Not a test: step 12's calls timed, with host memory a call, beside what the host offers (Windows' CNG, wcslen);
# results kept in docs/perf/archive/ (sys_step12_bench [repeats]).
add_executable(sys_step12_bench tests/sys_step12_bench.cpp)
target_link_libraries(sys_step12_bench PRIVATE any360_sys any360_alloc_meter)
target_include_directories(sys_step12_bench PRIVATE tests)
if(WIN32)
    target_link_libraries(sys_step12_bench PRIVATE bcrypt cabinet)
endif()
target_compile_options(sys_step12_bench PRIVATE -Wall -Wextra)

# The network over the host's loopback sockets.
# LDI's LZX decoder and contexts; on Windows checked against cabinet.dll's LZX encoder (FCI).
add_executable(sys_ldi_tests tests/sys_ldi_tests.cpp)
target_link_libraries(sys_ldi_tests PRIVATE any360_sys)
target_include_directories(sys_ldi_tests PRIVATE tests)
target_compile_options(sys_ldi_tests PRIVATE -Wall -Wextra)
if(WIN32)
  target_link_libraries(sys_ldi_tests PRIVATE cabinet)
endif()
add_test(NAME sys_ldi_tests COMMAND sys_ldi_tests)
add_executable(sys_net_tests tests/sys_net_tests.cpp)
target_link_libraries(sys_net_tests PRIVATE any360_sys)
target_compile_options(sys_net_tests PRIVATE -Wall -Wextra)
add_test(NAME sys_net_tests COMMAND sys_net_tests)

# any360::xdk: native replacements of statically linked library functions, with their tests and benchmark.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../xdk ${CMAKE_CURRENT_BINARY_DIR}/xdk)

# any360::gfx, built with the system layer it sits on.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gfx ${CMAKE_CURRENT_BINARY_DIR}/gfx)

set(ANY360_REFERENCE_SDK "" CACHE PATH "ReXGlue SDK install used as the reference (include/, lib/, bin/)")
if(ANY360_REFERENCE_SDK)
    add_library(any360_sys_ppc STATIC ppc/exports.cpp)
    target_link_libraries(any360_sys_ppc PUBLIC any360_sys)
    target_include_directories(any360_sys_ppc SYSTEM PUBLIC "${ANY360_REFERENCE_SDK}/include")
    target_compile_definitions(any360_sys_ppc PUBLIC SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN>)

    # The XMA decoder (an XmaBackend, src/xma.cpp): FFmpeg's xmaframes decoder (FFmpeg is LGPL), from
    #   - an SDK built in the title's tree (REXSDK_DIR): its FFmpeg targets;
    #   - else, on x86-64, Any360's own build of the SDK's FFmpeg (third_party/ffmpeg, NOTICE.md): with NASM found,
    #     FFmpeg's x86 assembly on the XMA path (the SDK's libraries have none);
    #   - else the SDK's prebuilt libraries, with the headers of the FFmpeg they were built from.
    option(ANY360_FFMPEG_VENDORED "Build the SDK's FFmpeg here (third_party/ffmpeg) for the XMA decoder" ON)
    find_path(ANY360_FFMPEG_INCLUDE libavcodec/avcodec.h HINTS "${ANY360_REFERENCE_SDK}/include"
              DOC "Headers of the FFmpeg the reference SDK's libavcodec was built from (with the xmaframes decoder)")
    if(TARGET libavcodec AND TARGET libavutil)
        # An SDK built in the title's tree (REXSDK_DIR): its FFmpeg targets bring their headers.
        add_library(any360_sys_xma STATIC src/xma.cpp)
        target_link_libraries(any360_sys_xma PUBLIC any360_sys libavcodec libavutil)
        target_compile_options(any360_sys_xma PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
    elseif(ANY360_FFMPEG_VENDORED AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        add_subdirectory(third_party/ffmpeg)
        add_library(any360_sys_xma STATIC src/xma.cpp)
        target_link_libraries(any360_sys_xma PUBLIC any360_sys any360_avcodec any360_avutil)
        target_compile_options(any360_sys_xma PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
    elseif(ANY360_FFMPEG_INCLUDE)
        add_library(any360_sys_xma STATIC src/xma.cpp)
        target_link_libraries(any360_sys_xma PUBLIC any360_sys)
        target_include_directories(any360_sys_xma SYSTEM PRIVATE "${ANY360_FFMPEG_INCLUDE}")
        target_compile_options(any360_sys_xma PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
        if(WIN32)
            set(_ffmpeg_suffix "$<IF:$<CONFIG:Debug>,d,$<IF:$<CONFIG:RelWithDebInfo>,rd,>>")
            target_link_libraries(any360_sys_xma PUBLIC "${ANY360_REFERENCE_SDK}/lib/libavcodec${_ffmpeg_suffix}.lib"
                                  "${ANY360_REFERENCE_SDK}/lib/libavutil${_ffmpeg_suffix}.lib" bcrypt)
        else()
            # The SDK's archives are named after their CMake targets (libavcodec): liblibavcodec.a.
            target_link_libraries(any360_sys_xma PUBLIC "${ANY360_REFERENCE_SDK}/lib/liblibavcodec.a"
                                  "${ANY360_REFERENCE_SDK}/lib/liblibavutil.a")
        endif()
    endif()
    if(TARGET any360_sys_xma)
        add_executable(xma_tests tests/xma_tests.cpp)
        # Built with FFmpeg's x86 assembly: the test checks that FFmpeg detects the processor's SIMD (it is live).
        if(TARGET any360_avcodec AND ANY360_NASM)
            target_compile_definitions(xma_tests PRIVATE ANY360_FFMPEG_X86ASM=1)
        endif()
        target_link_libraries(xma_tests PRIVATE any360_sys_xma)
        target_compile_options(xma_tests PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
        add_test(NAME xma_tests COMMAND xma_tests)
        # Timings of the frame decoder on a capture from a title (not committed: xma_bench <capture>); on Windows it
        # also counts the C runtime's allocations through the program's import table.
        add_executable(xma_bench tests/xma_bench.cpp)
        target_link_libraries(xma_bench PRIVATE any360_sys_xma)
        target_compile_options(xma_bench PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
    endif()

    # The audio group's output (host/rexglue's audio.cpp) links against a title's SDL: on in a title's build, off
    # when this layer is built on its own (its tests), which has no SDL to link.
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(_any360_host_audio OFF)
    else()
        set(_any360_host_audio ON)
    endif()
    option(ANY360_HOST_AUDIO "Build the audio group's output (SDL) into the ReXGlue binding" ${_any360_host_audio})

    # The binding of a ReXGlue title to these layers (graphics first): built here to keep it compiling.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../host/rexglue ${CMAKE_CURRENT_BINARY_DIR}/host/rexglue)

    enable_testing()
    add_executable(sys_io_tests tests/sys_io_tests.cpp)
    target_include_directories(sys_io_tests PRIVATE tests)
    target_compile_options(sys_io_tests PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(sys_io_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                              "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
        foreach(_dll rexruntimerd.dll TracyClientrd.dll)
            add_custom_command(TARGET sys_io_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                               "${ANY360_REFERENCE_SDK}/bin/${_dll}" "$<TARGET_FILE_DIR:sys_io_tests>")
        endforeach()
    else()
        target_link_libraries(sys_io_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/librexruntimerd.so")
    endif()
    add_test(NAME sys_io_tests COMMAND sys_io_tests)
    # The original against Any360 through the PowerPC ABI for step 12's calls (a benchmark, not a test).
    add_executable(sys_abi_bench tests/sys_abi_bench.cpp)
    target_include_directories(sys_abi_bench PRIVATE tests)
    target_compile_options(sys_abi_bench PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(sys_abi_bench PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                              "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
        target_link_libraries(sys_abi_bench PRIVATE cabinet)
    else()
        target_link_libraries(sys_abi_bench PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/librexruntimerd.so")
    endif()
    # Step 12's adapters called as a title calls them (registers in, r3 and guest bytes out).
    add_executable(sys_adapter_tests tests/sys_adapter_tests.cpp)
    target_include_directories(sys_adapter_tests PRIVATE tests)
    target_compile_options(sys_adapter_tests PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(sys_adapter_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                              "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
    else()
        target_link_libraries(sys_adapter_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/librexruntimerd.so")
    endif()
    add_test(NAME sys_adapter_tests COMMAND sys_adapter_tests)

    # The oracle with the reference kernel running: the same libraries, a guest thread of the reference runtime.
    add_executable(sys_runtime_tests tests/sys_runtime_tests.cpp)
    target_include_directories(sys_runtime_tests PRIVATE tests)
    target_compile_options(sys_runtime_tests PRIVATE -Wall -Wextra)
    if(WIN32)
        target_link_libraries(sys_runtime_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                              "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
    else()
        target_link_libraries(sys_runtime_tests PRIVATE any360_sys_ppc "${ANY360_REFERENCE_SDK}/lib/librexruntimerd.so")
    endif()
    add_test(NAME sys_runtime_tests COMMAND sys_runtime_tests)
endif()
