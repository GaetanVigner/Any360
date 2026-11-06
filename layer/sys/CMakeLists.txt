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

add_library(any360_sys STATIC src/rtl.cpp src/info.cpp src/config.cpp src/error_tables.cpp src/video.cpp src/net.cpp
            src/threading.cpp src/format.cpp src/objects.cpp src/scheduler.cpp src/memory.cpp src/files.cpp src/host_times.cpp src/xam.cpp src/crypt.cpp
            src/graphics.cpp src/audio.cpp src/variables.cpp)
target_include_directories(any360_sys PUBLIC include)
target_compile_options(any360_sys PRIVATE -Wall -Wextra)

# The object manager's scenario tests need no reference runtime.
enable_testing()
add_executable(sys_object_tests tests/sys_object_tests.cpp)
target_link_libraries(sys_object_tests PRIVATE any360_sys)
target_compile_options(sys_object_tests PRIVATE -Wall -Wextra)
add_test(NAME sys_object_tests COMMAND sys_object_tests)

# any360::gfx, built with the system layer it sits on.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gfx ${CMAKE_CURRENT_BINARY_DIR}/gfx)

set(ANY360_REFERENCE_SDK "" CACHE PATH "ReXGlue SDK install used as the reference (include/, lib/, bin/)")
if(ANY360_REFERENCE_SDK)
    add_library(any360_sys_ppc STATIC ppc/exports.cpp)
    target_link_libraries(any360_sys_ppc PUBLIC any360_sys)
    target_include_directories(any360_sys_ppc SYSTEM PUBLIC "${ANY360_REFERENCE_SDK}/include")
    target_compile_definitions(any360_sys_ppc PUBLIC SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL $<$<BOOL:${WIN32}>:NOMINMAX WIN32_LEAN_AND_MEAN>)

    # The XMA decoder (an XmaBackend, src/xma.cpp): FFmpeg's xmaframes decoder from the SDK's libraries, with the
    # headers of the FFmpeg they were built from (the SDK's thirdparty/FFmpeg; FFmpeg is LGPL, linked as the SDK ships it).
    find_path(ANY360_FFMPEG_INCLUDE libavcodec/avcodec.h HINTS "${ANY360_REFERENCE_SDK}/include"
              DOC "Headers of the FFmpeg the reference SDK's libavcodec was built from (with the xmaframes decoder)")
    if(TARGET libavcodec AND TARGET libavutil)
        # An SDK built in the title's tree (REXSDK_DIR): its FFmpeg targets bring their headers.
        add_library(any360_sys_xma STATIC src/xma.cpp)
        target_link_libraries(any360_sys_xma PUBLIC any360_sys libavcodec libavutil)
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
            target_link_libraries(any360_sys_xma PUBLIC "${ANY360_REFERENCE_SDK}/lib/libavcodec.a"
                                  "${ANY360_REFERENCE_SDK}/lib/libavutil.a")
        endif()
        add_executable(xma_tests tests/xma_tests.cpp)
        target_link_libraries(xma_tests PRIVATE any360_sys_xma)
        target_compile_options(xma_tests PRIVATE -Wall -Wextra)
        add_test(NAME xma_tests COMMAND xma_tests)
    endif()

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
