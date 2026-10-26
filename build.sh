#!/usr/bin/env bash
# Build QLang out-of-source. All build files land in ./build.
#
#   ./build.sh          configure + build
#   ./build.sh test     build, then run ctest + the harness binaries
#   ./build.sh clean    wipe ./build
#
# Env: BUILD_TYPE (default Release), JOBS (default nproc)
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"

do_build() {
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_DIR" -j "$JOBS"
}

case "${1:-build}" in
  build)
    do_build
    echo
    echo "Build complete ($BUILD_TYPE). Artifacts in $BUILD_DIR/:"
    echo "  lib:      $BUILD_DIR/libqlang.a"
    echo "  example:  $BUILD_DIR/qlang_example"
    echo "  runner:   $BUILD_DIR/qlang_run"
    echo "  harness:  $BUILD_DIR/spec_check2  $BUILD_DIR/test_module  $BUILD_DIR/test_gen_simple  $BUILD_DIR/range_check"
    ;;
  test)
    do_build
    echo
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    echo
    "$BUILD_DIR/spec_check2"
    "$BUILD_DIR/test_module"
    "$BUILD_DIR/range_check"
    ;;
  clean)
    rm -rf "$BUILD_DIR"
    echo "Removed $BUILD_DIR/"
    ;;
  *)
    echo "usage: $0 [build|test|clean]" >&2
    exit 2
    ;;
esac
