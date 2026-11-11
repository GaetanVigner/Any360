# any360::xdk: native replacements of the functions titles link statically from the XDK's libraries (the C runtime
# first). Header-only portable C++ (include/any360/xdk), so a title's binding inlines them; unit tests and a
# benchmark for every function (docs/NATIVE_FUNCTIONS.md). Built with layer/sys.
add_library(any360_xdk INTERFACE)
target_include_directories(any360_xdk INTERFACE include)

enable_testing()
add_executable(xdk_crt_tests tests/xdk_crt_tests.cpp)
target_link_libraries(xdk_crt_tests PRIVATE any360_xdk)
target_compile_options(xdk_crt_tests PRIVATE -Wall -Wextra)
add_test(NAME xdk_crt_tests COMMAND xdk_crt_tests)

add_executable(xdk_xapi_tests tests/xdk_xapi_tests.cpp)
target_link_libraries(xdk_xapi_tests PRIVATE any360_xdk)
target_compile_options(xdk_xapi_tests PRIVATE -Wall -Wextra)
add_test(NAME xdk_xapi_tests COMMAND xdk_xapi_tests)

add_executable(xdk_d3d_tests tests/xdk_d3d_tests.cpp)
target_link_libraries(xdk_d3d_tests PRIVATE any360_xdk)
target_compile_options(xdk_d3d_tests PRIVATE -Wall -Wextra)
add_test(NAME xdk_d3d_tests COMMAND xdk_d3d_tests)

# Not tests: they print timings (xdk_crt_bench [repeats], xdk_xapi_bench [repeats]).
add_executable(xdk_crt_bench tests/xdk_crt_bench.cpp)
target_link_libraries(xdk_crt_bench PRIVATE any360_xdk)
target_compile_options(xdk_crt_bench PRIVATE -Wall -Wextra)
add_executable(xdk_xapi_bench tests/xdk_xapi_bench.cpp)
target_link_libraries(xdk_xapi_bench PRIVATE any360_xdk)
target_compile_options(xdk_xapi_bench PRIVATE -Wall -Wextra)
