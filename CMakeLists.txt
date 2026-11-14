cmake_minimum_required(VERSION 3.16)
project(qlang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# IDE navigation (VS Code C/C++ and clangd both read build/compile_commands.json).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC: use the conforming preprocessor. The declarative-meta macros in
# include/qlang/QLangMeta.h rely on __VA_ARGS__ content being re-scanned for
# further macro expansion (the recursive QL_FE2_N fold, and macro calls such
# as READ(...)/WRITE(...) passed through __VA_ARGS__). MSVC's *traditional*
# preprocessor does not re-scan __VA_ARGS__ (C99 6.10.3.1 restriction), which
# breaks QL_ENUM/QL_FLAGS/QL_PROPERTY expansion (undeclared enumerator
# identifiers, malformed prop(...) calls). /Zc:preprocessor (MSVC 19.29+)
# implements the conforming behavior. See docs/COMPILER_PORTABILITY.md.
if(MSVC)
  add_compile_options(/Zc:preprocessor)
endif()

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
  src/QLangLinker.cpp
  src/QLangManifest.cpp
  src/QLangModules.cpp
  src/QLangObjects.cpp
)
# BUILD_INTERFACE/INSTALL_INTERFACE so install(EXPORT) does not embed a
# source-tree path (Phase 5, F7): consumers of the installed package get
# <prefix>/include.
target_include_directories(qlang
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_options(qlang PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)

# Install + export rules (Phase 5, F7): embedding no longer requires a source
# build. `find_package(qlang)` resolves the `qlang::qlang` target, the static
# library, and the public headers.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(qlang_VERSION 0.5.0)

install(TARGETS qlang
  EXPORT qlangTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/qlang
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)
install(EXPORT qlangTargets
  FILE qlangTargets.cmake
  NAMESPACE qlang::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qlang
)

# find_package(qlang) support: generate qlangConfig.cmake from the template.
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qlangConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/qlangConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qlang
)
# CMake 4.x on this toolchain does not ship write_basic_package_config_file,
# so the version file is templated by hand (AnyNewerVersion policy).
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qlangConfigVersion.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/qlangConfigVersion.cmake
  @ONLY
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/qlangConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/qlangConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qlang
)

# Example program that exercises the interpreter.
add_executable(qlang_example examples/hello.cpp)
target_link_libraries(qlang_example PRIVATE qlang)

# qlcc (formerly qlang_run): .ql/.qlc compiler driver, runs only with --run
# (see docs/spec/18-qlang-run.md)
add_executable(qlcc examples/qlcc.cpp)
target_link_libraries(qlcc PRIVATE qlang)

# Module search paths demo (plan C8a): finds a library by path, by host
# callback, and shows a registered C++ module outranking both.
add_executable(module_search examples/module_search.cpp)
target_link_libraries(module_search PRIVATE qlang)

# Universal output callback demo (SPEC §api "output callback (universal)";
# docs/plans/output-callback-plan.md): captures every message the engine
# emits into a table and prints it — the sibling of module_search.
add_executable(output_callback examples/output_callback.cpp)
target_link_libraries(output_callback PRIVATE qlang)

# Portable module package demo (portable-module-plan P5, follow-on Phase 2):
# dumps the type manifest, packs two images around it into a .qpm, then runs it
# on a host that satisfies the contract and refuses two that do not.
add_executable(qlang_portable_module examples/portable_module.cpp)
target_link_libraries(qlang_portable_module PRIVATE qlang)

# Vendored cs3 map/actor model driven from QLang (examples/integration/). The
# host registers the QL_OBJECT bindings for CMap/CLayer/CActor, drives them
# from a script and CHECKs every bound line, so it is registered with ctest
# like the other suites. `integration_demo --dump-qlm FILE` writes the same
# bindings as a QLM1 manifest.
add_executable(integration_demo
  examples/integration/main.cpp
  examples/integration/actor.cpp
  examples/integration/layer.cpp
  examples/integration/logger.cpp
  examples/integration/map.cpp
  examples/integration/states.cpp
  examples/integration/tilesdefs.cpp
)
target_include_directories(integration_demo PRIVATE examples/integration)
# logger.cpp reaches for SDL3 unless the host says it has none; this demo has
# neither SDL nor Qt, so both engine logging backends stay compiled out.
target_compile_definitions(integration_demo PRIVATE SDL_NOT_WANTED)
target_link_libraries(integration_demo PRIVATE qlang)

