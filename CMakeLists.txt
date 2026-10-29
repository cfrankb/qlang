cmake_minimum_required(VERSION 3.16)
project(qlang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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

# Spec compliance test harness.
add_executable(spec_check2 examples/spec_check2.cpp)
target_link_libraries(spec_check2 PRIVATE qlang)

# Module system smoke test.
add_executable(test_module examples/test_module.cpp)
target_link_libraries(test_module PRIVATE qlang)

# Generator test harness.
add_executable(test_gen_simple examples/test_gen_simple.cpp)
target_link_libraries(test_gen_simple PRIVATE qlang)

# range keyword tests: for...in range (counter loop), `in range` (bounds check),
# arr = range() (materialization), and range generators.
add_executable(range_check examples/range_check.cpp)
target_link_libraries(range_check PRIVATE qlang)
add_test(NAME range COMMAND range_check)

# __str__ dispatch in all display paths (str/sprintf/printf/f-string/join/print/println).
add_executable(class_str_test examples/class_str_test.cpp)
target_link_libraries(class_str_test PRIVATE qlang)
add_test(NAME class_str COMMAND class_str_test)

# Class `static` members: shared __statics__ table, :: scope resolution,
# instance-level static access, static const.
add_executable(class_static_test examples/class_static_test.cpp)
target_link_libraries(class_static_test PRIVATE qlang)
add_test(NAME class_static COMMAND class_static_test)
