# tests

all tests source files and scripts must resides in `tests/`

tests have to be run after every tasks to prevent regressions.

## regression tests

these tests ensure that functionality is maintained


## unit tests

test individual features and funcionality. pay special attention to edge cases.

## performance tests

performance tests must create a baseline for the script `parser`, `compiler` and `execution time` that is immutable and will be logged and compared against. this data must be tracked as new features are being added.

# automatic testing

provide a complete github-ci system that will test the qlang library, smoke test app and testing suite against `msvc`, `g++`, `mingw`, `wasm`, and `clang`.

running on C++20

three separate ci/cd yaml
- msvc (windows)
- wasm (special emscripten image)
- everything else (ubuntu:latest)
