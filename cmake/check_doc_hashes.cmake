# check_doc_hashes.cmake — documentation hash gate.
#
# AGENTS.md ("Keep Compliance Documentation Current") requires every spec and
# compliance module to carry a SHA256 in its directory README, re-recorded
# after any verified change. This script enforces that mechanically:
#
#   1. every `*.md` module in docs/spec/ and docs/compliance/ has a table row
#      in the directory README (README.md itself is the index, not tracked);
#   2. the recorded SHA256 still matches the file on disk;
#   3. no recorded row points at a file that no longer exists.
#
# Usage:
#   cmake -DQLANG_SOURCE_DIR=<src> -P cmake/check_doc_hashes.cmake
#   cmake -DQLANG_SOURCE_DIR=<src> -DDOCS_HASH_UPDATE=ON \
#         -P cmake/check_doc_hashes.cmake
#
# QLANG_SOURCE_DIR defaults to the parent of this script's directory.
#
# DOCS_HASH_UPDATE=ON rewrites the recorded hashes in place and never fails on
# drift (that is its job). Per AGENTS.md, never run it before the suites are
# green: the hash only detects change, testing proves correctness.
#
# Registered as the `doc_hashes` ctest entry (see CMakeLists.txt). Uses
# file(GLOB) deliberately — an untracked new module must fail the gate, which
# is the opposite of the explicit-list rule used for test sources.
#
# CMake regex notes: `.` matches a newline and `{n}` repetition is unreliable,
# so rows are matched one line at a time (file(STRINGS)) with `[0-9a-f]+` plus
# an explicit 64-character length check.

if(NOT DEFINED QLANG_SOURCE_DIR)
  get_filename_component(QLANG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT IS_DIRECTORY "${QLANG_SOURCE_DIR}/docs")
  message(FATAL_ERROR "QLANG_SOURCE_DIR '${QLANG_SOURCE_DIR}' has no docs/ directory")
endif()

set(_qlh_ok 0)
set(_qlh_updated "")
set(_qlh_drift "")
set(_qlh_missing "")
set(_qlh_stale "")
set(_qlh_malformed "")
set(_qlh_indexes 0)

# One index (a README whose table carries a SHA256 column) plus the directory
# its rows point into.
macro(_qlh_check_index _qlh_index _qlh_dir)
  if(NOT EXISTS "${_qlh_index}")
    message(FATAL_ERROR "missing index file: ${_qlh_index}")
  endif()
  math(EXPR _qlh_indexes "${_qlh_indexes} + 1")
  file(READ "${_qlh_index}" _qlh_content)
  file(RELATIVE_PATH _qlh_rel "${QLANG_SOURCE_DIR}" "${_qlh_dir}")

  # Collect the recorded rows: module name -> hash, plus the row text (needed
  # to rewrite a single row in update mode).
  set(_qlh_names "")
  set(_qlh_hashes "")
  set(_qlh_rows "")
  # ENCODING UTF-8 matters: the row text contains non-ASCII characters (en
  # dashes, arrows) and the default ASCII reading would truncate those lines
  # before the SHA256 cell.
  file(STRINGS "${_qlh_index}" _qlh_lines ENCODING UTF-8)
  foreach(_qlh_line IN LISTS _qlh_lines)
    if(NOT _qlh_line MATCHES "^\\| \\[([^]|]+\\.md)\\]\\(")
      continue()
    endif()
    set(_qlh_rname "${CMAKE_MATCH_1}")
    if(_qlh_line MATCHES "`([0-9a-f]+)` *\\|$")
      set(_qlh_rhash "${CMAKE_MATCH_1}")
    else()
      list(APPEND _qlh_malformed
           "${_qlh_rel}/${_qlh_rname} (row has no SHA256 cell)")
      continue()
    endif()
    string(LENGTH "${_qlh_rhash}" _qlh_rlen)
    if(NOT _qlh_rlen EQUAL 64)
      list(APPEND _qlh_malformed
           "${_qlh_rel}/${_qlh_rname} (SHA256 cell is ${_qlh_rlen} chars, not 64)")
      continue()
    endif()
    list(FIND _qlh_names "${_qlh_rname}" _qlh_pos)
    if(_qlh_pos GREATER -1)
      list(APPEND _qlh_malformed
           "${_qlh_rel}/${_qlh_rname} (recorded twice in ${_qlh_rel}/README.md)")
      continue()
    endif()
    list(APPEND _qlh_names "${_qlh_rname}")
    list(APPEND _qlh_hashes "${_qlh_rhash}")
    list(APPEND _qlh_rows "${_qlh_line}")
  endforeach()

  # 1 + 2: every module file must have a row, and that row must match.
  file(GLOB _qlh_modules "${_qlh_dir}/*.md")
  list(SORT _qlh_modules)
  foreach(_qlh_mod IN LISTS _qlh_modules)
    get_filename_component(_qlh_name "${_qlh_mod}" NAME)
    if(_qlh_name STREQUAL "README.md")
      continue()
    endif()

    list(FIND _qlh_names "${_qlh_name}" _qlh_pos)
    file(SHA256 "${_qlh_mod}" _qlh_actual)

    if(_qlh_pos EQUAL -1)
      list(APPEND _qlh_missing
           "${_qlh_rel}/${_qlh_name} (no SHA256 row in ${_qlh_rel}/README.md)")
      continue()
    endif()
    list(GET _qlh_hashes ${_qlh_pos} _qlh_recorded)

    if(_qlh_recorded STREQUAL _qlh_actual)
      math(EXPR _qlh_ok "${_qlh_ok} + 1")
    elseif(DOCS_HASH_UPDATE)
      # Rewrite only this row: the row text names the module, so it is unique
      # in the index and a hash shared with another row is left alone.
      list(GET _qlh_rows ${_qlh_pos} _qlh_row)
      string(REPLACE "${_qlh_recorded}" "${_qlh_actual}" _qlh_newrow "${_qlh_row}")
      string(REPLACE "${_qlh_row}" "${_qlh_newrow}" _qlh_content "${_qlh_content}")
      list(APPEND _qlh_updated "${_qlh_rel}/${_qlh_name}")
    else()
      list(APPEND _qlh_drift
           "${_qlh_rel}/${_qlh_name}\n    recorded ${_qlh_recorded}\n    actual   ${_qlh_actual}")
    endif()
  endforeach()

  # 3: a row whose file is gone.
  foreach(_qlh_name IN LISTS _qlh_names)
    if(NOT EXISTS "${_qlh_dir}/${_qlh_name}")
      list(APPEND _qlh_stale
           "${_qlh_rel}/${_qlh_name} (row in ${_qlh_rel}/README.md, file missing)")
    endif()
  endforeach()

  if(_qlh_updated)
    file(WRITE "${_qlh_index}" "${_qlh_content}")
  endif()
