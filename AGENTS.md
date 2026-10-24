# instructions

reload `SPEC.md` and compare with `SPEC_COMPLIANCE.md` at session start.

`SPEC_COMPLIANCE.md` should track the hash of SPEC.md
always update `SPEC_COMPLIANCE.md`.

maintain separate targets in the CMakeLists.
- linkage library src/*.cpp
- demo application (e,g, examples/hello.cpp)
- test sets for all the features listed below. 

always suggest commit name but don't commit automatically.

always ask for permission before touching this file.

don't put build files in the source tree, including but not limited to these files:
```
CMakeFiles/*
CMakeCache.txt
CTestTestfile.cmake
Makefile
cmake_install.cmake
```