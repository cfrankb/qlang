# check_qlcc_cli.cmake — qlcc command-line contract gate.
#
# docs/spec/18-qlang-run.md defines the driver's surface: the program runs only
# with --run, --output/--asm are gone, -c (save the compiled image), -a (save a
# disassembly listing), -r (--rotate) and --output-path replace them, -c/-a/-r
# combine (-ca, -car), the artifact names are derived from the entry file,
# --strict parses, is range-checked, and configures the context, and --import
# resolves a
# MODULE (compiling and saving its .qlc when the image is missing, of another
# .qlc version, or older than its source) instead of requiring a path to an
# image.
#
# This gate drives the real executable over a generated fixture tree and checks
# the observable results: exit codes, which files appeared where, and what the
# driver reported on stderr. CMake script mode — no binary, no shell — so it
# runs on every CI job including MSVC (same reasoning as check_doc_hashes).
#
# Usage:
#   cmake -DQLCC=<build>/qlcc \
#         -DQLCC_CLI_WORK_DIR=<scratch dir> \
#         -P cmake/check_qlcc_cli.cmake
#
# Registered as the `qlcc_cli` ctest entry (see CMakeLists.txt). The work
# directory is wiped and rebuilt on every run, so the suite is repeatable and
# never touches the source tree.
#
# Timestamps: the freshness rule compares mtimes, so a fixture that must be
# newer than an image is touched only after a sleep longer than the coarsest
# filesystem timestamp granularity (1 s on some volumes).

if(NOT DEFINED QLCC)
  message(FATAL_ERROR "QLCC (path to the qlcc executable) is not defined")
endif()
if(NOT EXISTS "${QLCC}")
  message(FATAL_ERROR "QLCC '${QLCC}' does not exist")
endif()
if(NOT DEFINED QLCC_CLI_WORK_DIR)
  message(FATAL_ERROR "QLCC_CLI_WORK_DIR (scratch directory) is not defined")
endif()

set(W "${QLCC_CLI_WORK_DIR}")
file(REMOVE_RECURSE "${W}")
file(MAKE_DIRECTORY "${W}/libs" "${W}/nest" "${W}/only")

# --- fixtures ---------------------------------------------------------------
# A library, an entry that imports it, a dependency chain (main -> aa -> bb) and
# a script with no imports at all. Library sources declare MODULE_NAME because
# library mode makes it mandatory (SPEC §bytecodes "keyword export").
file(WRITE "${W}/libs/greeter.ql" [[
export const MODULE_NAME = "greeter"
export def hello(name)
  return "hello, " + name
end
]])
file(WRITE "${W}/plain.ql" [[
local n = 6
println(n * 7)
]])
file(WRITE "${W}/dupdecl.ql" [[
local a
local a
println(a)
]])
file(WRITE "${W}/strictcalls.ql" [[
def hello_world(name:str, age:int) {
}
hello_world("Tommy", "24")
]])
file(WRITE "${W}/app.ql" [[
import greeter
println(greeter.hello("world"))
]])
file(WRITE "${W}/nest/bb.ql" [[
export const MODULE_NAME = "bb"
export def twice(x)
  return x + x
end
]])
file(WRITE "${W}/nest/aa.ql" [[
export const MODULE_NAME = "aa"
import bb
export def quad(x)
  return bb.twice(bb.twice(x))
end
]])
file(WRITE "${W}/nest/main.ql" [[
import aa
println(aa.quad(21))
]])

