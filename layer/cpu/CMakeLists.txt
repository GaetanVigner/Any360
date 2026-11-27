# any360::cpu: the translator of a title's PowerPC code from the XEX's words (tools/ppc_translate.py) and its tests.
# cpu_isa_tests: each instruction case against the ISA. cpu_e2e_tests / cpu_e2e_bench: a title's functions as Any360
# translates them against the reference runtime's, when that title's files are on this machine (docs/CPU.md).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ANY360_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CPU_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CPU_GENERATED})

add_custom_command(
  OUTPUT ${CPU_GENERATED}/cpu_isa_cases.h
  COMMAND Python3::Interpreter -I ${ANY360_ROOT}/tools/ppc_translate.py --cases ${CMAKE_CURRENT_SOURCE_DIR}/tests/isa_cases.txt
          --namespace any360::cpu::translated::isa --out ${CPU_GENERATED}/cpu_isa_cases.h
  DEPENDS ${ANY360_ROOT}/tools/ppc_translate.py ${CMAKE_CURRENT_SOURCE_DIR}/tests/isa_cases.txt
  VERBATIM)

set(CPU_TEST_OPTIONS -Wall -Wextra -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)

add_executable(cpu_isa_tests tests/cpu_isa_tests.cpp ${CPU_GENERATED}/cpu_isa_cases.h)
target_include_directories(cpu_isa_tests PRIVATE ${CPU_GENERATED} tests)
target_link_libraries(cpu_isa_tests PRIVATE any360_xdk)
target_compile_options(cpu_isa_tests PRIVATE ${CPU_TEST_OPTIONS})
add_test(NAME cpu_isa_tests COMMAND cpu_isa_tests)

set(ANY360_CPU_E2E_TITLE "mirrorsedge" CACHE STRING "The title whose functions cpu_e2e_tests translates (titles/<title>/pipeline.json)")
set(CPU_E2E_PIPELINE ${ANY360_ROOT}/titles/${ANY360_CPU_E2E_TITLE}/pipeline.json)
set(CPU_E2E_FUNCTIONS ${ANY360_ROOT}/docs/data/functions-${ANY360_CPU_E2E_TITLE}.json)
set(CPU_E2E_READY OFF)
if(EXISTS ${CPU_E2E_PIPELINE} AND EXISTS ${CPU_E2E_FUNCTIONS})
  file(READ ${CPU_E2E_PIPELINE} CPU_E2E_JSON)
  string(JSON CPU_E2E_IMAGE ERROR_VARIABLE CPU_E2E_NO_IMAGE GET ${CPU_E2E_JSON} image)
  string(JSON CPU_E2E_SOURCE ERROR_VARIABLE CPU_E2E_NO_SOURCE GET ${CPU_E2E_JSON} generated)
  if(NOT CPU_E2E_NO_IMAGE AND NOT CPU_E2E_NO_SOURCE AND EXISTS ${CPU_E2E_IMAGE} AND EXISTS ${CPU_E2E_SOURCE})
    set(CPU_E2E_READY ON)
  endif()
endif()

if(CPU_E2E_READY)
  set(CPU_E2E_HEADERS ${CPU_GENERATED}/cpu_e2e_table.h ${CPU_GENERATED}/cpu_e2e_translated.h ${CPU_GENERATED}/cpu_e2e_reference.h
      ${CPU_GENERATED}/cpu_e2e_plain_table.h ${CPU_GENERATED}/cpu_e2e_plain_translated.h ${CPU_GENERATED}/cpu_e2e_plain_reference.h)
  add_custom_command(
    OUTPUT ${CPU_E2E_HEADERS}
    COMMAND Python3::Interpreter -I ${ANY360_ROOT}/tools/cpu_e2e.py --title ${ANY360_CPU_E2E_TITLE} --count 300 --out ${CPU_GENERATED}
    DEPENDS ${ANY360_ROOT}/tools/cpu_e2e.py ${ANY360_ROOT}/tools/ppc_translate.py ${ANY360_ROOT}/tools/ppc_model.py
            ${CPU_E2E_PIPELINE} ${CPU_E2E_FUNCTIONS} ${CPU_E2E_IMAGE}
    VERBATIM)
  foreach(target cpu_e2e_tests cpu_e2e_bench)
    add_executable(${target} tests/${target}.cpp ${CPU_E2E_HEADERS})
    target_include_directories(${target} PRIVATE ${CPU_GENERATED} tests)
    target_link_libraries(${target} PRIVATE any360_xdk)
    target_compile_options(${target} PRIVATE ${CPU_TEST_OPTIONS})
  endforeach()
  add_test(NAME cpu_e2e_tests COMMAND cpu_e2e_tests)
  set_tests_properties(cpu_e2e_tests PROPERTIES TIMEOUT 300)
else()
  message(STATUS "any360 cpu: no ${ANY360_CPU_E2E_TITLE} files on this machine, cpu_e2e_tests not built")
endif()
