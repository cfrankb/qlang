# Agent instructions

## Session start
- Reload `SPEC.md` and compare it against `SPEC_COMPLIANCE.md`.

## Keep SPEC_COMPLIANCE.md current
- It records the SHA256 of the SPEC.md it describes.
- On any change to SPEC.md or to the implementation it covers:
  1. Rebuild and re-run the affected suites (see Build & test).
  2. Only then flip the relevant Open Items to verified, and update the hash, tally, and checkpoint rows.
- The hash detects change; re-running the tests proves correctness. Don't bump the hash before testing.

## Build & test (required before claiming "verified")
- `cmake -B build && cmake --build build`
- `ctest --test-dir build`  (or build/run a single suite directly, e.g. `spec_check2.cpp` against `build/libqlang.a`)

## CMake layout
- Keep separate, independently buildable targets:
  - library: `src/*.cpp` → `libqlang.a`
  - demo: e.g. `examples/hello.cpp`
  - tests: one set per feature area (the feature list lives in SPEC.md)

## Commits
- Always suggest a commit name; don't commit automatically.
- Stage by concern — keep unrelated doc-only edits out of feature commits.
- Leave patch artifacts (`*.orig`, `*.rej`) out of the commit, or delete them.

## Edit this file
- Always ask for permission before editing AGENTS.md.

## Build files
Build artifacts belong in `build/` — that's the correct location. Don't scatter
build files in the source root:
```
CMakeFiles/*
CMakeCache.txt
CTestTestfile.cmake
Makefile
cmake_install.cmake
*.o
*.a
*.rej
*.orig
```
