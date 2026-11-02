cmake_minimum_required(VERSION 3.16)
project(qlang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Core library: lexer + parser + value types + interpreter host.
add_library(qlang
  src/QLangValue.cpp
  src/QLangLexer.cpp
  src/QLangParser.cpp
  src/QLangBytecode.cpp
  src/QLangBytecodeIO.cpp
  src/QLangCompiler.cpp
  src/QLangVM.cpp
  src/QLangContext.cpp
  src/QLangModules.cpp
  src/QLangObjects.cpp
)
target_include_directories(qlang PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(qlang PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)

# Example program that exercises the interpreter.
add_executable(qlang_example examples/hello.cpp)
target_link_libraries(qlang_example PRIVATE qlang)

# Small script runner: qlang_run [--trace-level <0-3>] [--output <file.qlc>] [--asm <file.qla>] [--asm-level <0-4>] file.ql
add_executable(qlang_run examples/qlang_run.cpp)
target_link_libraries(qlang_run PRIVATE qlang)

enable_testing()
add_test(NAME smoke COMMAND qlang_example)

# Unit test harnesses (tests/unit/). Each links libqlang and is registered
# with ctest; a non-zero exit code fails the suite.

# Spec compliance test harness.
add_executable(spec_check tests/unit/spec_check.cpp)
target_link_libraries(spec_check PRIVATE qlang)
add_test(NAME spec_check COMMAND spec_check)

# Extended spec compliance test harness (421 cases).
add_executable(spec_check2 tests/unit/spec_check2.cpp)
target_link_libraries(spec_check2 PRIVATE qlang)
add_test(NAME spec_check2 COMMAND spec_check2)

# cast<Type>(expr) tests.
add_executable(spec_cast_test tests/unit/spec_cast_test.cpp)
target_link_libraries(spec_cast_test PRIVATE qlang)
add_test(NAME spec_cast_test COMMAND spec_cast_test)

# Module system smoke test.
add_executable(test_module tests/unit/test_module.cpp)
target_link_libraries(test_module PRIVATE qlang)
add_test(NAME test_module COMMAND test_module)

# Generator test harness (yield, exhaustion, outer capture).
add_executable(gen_test tests/unit/gen_test.cpp)
target_link_libraries(gen_test PRIVATE qlang)
add_test(NAME gen_test COMMAND gen_test)

# Legacy one-off script runner (superseded by qlang_run); built but not tested.
add_executable(test_gen_simple examples/test_gen_simple.cpp)
target_link_libraries(test_gen_simple PRIVATE qlang)

# range keyword tests: for...in range (counter loop), `in range` (bounds check),
# arr = range() (materialization), and range generators.
add_executable(range_check tests/unit/range_check.cpp)
target_link_libraries(range_check PRIVATE qlang)
add_test(NAME range COMMAND range_check)

# __str__ dispatch in all display paths (str/sprintf/printf/f-string/join/print/println).
add_executable(class_str_test tests/unit/class_str_test.cpp)
target_link_libraries(class_str_test PRIVATE qlang)
add_test(NAME class_str COMMAND class_str_test)

# Class `static` members: shared __statics__ table, :: scope resolution,
# instance-level static access, static const.
add_executable(class_static_test tests/unit/class_static_test.cpp)
target_link_libraries(class_static_test PRIVATE qlang)
add_test(NAME class_static COMMAND class_static_test)

# .qlc codec round-trip + malformed-input robustness (docs/SERIALIZATION.md §9).
add_executable(codec_check tests/unit/codec_check.cpp)
target_link_libraries(codec_check PRIVATE qlang)
add_test(NAME codec COMMAND codec_check)

# External C++ types (docs/EXTERNAL_TYPES.md): public-member setters,
# read-only computed props, methods (incl. BoundMethod extraction), enum
# constants, str, handoff integrity, registration-time validation.
add_executable(test_external tests/unit/test_external.cpp)
target_link_libraries(test_external PRIVATE qlang)
add_test(NAME external COMMAND test_external)

# Native/variable injection: ctx.set (incl. const) + ctx.setFunc + ctx.call.
add_executable(test_inject tests/unit/test_inject.cpp)
target_link_libraries(test_inject PRIVATE qlang)
add_test(NAME test_inject COMMAND test_inject)

# $ operator (stringify variable content, dynamic table keys).
add_executable(test_dollar_operator tests/unit/test_dollar_operator.cpp)
target_link_libraries(test_dollar_operator PRIVATE qlang)
add_test(NAME test_dollar_operator COMMAND test_dollar_operator)

# Script-level regression suite (tests/scripts/). Each .ql script is driven
# by qlang_run (the smoke-test app); a non-zero exit code fails the suite.
file(GLOB QL_SCRIPTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/*.ql)
foreach(ql_script IN LISTS QL_SCRIPTS)
  get_filename_component(ql_name ${ql_script} NAME_WE)
  add_test(NAME script_${ql_name} COMMAND qlang_run ${ql_script})
endforeach()

# Performance baseline harness (SPEC # performance tests): median-of-5 per
# stage (parse/compile/execute) over a fixed corpus, compared against
# tests/perf/baseline.json (default tolerance ±20%, QL_PERF_TOLERANCE).
# The `perf` gate is calibrated for Release builds (CI gxx job); in any
# other build type it skips (exits 77, see SKIP_RETURN_CODE below).
add_executable(perf_bench tests/perf/perf_bench.cpp)
target_link_libraries(perf_bench PRIVATE qlang)
target_compile_definitions(perf_bench PRIVATE
  QLANG_TEST_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts"
  QLANG_PERF_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/tests/perf/baseline.json"
  QLANG_PERF_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
add_test(NAME perf COMMAND perf_bench)
# In non-Release builds perf_bench exits 77 ("not run"): the baseline is
# calibrated for Release (CI gxx job), so the gate skips instead of failing
# on build-type noise.
set_tests_properties(perf PROPERTIES SKIP_RETURN_CODE 77)