enable_testing()
add_test(NAME smoke COMMAND qlang_example)
add_test(NAME integration_demo COMMAND integration_demo)
# --dump-qlm writes the QLM1 manifest of the registered bindings.
add_test(NAME integration_demo_dump_qlm
         COMMAND integration_demo --dump-qlm ${CMAKE_CURRENT_BINARY_DIR}/host.qlm)
set_tests_properties(integration_demo_dump_qlm
                     PROPERTIES PASS_REGULAR_EXPRESSION "wrote 3 types")

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

# Native function injection + ctx.call round-trips (moved from examples/
# during the Phase 1.5 orphan-file triage).
add_executable(test_native tests/unit/test_native.cpp)
target_link_libraries(test_native PRIVATE qlang)
add_test(NAME test_native COMMAND test_native)

add_executable(test_call tests/unit/test_call.cpp)
target_link_libraries(test_call PRIVATE qlang)
add_test(NAME test_call COMMAND test_call)

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

# Signals (docs/spec/17-signals.md): QL_SIGNAL meta, script emit,
# connect/disconnect (function value + string-slot), synchronous delivery,
# auto-disconnect on sender destruction, C++ hasSignal/emitSignal.
add_executable(signal_test tests/unit/signal_test.cpp)
target_link_libraries(signal_test PRIVATE qlang)
add_test(NAME signal COMMAND signal_test)

add_executable(strict_typing_test tests/unit/strict_typing_test.cpp)
target_link_libraries(strict_typing_test PRIVATE qlang)
add_test(NAME strict_typing COMMAND strict_typing_test)

# .qlc codec round-trip + the v7 version boundary + v6-encoding load-time
# validation + malformed-input robustness + the script-corpus .qlc round-trip
# and execute gate (docs/SERIALIZATION.md §9).
add_executable(codec_check tests/unit/codec_check.cpp)
target_link_libraries(codec_check PRIVATE qlang)
target_compile_definitions(codec_check PRIVATE
  QLANG_TEST_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts"
)
add_test(NAME codec COMMAND codec_check)

# Instruction encoding: 2/2/4 modifier byte, immediate forms, permitted kinds,
# golden wire bytes and size/encode/decode agreement
# (docs/plans/encoding-v6-and-indirect-plan.md §2, verification §11). This is
# the only instruction-encoding suite: the v5 `encoding` target retired with
# the v5 codec (docs/plans/m2-pipeline-switch-plan.md S7).
add_executable(encoding_v6_test tests/unit/encoding_v6_test.cpp)
target_link_libraries(encoding_v6_test PRIVATE qlang)
add_test(NAME encoding_v6 COMMAND encoding_v6_test)

# Flag-flow behaviour matrix: every comparator that writes Frame::flags against
# every flag jump, over each relation and right-operand kind, the "none set"
# state, `tst`, the general value domains, error-text parity with the
# value-producing operators, the alias guard and the reserved-slot load
# rejections (docs/plans/m3-flag-flow-plan.md S4, verification §11 item 5).
add_executable(flag_flow_test tests/unit/flag_flow_test.cpp)
target_link_libraries(flag_flow_test PRIVATE qlang)
add_test(NAME flag_flow COMMAND flag_flow_test)

# Builtin-function name table (SPEC §bytecodes "table for build in function
# (qlang base)", docs/plans/archive/external-modules-plan-v4.md Phase 1): id -> name and
# the round trip through findBuiltin/builtinId, the str/int aliases, the -1 vs
# throw split, and the compiler integration (emitted CALL ids, the negative-slot
# native path, the capture scan and the deepcopy lowering guard).
add_executable(builtin_table_test tests/unit/builtin_table_test.cpp)
target_link_libraries(builtin_table_test PRIVATE qlang)
add_test(NAME builtin_table COMMAND builtin_table_test)

