# any360_host_rexglue: binds a title built on the ReXGlue runtime (stage 0) to Any360's layers, a piece at a time
# (include/any360/host/rexglue.h). Compiled in the title's build, against the runtime's headers; layer/sys adds it
# when ANY360_REFERENCE_SDK names a runtime install. A title adds layer/sys with EXCLUDE_FROM_ALL and links this.
# Diagnostics with no runtime or renderer behind them: thread stacks, the hitch tracer (any360/host/hitch_tracer.h),
# the function profiler (any360/host/profiler.h).
add_library(any360_host_diagnostics STATIC src/thread_stacks.cpp src/hitch_tracer.cpp src/profiler.cpp)
target_include_directories(any360_host_diagnostics PUBLIC include)
target_compile_options(any360_host_diagnostics PRIVATE -Wall -Wextra)
if(WIN32)
  target_link_libraries(any360_host_diagnostics PUBLIC dbghelp psapi winmm)
endif()
add_library(any360_host_rexglue STATIC src/rexglue.cpp src/system.cpp)
target_include_directories(any360_host_rexglue PUBLIC include)
# The renderer stays private: its vendored headers (fmt among them) must not reach the title's sources.
target_link_libraries(any360_host_rexglue PUBLIC any360_sys_ppc any360_host_diagnostics PRIVATE any360_gfx_vulkan)
target_compile_options(any360_host_rexglue PRIVATE -Wall -Wextra)
# The settings glue logs through the runtime's spdlog: kept apart from the renderer's vendored fmt (its namespace
# renamed) so it compiles against the runtime's fmt. Titles get it with the binding. With it, the native replacements
# of library functions (xdk_natives.h), which log and read settings the same way.
add_library(any360_host_rexglue_settings STATIC src/settings.cpp src/crash_report.cpp src/xdk_natives.cpp
            src/d3d_selftest.cpp src/d3d_draw.cpp)
target_include_directories(any360_host_rexglue_settings PUBLIC include)
target_link_libraries(any360_host_rexglue_settings PUBLIC any360_sys_ppc any360_xdk)
target_compile_options(any360_host_rexglue_settings PRIVATE -Wall -Wextra)
target_link_libraries(any360_host_rexglue PUBLIC any360_host_rexglue_settings)
# The audio group's decoder and output (StartAudio), when the XMA decoder is built (FFmpeg's headers found).
if(TARGET any360_sys_xma)
    target_sources(any360_host_rexglue PRIVATE src/audio.cpp)
    target_link_libraries(any360_host_rexglue PUBLIC any360_sys_xma)
    target_compile_definitions(any360_host_rexglue PUBLIC ANY360_REXGLUE_AUDIO)
    target_compile_definitions(any360_host_rexglue_settings PRIVATE ANY360_REXGLUE_AUDIO)
    # The settings start the audio group (StartAudio, in the binding's library): a cycle between the two static
    # libraries, said so that CMake repeats them on the link line (GNU ld resolves archives in order; lld does not
    # care).
    target_link_libraries(any360_host_rexglue_settings PRIVATE any360_host_rexglue)
endif()

# The binding inside a running reference runtime (tool mode), with a ring, interrupts and vertical blanks.
if(WIN32)
    enable_testing()
    add_executable(host_rexglue_tests tests/host_rexglue_tests.cpp)
    target_link_libraries(host_rexglue_tests PRIVATE any360_host_rexglue "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                          "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
    target_compile_options(host_rexglue_tests PRIVATE -Wall -Wextra)
    foreach(_dll rexruntimerd.dll TracyClientrd.dll)
        add_custom_command(TARGET host_rexglue_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                           "${ANY360_REFERENCE_SDK}/bin/${_dll}" "$<TARGET_FILE_DIR:host_rexglue_tests>")
    endforeach()
    add_test(NAME host_rexglue_tests COMMAND host_rexglue_tests)
    # The system binding: every override, a group off reaching the runtime, on reaching any360::sys, the dispatcher.
    add_executable(host_system_tests tests/host_system_tests.cpp)
    target_link_libraries(host_system_tests PRIVATE any360_host_rexglue "${ANY360_REFERENCE_SDK}/lib/rexruntimerd.lib"
                          "${ANY360_REFERENCE_SDK}/lib/spdlogrd.lib" "${ANY360_REFERENCE_SDK}/lib/fmtrd.lib")
    target_compile_options(host_system_tests PRIVATE -Wall -Wextra)
    add_test(NAME host_system_tests COMMAND host_system_tests)
    # The hitch tracer's sampling on a thread spinning in a known function.
    add_executable(host_diagnostics_tests tests/host_diagnostics_tests.cpp)
    target_link_libraries(host_diagnostics_tests PRIVATE any360_host_diagnostics)
    target_compile_options(host_diagnostics_tests PRIVATE -Wall -Wextra)
    add_test(NAME host_diagnostics_tests COMMAND host_diagnostics_tests)
endif()
