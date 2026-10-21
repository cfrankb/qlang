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
)
target_include_directories(qlang PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(qlang PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)

# Example program that exercises the interpreter.
add_executable(qlang_example examples/hello.cpp)
target_link_libraries(qlang_example PRIVATE qlang)

enable_testing()
add_test(NAME smoke COMMAND qlang_example)