# Module exports (SPEC §bytecodes "keyword export", "export table (lookup)",
# docs/plans/archive/external-modules-plan-v4.md Phase 3a): both spec forms, global-scope
# only, the export table's kind + resolved ref (function index / register),
# MODULE_NAME resolution (source > driver > "main"), library mode's `<init>`,
# the duplicate-export and exported-function-rebind guards, and the table's
# survival through the .qlc v9 round trip.
add_executable(export_test tests/unit/export_test.cpp)
target_link_libraries(export_test PRIVATE qlang)
add_test(NAME export COMMAND export_test)

# Loader + linker (SPEC §bytecodes "technical specifications" 1–5,
# docs/plans/archive/external-modules-plan-v4.md Phase 3b): QLangContext::loadProgram /
# loadProgramFile and the module registry, QLangLinker's merge (one function
# table, one folded pool per Q33, deduped native slots, disjoint register
# slices), operand re-widening on the decoded RawInstr per Q32, the init
# sequence per Q35, the shared exported cell per Q30, and the rejections
# (already linked, not loaded, not linkable, a cycle, writeQlc of a merge).
add_executable(linker_test tests/unit/linker_test.cpp)
target_link_libraries(linker_test PRIVATE qlang)
add_test(NAME linker COMMAND linker_test)

# Module search paths + the external search callback (SPEC §api "module search
# paths", §api "external search callback"; docs/plans/missing-features-plan.md
# C8a): QLangContext::moduleSearchPaths/setModuleSearchPaths/addModuleSearchPath
# and findModule's resolution order (C++ module, registry, callback, paths;
# .qlc before .ql, missing directories skipped, first match wins, resolved once
# per context), the callback's three answers and its precedence, transitive
# resolution of a discovered image's own imports, MODULE_NAME vs filename, and
# an entry that compiles, links and runs with nothing pre-loaded.
add_executable(module_search_test tests/unit/module_search_test.cpp)
target_link_libraries(module_search_test PRIVATE qlang)
add_test(NAME module_search COMMAND module_search_test)

# Universal output callback (SPEC §api "output callback (universal)";
# docs/plans/output-callback-plan.md P1): QLangContext::output as the one
# sink — default stdout/stderr routing byte-identical, the callback redirect
# exclusive, program output delivered at OUT_INFO with module name + line,
# trace at OUT_TRACE with the ""/0 sentinels, verbose gating unchanged, the
# must-not-throw guard, nullptr restoring defaults, and survival of clear().
add_executable(output_callback_test tests/unit/output_callback_test.cpp)
target_link_libraries(output_callback_test PRIVATE qlang)
add_test(NAME output_callback COMMAND output_callback_test)

# External C++ types (docs/EXTERNAL_TYPES.md): public-member setters,
# read-only computed props, methods (incl. BoundMethod extraction), enum
# constants, str, handoff integrity, registration-time validation.
add_executable(test_external tests/unit/test_external.cpp)
target_link_libraries(test_external PRIVATE qlang)
add_test(NAME external COMMAND test_external)

# QLM1 type-manifest codec (portable-module-plan P2, docs/SERIALIZATION.md
# §11): round-trip, truncation matrix, fingerprint stability/sensitivity.
add_executable(manifest_check tests/unit/manifest_check.cpp)
target_link_libraries(manifest_check PRIVATE qlang)
add_test(NAME manifest_check COMMAND manifest_check)
# CLI smoke: manifest_check runs `qlcc --qlmeta-dump <tmp>` (P3) and
# parses the file — exit 0 required, plus the refuse-to-combine check.
add_test(NAME qlmeta_dump COMMAND manifest_check $<TARGET_FILE:qlcc>)