endmacro()

_qlh_check_index("${QLANG_SOURCE_DIR}/docs/spec/README.md"
                 "${QLANG_SOURCE_DIR}/docs/spec")
_qlh_check_index("${QLANG_SOURCE_DIR}/docs/compliance/README.md"
                 "${QLANG_SOURCE_DIR}/docs/compliance")

list(LENGTH _qlh_updated _qlh_updated_n)
list(LENGTH _qlh_drift _qlh_drift_n)
list(LENGTH _qlh_missing _qlh_missing_n)
list(LENGTH _qlh_stale _qlh_stale_n)
list(LENGTH _qlh_malformed _qlh_malformed_n)

if(_qlh_updated_n)
  message("doc hashes re-recorded (${_qlh_updated_n}):")
  foreach(_qlh_item IN LISTS _qlh_updated)
    message("  ${_qlh_item}")
  endforeach()
endif()

if(NOT _qlh_drift AND NOT _qlh_missing AND NOT _qlh_stale AND NOT _qlh_malformed)
  message("doc hashes: ${_qlh_ok} modules verified across ${_qlh_indexes} indexes")
endif()

set(_qlh_problems "")
if(_qlh_drift_n)
  list(APPEND _qlh_problems "${_qlh_drift_n} stale SHA256 (file changed since it was recorded):")
  list(APPEND _qlh_problems "${_qlh_drift}")
endif()
if(_qlh_missing_n)
  list(APPEND _qlh_problems "${_qlh_missing_n} module(s) without a SHA256 row:")
  list(APPEND _qlh_problems "${_qlh_missing}")
endif()
if(_qlh_stale_n)
  list(APPEND _qlh_problems "${_qlh_stale_n} SHA256 row(s) without a module file:")
  list(APPEND _qlh_problems "${_qlh_stale}")
endif()
if(_qlh_malformed_n)
  list(APPEND _qlh_problems "${_qlh_malformed_n} malformed SHA256 row(s):")
  list(APPEND _qlh_problems "${_qlh_malformed}")
endif()

if(_qlh_problems)
  list(JOIN _qlh_problems "\n  " _qlh_report)
  message(FATAL_ERROR
          "doc hash check failed:\n  ${_qlh_report}\n"
          "After the suites are green, re-record with:\n"
          "  cmake -DQLANG_SOURCE_DIR=${QLANG_SOURCE_DIR} -DDOCS_HASH_UPDATE=ON "
          "-P cmake/check_doc_hashes.cmake")
endif()