# --- harness ----------------------------------------------------------------
# Run the driver inside the fixture tree (the module search paths default to the
# working directory, so relative arguments resolve as they would for a user).
macro(run)
  execute_process(
    COMMAND ${QLCC} ${ARGN}
    WORKING_DIRECTORY "${W}"
    RESULT_VARIABLE RC
    OUTPUT_VARIABLE OUT
    ERROR_VARIABLE ERR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
endmacro()

# Make a file strictly newer than anything written before it.
macro(touch_new file)
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
  file(TOUCH "${file}")
endmacro()

# The checks are explicit macros, not a generic `if(${ARGN})` evaluator: the
# driver's stdout/stderr carry newlines and semicolons, and CMake turns a
# semicolon inside an expanded variable into an argument separator, which
# destroys the quoting of any expression passed through as text.
set(QL_CLI_OK 0)
set(QL_CLI_FAIL "")

macro(_ql_pass)
  math(EXPR QL_CLI_OK "${QL_CLI_OK} + 1")
endmacro()

macro(_ql_fail label)
  list(APPEND QL_CLI_FAIL "${label}  [rc=${RC} stderr=${ERR}]")
endmacro()

# Exit code of the last run.
macro(expect_rc label want)
  if(RC EQUAL "${want}")
    _ql_pass()
  else()
    _ql_fail("${label} (expected exit ${want})")
  endif()
endmacro()

# The last run's stream matches a regular expression.
macro(expect_match label stream regex)
  if("${${stream}}" MATCHES "${regex}")
    _ql_pass()
  else()
    _ql_fail("${label} (no match for '${regex}')")
  endif()
endmacro()

macro(expect_no_match label stream regex)
  if("${${stream}}" MATCHES "${regex}")
    _ql_fail("${label} (unexpected match for '${regex}')")
  else()
    _ql_pass()
  endif()
endmacro()

# A path produced by the last run.
macro(expect_exists label path)
  if(EXISTS "${path}")
    _ql_pass()
  else()
    _ql_fail("${label} (missing ${path})")
  endif()
endmacro()

macro(expect_missing label path)
  if(EXISTS "${path}")
    _ql_fail("${label} (unexpected ${path})")
  else()
    _ql_pass()
  endif()
endmacro()

macro(expect_dir label path)
  if(IS_DIRECTORY "${path}")
    _ql_pass()
  else()
    _ql_fail("${label} (not a directory ${path})")
  endif()
endmacro()

# How many files a glob matched.
macro(expect_count label glob_pattern want)
  file(GLOB _ql_glob ${glob_pattern})
  list(LENGTH _ql_glob _ql_n)
  if(_ql_n EQUAL "${want}")
    _ql_pass()
  else()
    _ql_fail("${label} (glob ${glob_pattern} matched ${_ql_n}, expected ${want})")
  endif()
endmacro()

# --- the removed options are gone -------------------------------------------
run(--output x.qlc plain.ql)
expect_rc("--output is no longer accepted" 1)
expect_match("--output is reported as an unknown option" ERR "unknown option")
run(--asm x.qla plain.ql)
expect_rc("--asm is no longer accepted" 1)

# --strict parses, is range-checked, and configures the context
# (docs/spec/16-strict_typing.md). Enforcement landed with the duplicate
# declaration diagnostic: level 1 warns, level 2 is a compile error (exit 3,
# the driver's compile-failure code), level 0 says nothing.
run(--strict 3 plain.ql)
expect_rc("--strict 3 is out of range" 1)
expect_match("--strict names the accepted levels" ERR "0, 1, or 2")
run(--strict plain.ql)
expect_rc("--strict without a level exits 1" 1)
run(--strict 0 dupdecl.ql)
expect_rc("--strict 0 compiles a duplicate declaration" 0)
expect_no_match("--strict 0 says nothing" ERR "duplicated variable")
run(--strict 1 dupdecl.ql)
expect_rc("--strict 1 compiles a duplicate declaration" 0)
expect_match("--strict 1 warns" ERR "warning: duplicated variable .a. declaration within scope")
run(--strict 2 dupdecl.ql)
expect_rc("--strict 2 rejects a duplicate declaration" 3)
expect_match("--strict 2 reports the error" ERR "duplicated variable .a. declaration within scope")

# Call-boundary enforcement carries to the driver (docs/spec/16-strict_typing.md
# "error messages"): a mismatched argument raises at run time with the pinned
# spec message and the driver's failure exit code; level 0 runs it silently.
run(--strict 0 --run strictcalls.ql)
expect_rc("--strict 0 does not check the call boundary" 0)
expect_no_match("--strict 0 says nothing about parameters" ERR "expects int")
run(--strict 1 --run strictcalls.ql)
expect_rc("--strict 1 raises at the call boundary" 3)
expect_match("--strict 1 prints the pinned spec message" ERR "parameter 'age' of 'hello_world' expects int, got string")
run(--strict 2 --run strictcalls.ql)
expect_rc("--strict 2 raises at the call boundary" 3)

# --- help still answers -----------------------------------------------------
run(--help)
expect_rc("--help exits 0" 0)
expect_match("--help documents --output-path" OUT "output-path")
expect_match("--help documents -c" OUT "-c  ")
expect_match("--help documents --run" OUT "--run")
expect_match("--help documents --strict" OUT "--strict")
expect_match("--help documents combinable flags" OUT "-ca")
expect_no_match("--help no longer advertises --output <file>" OUT "--output ")

# --- the program runs only with --run ---------------------------------------
# Without --run the driver compiles: nothing executes, and with no -c/-a/-r
# there is no artifact at all.
run(plain.ql)
expect_rc("plain compile-only exits 0" 0)
expect_no_match("without --run the program does not execute" OUT "42")
expect_count("compile-only writes no artifact" "${W}/plain.qlc" 0)
expect_count("compile-only writes no listing" "${W}/plain.qla" 0)

# -c names its file and defaults to the entry's directory; it does not execute.
run(-c plain.ql)
expect_rc("-c exits 0" 0)
expect_no_match("-c does not execute the program" OUT "42")
expect_exists("-c wrote <stem>.qlc beside the source" "${W}/plain.qlc")
expect_match("-c reports the artifact" ERR "bytecode written")
# -a lists and does not execute either.
run(-a plain.ql --output-path aonly)
expect_rc("-a exits 0" 0)
expect_no_match("-a does not execute the program" OUT "42")
expect_exists("-a wrote <stem>.qla" "${W}/aonly/plain.qla")
expect_match("-a reports the listing" ERR "disassembly written")
expect_missing("-a alone writes no image" "${W}/aonly/plain.qlc")
# --run executes and, with no artifact flags, writes nothing.
run(--run plain.ql)
expect_rc("--run exits 0" 0)
expect_match("--run executes the program" OUT "42")
expect_count("--run alone writes no artifact" "${W}/plain.qla" 0)

# --- combinable short flags --------------------------------------------------
run(-ca plain.ql --output-path combo)
expect_rc("-ca combines -c and -a" 0)
expect_exists("-ca wrote the image" "${W}/combo/plain.qlc")
expect_exists("-ca wrote the listing" "${W}/combo/plain.qla")
expect_no_match("-ca does not execute the program" OUT "42")
run(-car plain.ql --output-path combo2)
expect_rc("-car adds --rotate" 0)
expect_count("-car rotates the image name" "${W}/combo2/plain_*.qlc" 1)
expect_count("-car rotates the listing name" "${W}/combo2/plain_*.qla" 1)
expect_missing("-car is not a package request" "${W}/combo2/plain.qpm")

# --- --output-path ------------------------------------------------------------
run(-c -a --output-path out plain.ql)
expect_rc("--output-path run exits 0" 0)
expect_dir("--output-path is created" "${W}/out")
expect_exists("--output-path holds the image" "${W}/out/plain.qlc")
expect_exists("--output-path holds the listing" "${W}/out/plain.qla")
run(-c -a --output-path=nested/deep plain.ql)
expect_exists("--output-path= form works" "${W}/nested/deep/plain.qlc")
expect_missing("--output-path leaves the source directory clean" "${W}/nested.qlc")

# --- -r / --rotate ------------------------------------------------------------
run(-c -a -r --output-path rot plain.ql)
expect_count("-r rotates the image name" "${W}/rot/plain_*.qlc" 1)
expect_count("-r rotates the listing name" "${W}/rot/plain_*.qla" 1)
expect_missing("-r does not also write the plain name" "${W}/rot/plain.qlc")
run(-c --rotate --output-path rot2 plain.ql)
expect_count("--rotate is the long form of -r" "${W}/rot2/plain_*.qlc" 1)

# --- --import compiles a .ql and caches it as a .qlc ---------------------------
# These runs pass --run so the checks below observe both the compile cache on
# disk and the program output; the caching itself happens at compile time.
run(--run --import libs/greeter app.ql)
expect_rc("--import of a source compiles and runs" 0)
expect_match("--import made the library callable" OUT "hello, world")
expect_exists("--import saved the image beside the source" "${W}/libs/greeter.qlc")
expect_match("--import reports the compile" ERR "module compiled")
run(--run --import libs/greeter app.ql)
expect_rc("a current image is loaded" 0)
expect_no_match("a current image is not recompiled" ERR "module compiled")
touch_new("${W}/libs/greeter.ql")
run(--run --import libs/greeter app.ql)
expect_match("a source newer than its image recompiles" ERR "module compiled")
# An image whose header is not this build's QLC1 v9 is stale even when it is
# newer than the source, so it must be rebuilt rather than rejected at load.
file(WRITE "${W}/libs/greeter.qlc" "QLC1 not a version header\n")
run(--run --import libs/greeter app.ql)
expect_match("an image of another version recompiles" ERR "module compiled")
expect_rc("the rebuilt image loads" 0)
# A bare name resolves on the module search paths, .qlc before .ql.
run(--run --module-path libs --import greeter app.ql)
expect_rc("a bare module name resolves on the search path" 0)
expect_no_match("a bare name reuses the current image" ERR "module compiled")
# A module with an image and no source beside it is loaded as-is.
file(COPY "${W}/libs/greeter.qlc" DESTINATION "${W}/only")
run(--run --import only/greeter app.ql)
expect_rc("an image with no source loads" 0)
expect_no_match("an image with no source is not recompiled" ERR "module compiled")
# A name nothing can answer is a missing file, not a QLang error.
run(--run --import nosuch app.ql)
expect_rc("a module that is nowhere exits 2" 2)
expect_match("a missing module names what was asked for" ERR "nosuch")

# --- -a lists every image it generates ----------------------------------------
touch_new("${W}/libs/greeter.ql")
run(-a --import libs/greeter app.ql)
expect_exists("-a lists a generated image" "${W}/libs/greeter.qla")
expect_exists("-a still lists the entry" "${W}/app.qla")
# Transitive dependencies are compiled by the same rule, and listed too.
run(-a --module-path nest --import nest/aa nest/main.ql)
expect_exists("a dependency is compiled transitively" "${W}/nest/bb.qlc")
expect_exists("a transitive dependency is listed with -a" "${W}/nest/bb.qla")
# Generated images keep their plain name: a rotated one could never be found
# again by the name its dependent imports.
touch_new("${W}/nest/bb.ql")
run(-r -a --module-path nest --import nest/bb nest/main.ql)
expect_count("a generated image is never rotated" "${W}/nest/bb_*.qlc" 0)
expect_exists("a generated image keeps its plain name" "${W}/nest/bb.qlc")

# --- positional library arguments behave like --import ------------------------
# The first positional is the entry, every later one a module (see --help), so
# the library is spelled after the script it serves.
run(--run app.ql libs/greeter.qlc)
expect_rc("a positional image still loads" 0)
expect_match("a positional image makes the library callable" OUT "hello, world")
touch_new("${W}/libs/greeter.ql")
run(--run app.ql libs/greeter.ql)
expect_match("a positional source is compiled like --import" ERR "module compiled")
expect_rc("a positional source runs the entry" 0)

# --- -c writes the entry's own module image -----------------------------------
# The entry imports, so the run needs the linked program; the image on disk is
# the entry module, which is what .qlc v9 can represent.
run(-c --module-path nest --import nest/aa nest/main.ql)
expect_rc("-c on an importing entry succeeds" 0)
expect_no_match("-c does not execute the entry" OUT "84")
expect_exists("-c wrote the entry image" "${W}/nest/main.qlc")
expect_match("-c says the linked program is not serializable" ERR "not serializable")
# A .qlc entry is compiled as-is: it too needs --run to execute.
run(plain.qlc)
expect_rc("an entry image compiles without --run" 0)
expect_no_match("an entry image does not execute without --run" OUT "42")
run(--run --module-path nest --import nest/aa nest/main.qlc)
expect_rc("the written entry image runs" 0)
expect_match("the written entry image still prints" OUT "84")
run(--module-path nest --import nest/aa -c --run nest/main.qlc)
expect_match("-c on its own source skips the write" ERR "skipping bytecode")
expect_rc("-c on its own source still runs with --run" 0)

# --- report -------------------------------------------------------------------
if(NOT QL_CLI_FAIL)
  message("qlcc cli: ${QL_CLI_OK} checks passed")
else()
  list(LENGTH QL_CLI_FAIL QL_CLI_FAIL_N)
  list(JOIN QL_CLI_FAIL "\n  " QL_CLI_REPORT)
  message(FATAL_ERROR "qlcc cli check failed (${QL_CLI_FAIL_N} of "
                      "${QL_CLI_OK} checks):\n  ${QL_CLI_REPORT}")
endif()