# Compile-time external-type validation from a QLM1 manifest (portable-module-
# plan P4): the negative matrix, receiver liveness, enum folding/drift.
add_executable(manifest_compile tests/unit/manifest_compile.cpp)
target_link_libraries(manifest_compile PRIVATE qlang)
add_test(NAME manifest_compile COMMAND manifest_compile)
# CLI smoke: manifest_compile runs `qlcc --qlmeta ... -c ...` green/red.
add_test(NAME qlmeta_compile_cli COMMAND manifest_compile $<TARGET_FILE:qlcc>)

# QPM1 portable-module container (portable-module-plan P5, docs/SERIALIZATION.md
# §12): package round-trip with untouched payloads, the untrusted-input matrix,
# the load-time type handshake, and the --package / .qpm CLI flow.
add_executable(portable_module tests/unit/portable_module_test.cpp)
target_link_libraries(portable_module PRIVATE qlang)
# One entry, always with argv[1]: the --package / .qpm gate runs in-suite.
add_test(NAME portable_module COMMAND portable_module $<TARGET_FILE:qlcc>)

# Native/variable injection: ctx.set (incl. const) + ctx.setFunc + ctx.call.
add_executable(test_inject tests/unit/test_inject.cpp)
target_link_libraries(test_inject PRIVATE qlang)
add_test(NAME test_inject COMMAND test_inject)

# $ operator (stringify variable content, dynamic table keys).
add_executable(test_dollar_operator tests/unit/test_dollar_operator.cpp)
target_link_libraries(test_dollar_operator PRIVATE qlang)
add_test(NAME test_dollar_operator COMMAND test_dollar_operator)

# Phase 0 red tests (architecture-remediation-plan.md): scope/lifetime/
# instance defects (S1/S2/S3/P2) and host-API durability defects (P1/P3/F1).
# Expected to FAIL on the pre-fix tree; each goes green as its phase lands.
add_executable(phase0_scope tests/unit/phase0_scope.cpp)
target_link_libraries(phase0_scope PRIVATE qlang)
add_test(NAME phase0_scope COMMAND phase0_scope)

add_executable(phase0_api tests/unit/phase0_api.cpp)
target_link_libraries(phase0_api PRIVATE qlang)
add_test(NAME phase0_api COMMAND phase0_api)

# Phase 2 tests (architecture-remediation-plan.md): single global variable
# home — live global captures, delete isolation, host-set no-alias, and
# mid-run throw table consistency.
add_executable(phase2_scope tests/unit/phase2_scope.cpp)
target_link_libraries(phase2_scope PRIVATE qlang)
add_test(NAME phase2_scope COMMAND phase2_scope)

# Phase 3 tests (architecture-remediation-plan.md): host API hardening —
# stable type handles (shared_ptr, type_info validation), finalizer queue
# (enqueue at release, drain at safe points, thread-safe, re-entrancy
# guard), release() builtin for owned externals, and the context split
# (ProgramCache + TypeRegistry).
add_executable(phase3_api tests/unit/phase3_api.cpp)
target_link_libraries(phase3_api PRIVATE qlang)
add_test(NAME phase3_api COMMAND phase3_api)

# Phase 5 tests (architecture-remediation-plan.md): paper cuts — the
# context-owned cumulative step budget (stepsUsed/budget/step/resetSteps)
# and the one-context-one-thread contract.
add_executable(phase5_api tests/unit/phase5_api.cpp)
target_link_libraries(phase5_api PRIVATE qlang)
add_test(NAME phase5_api COMMAND phase5_api)

# Value semantics (docs/spec/01-types.md `## value semantics`, plan
# docs/plans/deepcopy-plan.md Phase 2): array/set/blob are pointer-backed, so
# they share on assignment and on call, a literal is fresh on every
# evaluation, and deepcopy is the only way to fork the storage.
add_executable(value_semantics tests/unit/value_semantics.cpp)
target_link_libraries(value_semantics PRIVATE qlang)
add_test(NAME value_semantics COMMAND value_semantics)

