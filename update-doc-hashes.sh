#!/usr/bin/env bash
# Re-record the documentation SHA256 rows by hand (AGENTS.md "Keep Compliance
# Documentation Current"). Only the rows whose module actually changed are
# rewritten; everything else in the two READMEs is left byte-for-byte alone.
#
#   ./update-doc-hashes.sh          re-record stale rows (run the tests FIRST)
#   ./update-doc-hashes.sh --check  verify only: exit 1 on drift, writes nothing
#
# Order of operations (AGENTS.md): build, run ctest, flip the relevant Open
# Items, and only then re-record — testing proves correctness, the hash merely
# detects change.
#
# Tracked: every docs/spec/*.md and docs/compliance/*.md module, recorded in the
# SHA256 column of that directory's README.md (a README's own hash is not
# tracked, so editing an index never requires re-hashing it). The hash is taken
# over LF-normalized content, so a CRLF working tree does not report drift.
#
# The doc_hashes ctest entry runs the same check in warn-only mode (drift is
# reported, then ctest marks the entry Skipped); this script is the manual step
# that clears the report.
set -euo pipefail
cd "$(dirname "$0")"

GATE="cmake/check_doc_hashes.cmake"

if [[ ! -f "$GATE" ]]; then
  echo "error: $GATE not found — run this script from the repository root" >&2
  exit 2
fi

case "${1:-update}" in
  check | --check)
    # Same exit status as a strict doc_hashes run: 0 = every row is current.
    cmake -DQLANG_SOURCE_DIR="$PWD" -P "$GATE"
    ;;
  update | --update)
    echo "Re-recording docs/spec/README.md + docs/compliance/README.md rows in $PWD"
    echo "(reminder: cmake --build build && ctest --test-dir build must be green first)"
    echo
    cmake -DQLANG_SOURCE_DIR="$PWD" -DDOCS_HASH_UPDATE=ON -P "$GATE"
    echo
    git diff --stat -- docs/spec/README.md docs/compliance/README.md || true
    ;;
  *)
    echo "usage: $0 [--check|--update]" >&2
    exit 2
    ;;
esac
