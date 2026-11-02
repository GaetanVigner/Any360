# any360_host_rexglue: binds a title built on the ReXGlue runtime (stage 0) to Any360's layers, a piece at a time
# (include/any360/host/rexglue.h). Compiled in the title's build, against the runtime's headers; layer/sys adds it
# when ANY360_REFERENCE_SDK names a runtime install. A title adds layer/sys with EXCLUDE_FROM_ALL and links this.
add_library(any360_host_rexglue STATIC src/rexglue.cpp)
target_include_directories(any360_host_rexglue PUBLIC include)
# The renderer stays private: its vendored headers (fmt among them) must not reach the title's sources.
target_link_libraries(any360_host_rexglue PUBLIC any360_sys_ppc PRIVATE any360_gfx_vulkan)
target_compile_options(any360_host_rexglue PRIVATE -Wall -Wextra)

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
endif()