# Emitted-instruction budget gate (SPEC §bytecodes "encoding guidelines"): the
# entry-function instruction count of each budgeted script (the perf corpus)
# must stay within its ceiling, so a codegen growth fails ctest instead of being
# found by comparing two .qla dumps by hand. Lower a ceiling when the stream
# legitimately shrinks; raising one needs a reason in docs/compliance/.
add_executable(codegen_size_check tests/unit/codegen_size_check.cpp)
target_link_libraries(codegen_size_check PRIVATE qlang)
target_compile_definitions(codegen_size_check PRIVATE
  QLANG_TEST_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts"
)
add_test(NAME codegen_size COMMAND codegen_size_check)

# Script-level regression suite (tests/scripts/). Each .ql script is driven
# by qlcc --run (the smoke-test app); a non-zero exit code fails the suite.
# Explicit list (Phase 5, F7) — no file(GLOB), so a new script is a deliberate
# CMake edit, not a silent pickup. `ql_module.ql` is the exception that proves
# the rule: it imports `ql_module_dep.ql`, a sibling source module with no image
# (plan C8b), which is a fixture and so is not listed here.
set(QL_SCRIPTS
  cmp.ql
  cast.ql
  encode_repro.ql
  ext_consts.ql
  ext_regs.ql
  flags.ql
  helpers.ql
  hello.ql
  imm_kinds.ql
  jump_widths.ql
  len.ql
  money.ql
  moneyv2.ql
  ql_module.ql
  ql_module_const.ql
  ql_star_import.ql
  perf_stress.ql
  table_subscript.ql
  test_assert.ql
  test_assert2.ql
  test_cast.ql
  test_cast2.ql
  test_minmax.ql
  test_minmax2.ql
  type.ql
  value_share.ql
)
foreach(ql_script IN LISTS QL_SCRIPTS)
  get_filename_component(ql_name ${ql_script} NAME_WE)
  add_test(NAME script_${ql_name}
           COMMAND qlcc --run ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/${ql_script})
endforeach()

# Driver contract gate (docs/spec/18-qlang-run.md): drives the real qlcc
# executable over a generated fixture tree and checks the observable surface —
# the removed --output/--asm, the -c/-a/-r/--run/--output-path artifact rules,
# combinable flags, and the
# --import compile-and-cache rule (an image is reused only when it carries this
# build's .qlc version and is not older than its source). CMake script mode —
# no binary, no shell — so it runs on every CI job including MSVC (same
# reasoning as doc_hashes). The scratch directory is wiped and rebuilt by the
# script on every run, so the suite is repeatable and never touches the source
# tree.
add_test(NAME qlcc_cli
         COMMAND ${CMAKE_COMMAND}
                 -DQLCC=$<TARGET_FILE:qlcc>
                 -DQLCC_CLI_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/_qlcc_cli
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_qlcc_cli.cmake)

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

# Documentation hash gate (AGENTS.md "Keep Compliance Documentation Current"):
# every SHA256 recorded in docs/spec/README.md and docs/compliance/README.md
# must still match the module it names, every module must have a row, and no
# row may point at a deleted file. CMake script mode — no binary, no shell, so
# it runs on every CI job including MSVC. Re-record after a verified doc edit:
#   ./update-doc-hashes.sh
# The gate is optional by default: drift is still reported, but the
# QLANG_DOC_HASH_DRIFT marker is the entry's SKIP_REGULAR_EXPRESSION, so ctest
# prints "Skipped" instead of "Failed" and a stale doc row cannot break a CI
# job. -DQLANG_DOC_HASHES_STRICT=ON restores the hard failure; a broken gate
# (missing docs/, bad source dir) never carries the marker and always fails.
option(QLANG_DOC_HASHES_STRICT "Fail doc_hashes on documentation hash drift" OFF)
if(QLANG_DOC_HASHES_STRICT)
  add_test(NAME doc_hashes
           COMMAND ${CMAKE_COMMAND}
                   -DQLANG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_doc_hashes.cmake)
else()
  add_test(NAME doc_hashes
           COMMAND ${CMAKE_COMMAND}
                   -DQLANG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                   -DDOCS_HASH_WARN_ONLY=ON
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_doc_hashes.cmake)
  set_tests_properties(doc_hashes PROPERTIES
                       SKIP_REGULAR_EXPRESSION "QLANG_DOC_HASH_DRIFT")
endif()
