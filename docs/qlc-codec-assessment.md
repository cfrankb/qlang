# Architecture assessment: 12-byte `Instruction` + library `.qlc` codec

Date: 2026-07-11
Scope: the two-commit change described in [plans/qlc-codec-plan.md](plans/qlc-codec-plan.md) —
(1) `Instruction` restructured to a 12-byte record with a `b/c | i32` union,
(2) the `.qlc` codec (`serializeValue`/`deserializeValue`/`writeQlc`/`readQlc`)
moved from the `qlang_run` POC into `libqlang.a`.
Format spec: [SERIALIZATION.md](SERIALIZATION.md) · Operand map: [spec/13-bytecodes.md](spec/13-bytecodes.md)

**Verdict: net architectural improvement.** Serialization moves from a POC inside
an example app to a tested library layer, and the instruction format becomes
self-describing. The costs are real but bounded.

## Improvements

- **The struct *is* the format.** Before, the 25-byte POC encoding lived in
  `examples/qlang_run.cpp`, hand-written field by field, and drifted from the
  in-memory struct. Now the raw 12 bytes of `Instruction` are the on-disk
  record, pinned by `static_assert(sizeof(Instruction) == 12)`. One place to
  look; one assert to break if it changes.
- **One source of truth for operand width.** `usesI32(Opcode)` drives the
  compiler's `Bytecode::add`, the VM's decode sites, and the disassembler.
  Before, "which operand is 32-bit" was tribal knowledge repeated in ~75
  places (55 backpatch sites, 20 decode sites, the disassembler).
- **Real layering.** The codec is in `libqlang.a` with a public header
  (`include/qlang/QLangBytecodeIO.h`); the app calls it. Snapshot v2, a REPL,
  or a loader can reuse `serializeValue` without touching example code.
- **The format is now verifiable.** There was no read path before — a
  write-only format is untestable. `readQlc` defines the load contract
  (bounds-checked reads, structural invariants, no partial out-params,
  `QLangError(Io)` with 0-based byte offsets) and the `codec` ctest
  (`examples/codec_check.cpp`) pins it: value round-trips, a compiled-program
  round-trip with native wiring, and a malformed-input suite.
- **It caught a latent bug.** The native-slot table invariant (unique,
  ascending) exposed that `QLangCompiler::addExternalFuncs` reused ids across
  calls — two natives could share a slot, a silent runtime hazard. It is now a
  format error on write/read, and the root cause is fixed (ids are unique
  across all calls). The int16 range check in `Bytecode::add` likewise turns
  operand overflow into a named compiler error instead of truncation.

## Costs / risks

- **Memory layout and file format are now the same decision.** Any future
  `Instruction` change is simultaneously an ABI change and a format break. The
  `version` field is the escape hatch (the reader rejects `version > 1`), but
  there is no migration story yet.
- **The union is a footgun that already bit.** `b`/`c` are the two 16-bit
  halves of `i32`, expressed as an anonymous struct inside the union. Bare
  `int16_t b, c` members of a union alias the same storage — the plan's own
  sketch had that bug, and it cost a debug session to find. The next person
  adding an opcode must remember `usesI32()`; forgetting it yields a range
  check throw or a silent zero, not a compile error.
- **A second place where format invariants live.** `readQlc` re-validates
  opcodes, closure indices, and slot order — logic the compiler already
  enforces. Necessary for untrusted input, but duplicated surface that must
  stay in sync; the codec test suite catches drift.
- **New hard limits.** Operands are int16: register counts, constant-pool
  indices, and call-argument counts are bounded at ±32767. Fine for this
  language and explicit via the range check, but a capability reduction vs.
  the old all-i32 in-memory layout.
- **The hole in the middle.** `readQlc` exists, but
  `QLangContext::loadProgram`/`execute` (SERIALIZATION.md §5.2/§6) do not, so
  nothing in the library can *run* a `.qlc` yet. The round-trip test compares
  structure, not execution. Not a regression (there was no reader before), but
  the stated goal — "re-run a script without the source" — is still unproven
  end-to-end.

## Next step

The deferred `loadProgram`/`execute` pair is the cheapest way to close the
biggest remaining risk: it converts "format round-trips" into "program
behaves identically from a file" (SERIALIZATION.md §9 items 2 and 4).

## Not assessed here

- No performance comparison: 12-byte records should be neutral-to-better for
  instruction-cache behavior, but VM throughput was not benchmarked.
- The assessment covers the codec and instruction layout only; parser/VM
  internals are unchanged by this work apart from the jump-decode sites.
