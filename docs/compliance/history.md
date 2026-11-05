## Round 39 (2026-11-04)

**Phase 1 of `docs/plans/missing-features-plan.md` — A1 `nop` opcode + A2 `?`
ternary operator.** No spec changes; no `.qlc` format change (v5). One new
opcode (`nop`, appended last); the ternary reuses existing branch opcodes.
- **A1 `nop`:** `Opcode::Nop` appended **last** in the enum (identity 68) so
  existing `.qlc` opcode numbers stay valid; `Bytecode::name` → `"nop"`;
  zero-operand (base size 1 byte; the escaped form is rejected — a 0-operand
  opcode has nothing to widen); no-op in the VM dispatch; the compiler never
  emits it (spec: "this OpCode takes no action"). 12 new `codec_check` cases
  (identity, name, validity, size, 1-byte encode/decode round-trip,
  escaped-form rejection, `.qlc` program round-trip).
- **A2 `?` ternary:** `TokenType::Question` (`?`) in the lexer. Parser
  `parseTernary` sits **below every binary operator** (lowest precedence) and
  is right-associative (`a ? b : c ? d : e` → `a ? b : (c ? d : e)`); each
  branch is an expression or a brace block — a `{` in branch position is
  always a block (a table-literal branch must be parenthesized). Codegen:
  condition to a temp, `JMPIFFALSE` to the false side, `JMP` past it; block
  branches lower to a zero-arg closure call (`compileTernaryBlock`) whose
  `return` yields the ternary's value. **Capture fix:** F3 removed implicit
  capture of enclosing locals, so the block's free variables are computed
  (`collectStmtIds` − `collectLocalVars`, minus names `buildFunction`
  auto-resolves) and passed as an **explicit capture list** (the
  arrow-closure pattern); at script level top-level locals have live
  registers and are captured like locals, inside a function scope
  unimported globals stay invisible (clean `use of undeclared variable`
  error). `collectIds` (free-var detection) gained a `Ternary` arm so a
  ternary inside a function body captures its sub-expression free vars.
  16 new `spec_check2` cases (both branches, nesting, block form, mixed
  branches, free-var capture, precedence vs `&&`/`||`/arithmetic, closure +
  script-level scope).
- **Tally:** `spec_check2` 463 → 479, `codec_check` 96 → 108; **834 counted
  unit cases**, 41/41 ctest green.
- **Docs:** `status.md` — `?` gap row → implemented, `nop` Open Item →
  implemented, Current Status tally + Round 39 note, `?` added to the
  Working operators list.

## Round 38 (2026-11-04)

**Constant-pool dedup restored** (SPEC §bytecodes `## constant pool`: "constant
in the pool must have unique values. duplicated entries should be folded into a
single one."). The pre-Phase-4 dedup was dropped during the per-function
`FnEmitter` refactor; this round recovers it from git history and adapts it to
the current architecture. No language changes, no new opcodes, no `.qlc`
format change (v5).
- **`valueKey(QLangValue)`** (free function, `QLangBytecode.cpp`): canonical
  byte-string key — a type byte + payload. Value types key structurally
  (recursively: scalars by bits, String/Blob by len+bytes, Array/Set by
  len+subkeys, Table by size+key-sorted entries+subkeys); `Function` keys on
  its compile-time spec (funcIndex + capture regs — cells bind at MAKEFUNC
  time); the remaining reference types (Generator, Object, External,
  BoundMethod, TypeRef) key by **identity** (pointer) so distinct objects never
  collide. (The old version remapped those to Nil — a latent collision — fixed
  here.)
- **`Bytecode::addConstant`/`setConstants`** (`QLangBytecode.cpp`): keep a
  `std::unordered_map<std::string,int> m_constKeys` (key → pool index) in sync
  with `m_consts`; `addConstant` returns the existing index on a key hit,
  `setConstants` rebuilds the map.
- **`FnEmitter::addConst`** (`QLangCompiler.h`): the per-function emitter dedups
  the same way via its own `constKeys` map before `finish()` moves `consts` →
  `fn.constants`, so each function's pool holds unique values.
- **Tests:** 11 new `codec_check` pool-dedup cases — repeated strings/nil/
  floats/bools/arrays fold to the right unique count, no-duplicate scans over
  the pools, folded scalars + arrays load correctly at runtime, and a real
  program (def + string concat) is duplicate-free across all function pools.
  Full suite green (41/41 ctest; 806 counted unit cases).
- **Docs:** `status.md` §bytecodes gains the constant-pool row; the
  "Current Status" tally is corrected to include `encoding_test` (41) and the
  19 script regressions (was 754/16 — both under-counted vs `tests.md`).

## Round 37 (2026-11-04)

**Instruction format — escape-byte encoding (flat byte stream)** (plan:
`docs/plans/instruction-format-escape-byte-implementation-plan.md`; design:
`docs/plans/instruction-format-escape-byte-plan.md`). No language changes, no
new opcodes (68 of 128 identities used). The `.qlc` format bumps 4 → 5 and
**drops support for v1–v4** (v5-only reader, per user direction).
- **Flat byte stream:** the fixed 8-byte `Instruction` struct is deleted.
  `Function` now stores `std::vector<uint8_t> code` + `std::vector<size_t>
  instrOffsets` (byte offset of each instruction); `loc` stays parallel to
  `instrOffsets`. `Bytecode` wraps the byte buffer. `usesI32()` removed.
- **Escape-byte opcode:** opcode byte bit7 = escape flag, bits0–6 = identity
  (128 base ops). The base (no-escape) form is uniformly 1-byte operands:
  register/pool index 1 byte, immediate 1-byte signed Int (−128..127), jump
  `jmp8` 1-byte relative. The escaped form adds a modifier byte `e` that
  widens operands: 2-byte reg/pool, 2/4-byte signed immediate, `jmp16`
  (e=0) / `jmp32` (e=1). Jumps are three-tier relative (`jmp8`/`jmp16`/
  `jmp32`); the compiler picks the narrowest that fits via iterative
  backpatch. Single register pool + single constant pool (index 0–255 → 1
  byte, 256–65535 → 2 bytes; no cap, no spill path).
- **Compiler:** per-function `FnEmitter` raw-instruction buffer; `finish()`
  computes sizes, `instrOffsets`, the jump-width fixpoint, and encodes into
  `fn.code`. `buildFunction`/`compileClass` save/restore the emitter.
- **VM:** byte-offset PC; `decodeInstruction` per step; `instructionIndexAt`
  (binary search over `instrOffsets`) resolves line/col on the error path.
- **Codec v5 (v5-only):** `kQlcVersion = 5`; writer emits the per-function
  code blob + `instrOffsets`; reader reads the blob + offsets and validates
  (loc size == offset count, strictly increasing offsets, every instruction
  decodes, last instruction ends exactly at `code.size()`). **v1–v4 files are
  rejected** (the legacy fixed-width read path is removed).
- **Disassembler + dump:** `examples/qlang_run.cpp` `disassemble()` iterates
  `instrOffsets` and decodes; `examples/hello.cpp` `dumpBytecode` updated.
- **Tests:** `codec_check` rewritten for v5 (version assertion 5, v4 now
  rejected, blob/offset round-trip). Step 7 stress tests landed:
  `tests/unit/encoding_test.cpp` (41 checks: base/escaped round-trips for
  MOV/MOVI/JMP/ADD, truncated-buffer and reserved-modifier rejection, size
  helpers) + `tests/scripts/ext_regs.ql` (258 locals, 2-byte register
  encoding), `tests/scripts/ext_consts.ql` (258 string constants, 2-byte
  constant encoding), `tests/scripts/jump_widths.ql` (jmp8/jmp16/jmp32 all
  exercised). Full suite green (41/41 ctest; 795 counted unit cases).
- **Docs:** `docs/SERIALIZATION.md` rewritten to v5 (v5-only reader);
  `docs/spec/13-bytecodes.md` gains a "constant pool" (unique values, dedup)
  + "bytecode serialization" (→ SERIALIZATION.md) section.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 36 (2026-11-03)

**Instruction format Phase 1 — line/col side-table (12 → 8 B)** (plan:
`docs/plans/instruction-format-optimization-plan.md` §Phase 1; uncommitted).
No language changes, no new opcodes. The `.qlc` format bumps 3 → 4.
- **`Instruction` shrinks 12 → 8 B:** `line`/`col` drop out of the hot-path
  struct (`static_assert(sizeof(Instruction) == 8)`). The VM decode loop now
  reads 8 B/instruction (32 per 256 B L1 line, was 21).
- **`Function::loc` side-table:** `std::vector<std::pair<uint16_t,uint16_t>>`
  parallel to `instructions`; the compiler stamps it in `emit()`/`emitImm()`
  (and the CALL-site override), and `buildFunction()`/`compileClass()`
  save/restore it with the code buffer. The VM's `vmThrowAt()` and the
  builtin/call throw sites resolve `loc[pc]` only on the error path.
- **`.qlc` v4:** per-function location section (`u32 locCount` + `locCount ×
  (u16 line, u16 col)`, `locCount <= instrCount`) after the instruction
  stream; instructions are 8 B on the wire (`u16 op, u16 a, i32`).
  `kQlcVersion = 4`; the reader accepts v1..v4.
- **`examples/qlang_run.cpp`:** `disassemble()` reads `f.loc[j]` (guarded)
  for the level-2 source-line interleave.
- **Tests:** `codec_check` raw-file builders gain the loc section; the
  unsupported-version case now uses v5; the version assertion is 4; the
  truncated-instruction case chops the i32 field. 85 checks, all green.
  Full suite green (37/37 ctest; 754 counted unit cases unchanged).
- **Docs:** `docs/SERIALIZATION.md` (header v4, §4 version 4 + loc side
  table, §5.1 `version <= 4`, §9 malformed suite v5, §10 status).
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).
  Phase 2 (8-bit opcode + 8-bit registers, 8 → 4 B) and Phase 3 (universal
  scratch registers 240–255) remain per the plan.

## Round 35 (2026-11-03)

**Phase 5 — paper cuts** (plan:
`docs/plans/architecture-remediation-plan.md` §5; uncommitted). No language
changes, no new opcodes, no `.qlc` format change. Closes F6, H5, F7 from the
plan's defect list — the final phase of the remediation plan.
- **5.1 Step budget on the context (F6):** the instruction counter moves from
  the per-VM `m_steps` (reset on every dispatch) to the context —
  `QLangContext::stepsUsed()` (cumulative), `budget()` (the live `maxSteps`
  cap), `step()` (increment, checked once in the single dispatch loop),
  `resetSteps()`. The VM no longer owns a counter, so the host can read
  `stepsUsed()` before/after a call to compute per-dispatch cost.
  `EVENT_HANDLERS.md` §8.1 rewritten to the context-owned story.
- **5.2 Threading contract (H5):** "one context, one thread" — the context
  binds the creating thread's `std::thread::id` and a debug-build `assert`
  fires on cross-thread use (compiles out under `NDEBUG`). Every public
  entry point calls `bindThread()`; `enqueueFinalizer()` is exempt (the
  deleter may fire on any thread). Documented in `docs/spec/07-api.md`
  (new "threading contract" section). No threading model built.
- **5.3 CMake install/export (F7):** `install(TARGETS qlang EXPORT
  qlangTargets)` + headers + `qlangTargets.cmake` (namespace `qlang::`) +
  `qlangConfig.cmake` / `qlangConfigVersion.cmake` (hand-written version
  file — CMake 4.4.3 lacks `write_basic_package_config_file`);
  `target_include_directories` uses `$<BUILD_INTERFACE:…>` /
  `$<INSTALL_INTERFACE:include>`. Embedders can now `find_package(qlang)`
  against an install tree (verified with a consumer project). `file(GLOB)`
  for the `.ql` regressions replaced with an explicit 16-entry list.
- **Tests:** new `tests/unit/phase5_api.cpp` (17 checks, S1–S7: fresh
  `stepsUsed()==0`, accumulation across dispatches (no reset), per-dispatch
  cost via `call()` > 0 and stable, `budget()` == `maxSteps()` (default
  100000, tracks `setMaxSteps`), `resetSteps()` zeroes, runaway loop →
  `Runtime` "step limit", `set_max_steps()` from QLang sets `budget()`).
  ctest 36 → 37 entries; counted unit cases 737 → 754. Full suite green
  (37/37).
- **Docs:** `docs/spec/07-api.md` gains "step budget" + "threading contract"
  sections (new hash `f7c9932f…`); `docs/EVENT_HANDLERS.md` §8.1.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 34 (2026-11-03)

**Phase 4 — `.qlc` format + compiler hygiene** (plan:
`docs/plans/architecture-remediation-plan.md` §4; uncommitted). No language
changes, no new opcodes, no value-tag changes. The one and only `.qlc` format
version bump in the plan lands here (2 → 3). Closes F1–F5 from the plan's
defect list.
- **4.1 Explicit on-wire encoding (F1):** writer and reader now emit/read
  every multi-byte field in explicit little-endian order, byte by byte — no
  struct `memcpy`, no host-endian `putU32`. Instructions are written field by
  field (`u16 op, u16 a, i32, u16 line, u16 col`); the raw 12-byte struct is
  no longer the on-disk encoding. The version is written and validated from a
  single shared `constexpr kQlcVersion = 3` (`QLangBytecodeIO.h`); the reader
  accepts v1..v3 (legacy files are byte-identical on little-endian hosts) and
  rejects anything newer. Non-durable types (Generator/Object/External/
  BoundMethod/TypeRef): `writeQlc` rejects a program whose constant pool holds
  one (returns `false`), and the reader rejects a file carrying tags
  15/16/20/21/22 with a clear `Io` error (never a silent Nil).
- **4.2 Compiler statement split (F2):** the 1,456-line `compileStmt` switch
  is split into one private method per `StmtKind` arm, with a dispatch switch
  at the top. Mechanical; no behavior change.
- **4.3 Remove auto-capture (F3):** the name-scanning free-variable
  augmentation is deleted — the parser's explicit capture list is the only
  capture mechanism. `global` resolves by name against the global table
  (Phase 2), killing the register-0 fallback. Compile-time tightening: scripts
  that relied on implicit capture now fail to compile (none did — the parser
  already required the explicit list).
- **4.4 Parser depth limit (F4):** a depth counter in the recursive-descent
  entry points (`parseStatement`/`parseExpression`); past `kMaxParseDepth =
  256` the parser throws a clean `QLangError(Parse, "nesting too deep")`
  instead of overflowing the C stack. RAII guard decrements on all paths.
- **4.5 Constant pool (F5):** the string-key dedup (which serialized every
  value to a key and roughly doubled pool memory) is dropped — `addConstant`
  now appends and returns the pool index per literal.
- **Tests:** `codec_check` updated for v3 (version assertion 2 → 3, the
  unsupported-version case now uses v4, the `Bin` test-file builder emits
  explicit little-endian). 85 checks, all green. Full suite green (36/36
  ctest; 737 counted unit cases unchanged).
- **Docs:** `docs/SERIALIZATION.md` (header v3, §3 R2 program-vs-value
  rejection, §3 R4 explicit load-time capture-rebinding rule, §4 version 3 +
  field-by-field instruction encoding, §5.1 `version <= 3`, §9 malformed
  suite v4, §10 status). `harness.md` codec line updated to v3.

## Round 33 (2026-11-03)

**Phase 3 — host API hardening** (plan:
`docs/plans/architecture-remediation-plan.md` §3; uncommitted). No language
changes, no new opcodes, no `.qlc` format change. Closes H1–H4 from the plan's
defect list.
- **3.1 Stable type handles + `type_info` validation (H2, H4):**
  `registerType`/`import<T>()` now return `std::shared_ptr<TypeDescriptor>`
  (was a raw `const TypeDescriptor*` invalidated by `clear()`). The Pimpl
  `TypeRegistry` drops only its own reference, so a handle the host legitimately
  holds — including a copied `NativeFunc` ctor lambda — stays valid across
  `clear()`. `ExternalRef` gains `const std::type_info* expected` + a `check`
  predicate; `self()`/`selfMut()` validate the resolved pointer and throw a
  clean `QLangError(Type, "external type mismatch: …")` on a mismatch
  (EXTERNAL_TYPES §10.2's "a wrong `weak_ptr` is UB" is now a runtime error).
  An expired borrow throws `QLangError(Runtime, "object destroyed: …")`.
- **3.2 Finalizer queue (H3):** the instance table's `shared_ptr` custom
  deleter no longer dispatches script — it only enqueues `(table, closure,
  program)` on the context's finalizer queue (`enqueueFinalizer`, thread-safe,
  never throws, so it is safe from a foreign thread or during unwinding).
  `processFinalizers()` drains at safe points — the start of `run()`/`call()`/
  `callClosure()`, inside `clear()`, or explicitly from the host — to a
  fixpoint (a finalizer that releases more instances runs them in the same
  call), swallows finalizer errors, and is guarded against re-entrancy
  (`m_drainingFinalizers`) and against a drain during context destruction
  (`m_destroying`).
- **3.3 `release()` builtin (H2 retention leak):** `Builtin::Release`
  (`releaseExternal`) drops the `owned` shared_ptr of an owned `External`,
  letting scripts free script-constructed objects; the value becomes `Nil`
  (use-after-release is a clean nil error). A borrowed external (host-owned) or
  a non-external is a type error. The argument is mutated in place (special-
  cased in `CALL`; the compiler writes the slot back to the source variable so
  the original reference is dropped).
- **3.4 Context split (H1):** `QLangContext`'s Pimpl now holds two small
  classes — `ProgramCache` (the compiled-program cache, cleared by `clear()`
  only after the global table) and `TypeRegistry` (the type handles). The
  public header surface stays source-compatible for existing embedders.
- **Tests:** new `tests/unit/phase3_api.cpp` (23 checks: T1 handle survives
  `clear()`, T2 type-mismatch clean error, T3 expired-borrow clean error,
  F1–F7 finalizer queue — not-from-deleter / drain-at-run / fixpoint /
  error-swallow / clear-drain / foreign-thread-enqueue / re-entrancy-guard,
  R1–R4 `release()` — owned freed / borrowed rejected / non-external rejected /
  use-after-release, C1 copied `NativeFunc` survives `clear()`). ctest 32 → 36
  entries; counted unit cases 696 → 737. Full suite green (36/36; `perf`
  skipped on this non-Release build).
- **Docs:** `docs/EXTERNAL_TYPES.md` §3/§5/§6/§10.2/§11 (shared handles,
  `type_info` validation, `release()`, UB → clean error) and
  `docs/EVENT_HANDLERS.md` §0/§8.2 (finalizer drain point) updated.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 32 (2026-11-02)

Test migration + compliance docs (plan: `docs/plans/test-migration-plan.md`;
Phases 0–5 landed in `cb853be` / `5b202ae` / `8f20139` / `0036dec` / `3e6e8b8` /
`1577bf7`). No language changes, no new opcodes.
- **Test suite relocated to `tests/`:** 15 unit harnesses in `tests/unit/`
  (incl. `test_native` / `test_call` from the orphan-file triage), 16 `.ql`
  regressions in `tests/scripts/`, perf harness + immutable baseline in
  `tests/perf/`; `examples/` holds demos only (`hello.cpp`, `qlang_run.cpp`).
- **ctest: 6 → 32 entries** — 15 unit + 16 script (`script_*`, driven by
  `qlang_run`) + 1 `perf`; all green (re-verified 2026-11-02). **696 counted
  unit cases**: 463 `spec_check2` (+42 scope/boolean/trace coverage, Phase 5),
  33 `spec_check`, 20 `spec_cast_test`, 7 `test_module`, 5 `gen_test`, 22
  `range`, 13 `class_str`, 17 `class_static`, 23 `external`, 8
  `test_dollar_operator`, 85 `codec`; plus `ALL PASS` (`test_native`,
  `test_call`, `test_inject`).
- **Perf baseline (spec `15-tests.md` "performance tests"):**
  `tests/perf/perf_bench.cpp` — median-of-5 per parse/compile/execute stage over
  `hello.ql`, `money.ql`, `cast.ql`, `perf_stress.ql` (one unmeasured warm-up
  pass per script) — compared against the committed
  `tests/perf/baseline.json` (±20% `QL_PERF_TOLERANCE` + 0.5 ms `QL_PERF_EPS_MS`
  floor; `--update-baseline` for deliberate regeneration). The gate runs in the
  gxx CI job (g++/Linux Release calibration); non-Release builds → exit 77 →
  ctest Skipped; clang/MSVC jobs exclude it (`ctest -E perf`).
- **CI matrix (spec `15-tests.md` "automatic testing" — "three separate
  ci/cd yaml"):** `ci.yml` (g++: full ctest incl. `perf` gate + `perf-result`
  artifact; clang++: ctest `-E perf`; mingw-w64 cross: build-only),
  `ci-msvc.yml` (MSVC x64 / `windows-latest`: ctest `-E perf` + compiler-version
  step feeding `docs/COMPILER_PORTABILITY.md`), `ci-wasm.yml` (emscripten:
  `qlang` + `qlang_example` with `-sALLOW_MEMORY_GROWTH -fexceptions`, smoke via
  `node build/qlang_example.js`). Push-to-`main` triggers.
- **Compliance:** new `docs/compliance/tests.md` module (validates
  `15-tests.md`); spec hashes re-verified — `13-bytecodes.md` → `1eac6a97…`,
  `14-scope.md` → `ec1eadb2…`, `15-tests.md` → `dd9c8fe3…` (stale since the
  Round 31 spec corrections and the 15-tests CI-yaml amendment); status/harness
  paths refreshed to `tests/unit/`.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 30 (2026-10-29)

**MOVI** — 32-bit `Int` literal immediate; inlines the `CONST poolIdx + mov`
pair for integer literals (plan step 4.4, `docs/plans/qlc-codec-plan.md`).
- `Opcode::MOVI` appended **last** in the `Opcode` enum (existing `.qlc`
  opcode numbers unchanged); `usesI32()` covers it. Layout: `a` = destination
  register, `i32` = 32-bit `Int` literal — `r[a] = Int(i32)`, `Type::Int`
  only.
- Compiler: `local`/plain-assignment inlined into the target register (saves
  the temp + `mov`); expression context keeps the temp + `MOV`.
- `.qlc` writer emits format version 2 (readers accept ≤ 2).
- VM: dispatched in all four exec loops. `codec_check` passes.
- **Step 4.4 (2026-10-30, `58a9659`):** compiler-generated `Type::Int`
  constants (for-loop unit/zero/direction counters, for-in index init, class
  `__static_const__`/`__const_fields__` markers, per-field `__fields__`
  indices, `ab[]` append index) also compile to `MOVI` — the constant pool
  holds no `Type::Int` entries at all; instruction count unchanged (1:1).

## Round 31 (2026-10-29)

**ADDI + SUBI (Part 1) and alternate conditional jump — CMP/CMPI +
je/jne/jg/jge/jl/jle (Part 2).**
- Part 1: `Opcode::ADDI` (`r[a] = r[a] + Int(i32)`, in-place, `Type::Int`
  only) and `Opcode::SUBI` (`r[a] = r[a] - Int(i32)`) appended after `MOVI`;
  `usesI32()` covers both; VM dispatch in all four exec loops; compiler emits
  them for integer-literal `+=`/`-=`.
- Part 2: flag-based conditional jump. `Opcode::CMP` (flags =
  `cmp(r[a], r[b])`) and `Opcode::CMPI` (flags = `cmp(r[a], Int(i32))`) set
  independent GT/EQ/LT bits (mutually exclusive per comparison); `JE`/`JNE`/
  `JG`/`JGE`/`JL`/`JLE` jump on the flags (`JE` = EQ, `JNE` = EQ not set,
  `JG` = GT, `JGE` = GT|EQ, `JL` = LT, `JLE` = LT|EQ), pc target in `i32`.
  The spec flag/jump tables in `docs/spec/13-bytecodes.md` were corrected to
  the coherent independent-bits model, and the operand map gained rows for
  SUBI/CMP/CMPI/J*.
- `codec_check` covers the new MOVI + ADDI + SUBI + CMP/j* suites; all
  suites green (421 `spec_check2`, ctest 5/5).

## Round 16 (2026-10-27)

Spec drift audit: SPEC.md moved `1edc77ac` (41751 B / 1485 lines) → `c7f30b2a` (42276 B / 1502 lines) → `658c5631` (42315 B / 1503 lines, staged refinement mid-audit).
- **Round 15 work committed** in `72e2711` (implicit `__str__` collection rendering + tests + compliance), **plus two SPEC.md changes**: the `# implicit __str__ rules` heading rename and the **new `## operators < and >` section** (11-row type-pair table), later refined (staged, uncommitted): `array|array` per-index added, `table|table` = "comparison of each key value", `table|array` invalid, `class|any` ("bytecode cannot be compared").
- **`## operators < and >` implemented:** `cmp()` gains an optional `QLangVM*` parameter — (1) **string vs table/class/array** compares the string against `displayString(object)` (i.e. `__str__()` / class name / implicit collection rendering), either order; (2) **array vs array** compares each value at the relative index — first differing value decides, shorter array is less; (3) **table vs table** compares each key value in key order — same first-difference/shorter-is-less rule. Class instances (tables carrying `__class__`) and table|array fall through to the throw (spec: table|class, table|array, class|any → invalid). `doBinaryVm` routes LT/GT/LE/GE through the vm-aware `cmp` (same pattern as Round 13's EQ/NE). All other pairs keep the existing behavior: number|string, number|object, nil|any → `type error: operator '<' cannot compare X and Y`. The `in` opcode's `cmp` call is unchanged (no vm → legacy behavior).
- 20 new `spec_check2` cases (262 → 282): 13 valid (number/float/string/string-object/string-class/string-array/table-per-index/table-prefix/table-le/array-per-index ×2/array-prefix/array-ge) + 7 invalid (number|string, number|object, nil|any, table|class, class|class, table|array, class|array).
- Harness re-run: **282/282** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **399 total**.
- `# error reporting` remains **not implemented**.

## Round 17 (2026-10-27)

Impl fixes from the `examples/scripts/cmp.ql` bug report; SPEC.md unchanged (hash `658c5631` still current).
- **Boolean-producing operators now return `Bool`, not `Int(1/0)`:** `EQ` `NE` `LT` `GT` `LE` `GE` `IN` `LAND` `LOR` in `doBinary` + `doBinaryVm`, and `NOT` in `doUnary`. So `type(1 < 2)` → `"bool"`, `f"{1 < 2}"` → `"true"`; truthiness, `isEqual`, and arithmetic on the results are unchanged (`isTrue`/`isNumeric` already handle `Bool`).
- **Unknown function calls are now a compile error:** `printl("x")` previously emitted `CALL b=-1`, which the VM mis-dispatched as native func `idx = -1 - (-1) = 0` — silently calling the first auto-registered builtin-module function and returning nil (no error, no output). The compiler now throws `parse error at line N,C: unknown function 'name'` when the callee is not a user `def`, closure var, generator var, builtin, or native/module function. The parser's general `name(args)` path now also records `callLine`/`callCol` (previously only the builtin special-cases did), so the error carries a real position — a small step toward `# error reporting`.
- 9 new `spec_check2` cases (282 → 291): `type()` of `<` `>` `==` `&&` `!` `in` → `"bool"`, f-string bool rendering, bool arithmetic (`1 + (1 < 2)` → 2), unknown-function throw.
- Harness re-run: **291/291** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **401 total**; `helpers.ql` + `cmp.ql` (minus its deliberate error line) clean.
- `# error reporting` remains **not implemented**.

## Round 18 (2026-10-27)

Spec drift audit: SPEC.md moved `658c5631` (42315 B / 1503 lines) → `1b777ec6` (42557 B / 1511 lines). No code changes.
- **Committed since last audit:** `b1e6743` (Round 17 Bool-operators/unknown-function fix + the staged `## operators < and >` table refinement + `|||` whitespace fix — the "staged, uncommitted" refinement from Round 16 is now in HEAD).
- **New uncommitted SPEC.md edits (2):** (1) reserved-word list cleanup `range => keyword` → `range` (staged; `range` is already reserved in the lexer — documentation-only, no behavior change); (2) two new numeric-literal examples in `## declarations and assignments`: `local my_float = 1.0f` ("floats use a special notation") and `local my_uint = 44u` ("unsigned int").
- **Probed:** both suffix forms are **NOT implemented** — `QLangLexer::number()` has no suffix handling; `1.0f` → `parse error at line 0,0: use of undeclared variable 'f'`; `44u` → `use of undeclared variable 'u'`. New Open Item + Not-Yet-Implemented row added.
- Harness re-run (no code changes): **291/291** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **408 total** (the prior "401 total" was an arithmetic slip — the components sum to 408).
- `# error reporting` remains **not implemented**.

## Round 19 (2026-10-27)

Numeric literal suffixes `1.0f` / `44u` implemented (SPEC §declarations and assignments examples from the Round 18 drift; SPEC.md unchanged, hash `1b777ec6` still current).
- **Lexer** (`QLangLexer::number()`): after the digit loop, a single directly-attached suffix is consumed — `f` forces `TokenType::Float` (`1.0f`, and `123f` → `Float 123.0`), `u` forces the new `TokenType::UInt` (`44u`); the suffix is stripped from the token text so `123 f` / `123 u` (whitespace) remain two tokens. Hex path untouched (no hex suffixes in the spec).
- **Parser** (`QLangParser::parsePrimary()`): new `case TokenType::UInt` → `QLangValue::Unsigned(stoull(value))`; the bare-literal statement check now includes `UInt`.
- `TokenType::UInt` added to the enum after `Float` (outside the `TypeByte..TypeTable` 1:1 range — no mapping impact).
- 4 new `spec_check2` cases (291 → 295): `type(1.0f)` → `"float"`, `type(44u)` → `"unsigned"`, `44u` value 44, float-suffix arithmetic.
- Harness re-run: **295/295** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **412 total**.
- `# error reporting` remains **not implemented**.

## Round 29 (2026-10-28)

Phase 4 of `docs/action_plan.md`: **`# error reporting`** (SPEC section
committed in `649be35`). QLang-side only — `$$` and the C++ API / external
data stay out of scope (as does the warning infrastructure). SPEC.md gained a
`## format` subsection documenting the report shape (new hash `7b2e6604…`,
1712 lines / 48172 bytes — the diff also includes the earlier uncommitted
enum-spec lines).
- **pc → line/col table (compiler):** `Stmt` gains `line`/`col` (1-based,
  first token of the statement); `QLangParser::parseStatement()` wraps
  `parseStatementImpl()` to stamp them. `QLangCompiler` tracks `m_curLine`/
  `m_curCol` (set at the top of `compileStmt`) and `emit()` writes them into
  every `Instruction.line/col` (the fields already existed on `Instruction`).
- **`QLangError` (header rewrite):** gains `module` (string) + `frames`
  (`std::vector<QLangFrameRef{name, line}>`, innermost first) + `message()`
  (raw message accessor); `formatReport()` renders
  `module 'main' line L, col C in function 'fn': <kind>: <message>` + one
  `at line N in <fn>` per frame (missing pieces omitted); `report()` rethrows
  with `what()` = the full report (kind/line/col/module/frames preserved).
  Plain errors keep the old `<kind> at line L,C: <msg>` `what()`.
- **VM throw sites:** new `QLangVM::vmThrow(kind, msg)` /
  `vmThrowAt(kind, line, col, msg)` — walk `m_frames` innermost-first,
  resolve each frame's pc → instruction line, take the current pc's line as
  the error line, attach `m_scope.moduleName()` + the frame chain. All
  ~97 member-function throw sites now go through them. Free helpers
  (`num`, `cmp`, `callBuiltin`, `strict_convert`, `doBinary`, `doUnary`,
  `toSetElements`, `appendValue`, `lenOf`, `seqGet`) gained an optional
  `QLangVM *vm` parameter threaded from their call sites and route through a
  `vThrow(vm, …)` shim (null vm → plain 0,0 error, as before). `callBuiltin`
  passes its existing caller line/col through.
- **Top-level catch:** `QLangContext::run()` wraps the pipeline in try/catch
  and rethrows `QLangError::report(e)` — callers (and the harness) see the
  full formatted message. `QLangContext::moduleName()` = `"main"`.
- 6 new `spec_check2` cases via an `expectMsg(name, src, …substrs)` helper:
  lex-error line, parse-error line (line 3 of 3), runtime type error
  (module + line 3 + kind), builtin arity error (line + `main` frame),
  named-function error (`in function 'foo'` + trace), deep call chain
  (`a→b→c` division by zero: `at line 2 in c`, `at line 5 in b`,
  `at line 8 in a`, `at line 10 in main` — innermost first).
- Harness re-run: **369/369** (`spec_check2`), **33** (`spec_check`), **20**
  (`spec_cast_test`), **7** (`test_module`), **22** (`range`), **13**
  (`class_str`), **17** (`class_static`), all green = **481 total**. ctest 4/4.
- Known limits: `QLangValue` accessor throws (e.g. `asInt()` on a string,
  `QLangValue.cpp`) and `QLangObjects.cpp` object-dispatch throws still pass
  0,0 (no VM context at those call sites — external-type paths are out of
  scope this phase); they still get the module name via the `run()` catch
  when thrown through it. Trace lines use the function name only (the spec's
  "module/class/function/table" per node is satisfied for the common case —
  QLang frames are functions; class/table context is not separately
  attributed).

## Round 28 (2026-10-28)

Phase 3 of `docs/action_plan.md`: the `enum` type (SPEC §enum). SPEC.md at
`a350a25c` (unchanged this round — the `## enum` section was staged earlier).
- **Lexer**: `TokenType::Enum` + `enum` in `keywordType()` (reserved word).
- **Parser**: `parseEnum()` — `enum Name[:numeric-type] { A, B, C = 100, D }`;
  optional `:type` restricted to the 8 numeric types; member values must be
  integer literals; empty body → parse error; `local`/`let` prefix supported.
- **Compiler**: enum members are compile-time constants. Values computed at
  codegen (implied sequential from 0; `= N` resumes at N+1) and typed per the
  enum's base type. Storage: an immutable table (`name → value`) in a hidden
  variable `__enum_<Name>` (class-prototype pattern) — so `len(Enum)` works
  via the table path and `type(Enum::M)` reports the member's type. Member
  access `E::M` and `E.M` folds to `CONST <value>`; unknown enum/member →
  compile error. Redeclaration (visible same-name enum, or a global enum
  inside a function) → compile error. `local`/`let` → block scope (hidden var
  in the block register map); global enums captured into function bodies via
  the free-variable mechanism (hidden var captured, like global consts).
- 16 new `spec_check2` cases (347 → 363): implied values, `= N` resume,
  `::` + `.` notation, typed `:byte` (value + `type()`), `len`, arithmetic,
  redeclaration / unknown enum / unknown member / valueless / non-integer
  value errors, block scope (visible inside, not outside), global-in-function,
  member-not-assignable. Harness: **363/363**; full tally **475**.
  ctest 4/4. Remaining spec gaps (not in this phase): the C++ enum-injection
  API is stated as "needed" but undefined; `$$` and external-data injection
  are Phase 4.

## Round 27 (2026-10-28)

SPEC.md advanced to `a350a25c` (1659 lines, 47151 bytes) — it **closed the
remaining set gaps**, so this round implements the rest of SPEC §sets:
- **`x in set(1,2,3)`** — membership, "similar to `x in [1,2,3]`". IN opcode
  gains a Set branch using `setContains` (the same exact-match element rule
  set dedup uses, so `in` is consistent with dedup and `==`). Works in `if`,
  `while`, and any boolean context.
- **`for x in set(1,2,3)`** — iteration, "similar to `for x in [1,2,3]`".
  `seqGet` gains a Set branch (yields elements in storage/insertion order;
  order is implementation-defined per spec).
- **`set == set`** — "only true if set is an exact match." `isEqual` gains a
  Set case: equal size + every element of A present in B. **Order-independent**
  (consistent with "value order is dependant on the implementation"); so
  `set(1,2,3) == set(3,1,2)` → true.
- **`cast<set>(array)`** / **`cast<array>(set)`** — new cast-table rows.
  Parser cast-type check + tmap gain `TypeSet`; `strict_convert` converts
  Set→Array (`v.asSet()`) and Array→Set (`toSetElements`, dedup). `cast<int>(set)`
  now rejected (Set added to the numeric-target reject list).
- 10 new `spec_check2` cases (337 → 347). Harness re-run: **347/347**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **459 total** (gen_test 5/5 separately).
  ctest 4/4. SPEC §sets is now **fully covered** — no set gaps remain.

## Round 26 (2026-10-28)

Phase 2 of `docs/action_plan.md`: the `set` type (SPEC §sets). SPEC.md at the
time was `fe8c72fe` (superseded by `a350a25c` in Round 27) — code + tests only.
- **Value type**: `QLangValue::Type::Set` appended **last** after `Long`
  (keeps the fixed `.qlc` serialization tags stable); stores an
  `ArrayValue` (the variant already had it — no variant change). Factory
  `Set(ArrayValue)`, accessors `asSet()` const/mut, `typeName()` → `"set"`.
- **Lexer/parser**: `TypeSet` token (`set` is now reserved — `local set = 1`
  is a parse error); `parsePrimary` special case treats `set(...)` as a
  `Call` with callee `"set"` (same pattern as `str()`/`int()`); `x:set`
  accepted in `local`/`const` typed decls (tmap entries).
- **VM**: `Builtin::Set` — `set(1,5,7)` variadic, `set([1,6,46,5])` array
  coerced; duplicates dropped (first-seen order kept); elements restricted
  to numbers and strings (else type error). `convert()` (CONV) coerces an
  array to a set for typed decls. `len(s)` = count; empty → false, non-empty
  → true; `__str__`/display → `set(1, 2, 3)` (strings quoted, per the SPEC
  example); `valueToString` → `"<set>"`. ARRAPP (the 4 duplicated dispatch
  sites) now routes arrays+sets through one `appendValue` helper, so
  `s[] = v` appends with dedup.
- **Not implemented (SPEC is silent — flagged, not invented):** `in`
  membership on sets, `==` between sets, `isset` on set elements, `for x in
  set` iteration, `cast<set>`.
- 12 new `spec_check2` cases (325 → 337). Harness re-run: **337/337**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **449 total** (gen_test 5/5 separately).
  ctest 4/4.

## Round 25 (2026-10-28)

Phase 1 of `docs/action_plan.md` (plan excludes `$$` and C++ API / external
data). SPEC.md unchanged (hash `56e3b092…` still current) — code + tests only.
- **Valueless `local` defaults to 0** (SPEC §declarations): the Declaration
  case emits `CONST Int(0)` instead of nil; a typed valueless decl
  (`local b:byte`) gets zero of that type via the existing CONV. The 4
  nil-equality cases (`eq/ne nil nil/x`) updated to declare `nil`
  explicitly — they had encoded the old nil default.
- **`fetch` reserved word**: `TokenType::Fetch` in the lexer enum +
  `keywordType()` — `local fetch = 1` → `parse error: expected variable name
  but found 'fetch'`. Behavior unspecified in SPEC; reserving is all it asks.
- **math `rand()`**: `mathRand` in `QLangModules.cpp` (`std::mt19937` +
  `uniform_int_distribution<int32_t>`, returns `Int`), registered in the math
  module.
- 6 new `spec_check2` cases (319 → 325). Harness re-run: **325/325**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **437 total** (gen_test 5/5 separately).
  ctest 4/4. Probe: `local z` → `type(z)` = "int", `z` = 0.
- **Duplicate-declaration warning REVERTED** (post-round, same day): the
  Round 25 warning (stderr at the Declaration site) was removed per user
  direction — the new `# warning` SPEC section requires warnings to be
  toggleable, so the warning is deferred to a later plan with the warning
  infrastructure (2 `spec_check2` cases removed; 327 → 325).

## Round 24 (2026-10-28)

SPEC.md one-line tweak (hash `3f1afe5b…` → `46cd4668…`, 1544 lines). Fixed the
`examples/scripts/cast.ql` bug report — four root causes in `src/QLangVM.cpp`:

1. `format()` (f-string interpolation) passed a `double` to a `%s` placeholder
   (`snprintf` UB) → Float args rendered empty/garbled. Now renders `%.15g`
   (same as `valueToString`) when the placeholder is `%s`.
2. `format()` numeric branch passed a `long long` to a `%d` placeholder —
   `%d` reads only 32 bits, so `cast<dword>(-1)` (4294967295) printed `-1`.
   Conversion is now `lld` (and explicit `:d`/`:i` specs are widened too).
3. `format()` had no Qword path — qword args went through `toInt64()`, which
   throws for values > INT64_MAX. New Qword branch uses `%llu`;
   `cast<qword>(-1)` now prints `18446744073709551615`.
4. `strict_convert` Float target went through `toInt64` → `cast<float>(1.24)`
   truncated to 1.0. Now `toDouble(v)` (float→float is identity).

Latent crash fixed: four VM debug-trace sites (`[DBG-RET]`, `[DBG-RES]`,
`[DBG-MAIN]` map dump, `[DBG-YIELD]` ×2) evaluated `toInt64()` in their
arguments — evaluated **before** `trace()` short-circuits — so any qword >
INT64_MAX in a traced register threw even with tracing **off**. The implicit
main-frame `RETURN` (retVal = regs[0]) hit this at end of every program whose
first variable held a big qword. Trace args now use non-throwing
`valueToString`.

`examples/scripts/cast.ql`: all 13 lines print the expected values; the
previously-commented qword block is uncommented and the stale `# bug:` lines
removed. 4 new `spec_check2` cases (f-string float, f-string unsigned > int32
max, f-string qword max, `cast<float>` identity).

- Harness re-run: **319/319** (`spec_check2`), **33** (`spec_check`), **20**
  (`spec_cast_test`), **7** (`test_module`), **22** (`range`), **13**
  (`class_str`), **17** (`class_static`), all green = **431 total**. ctest 4/4.
- Pre-existing, out of scope: a 2⁶⁴−1 **literal** (`18446744073709551615`)
  aborts in the lexer (`std::stoll` out_of_range) — only reachable via a
  literal, since `cast<qword>` now covers the value range.

## Round 23 (2026-10-28)

SPEC.md unchanged (hash `3f1afe5b…` from Round 22). Implemented the `long` type
(spec `## data types` row `|long|8bytes|int64_t|`):

- `QLangVariant` gains `int64_t`; `QLangValue::Type::Long` appended **last**
  (after `Eof`) so the fixed `.qlc` value tags (docs/SERIALIZATION.md R1) are
  unchanged — `Long` is tag 17, `i64` payload.
- Lexer: `long` → `TokenType::TypeLong` (outside the `TypeByte..TypeTable`
  1:1 range, like `TypeQword`). Parser: decl + cast `tmap` entries,
  type-token checks in `local`/`const`/`cast<…>`.
- VM: `isNumeric`, `valueToString`, `lenOf` (8), declaration `convert`,
  `strict_convert` (numeric tower). Modules: min/max/sum treat `Long` as
  integer. Context: `levelFromValue`, `setMaxStepsFunc`. Objects:
  `isNumericType` includes `"long"`. `.qlc` writer: tag + `u64` payload.
- 5 new `spec_check2` cases (typed decl with 64-bit literal, `type()` →
  `"long"`, `len` → 8, `const`, `cast<long>`). `examples/scripts/len.ql`
  now runs clean through `long is 8 byte`.
- Docs: README type table (`dword` alias + `long` rows), SERIALIZATION tag
  17, EXTERNAL_TYPES boundary table (`int64` → Long, `uint64` → Qword).
- Suites: `spec_check2` 315, `spec_check` 33, `spec_cast_test` 20,
  `test_module` 7, `range_check` 22, `class_str` 13, `class_static` 17 —
  all green; ctest 4/4.

## Round 22 (2026-10-28)

Spec wave: `dword` redefined in SPEC.md `## data types` as a **4-byte `uint32_t` alias of `unsigned`** — the original spec's 8-byte `dword` was an error; 8 bytes is `qword`. `qword` remains the `uint64_t` type.

- **Implementation (1 line):** `QLangLexer::keywordType` — `dword` now lexes as `TokenType::TypeUnsigned`, reusing the entire `unsigned` machinery: `local d:dword = 5` is an `Unsigned` declaration, `type(d)` → `"unsigned"`, `len(d)` → 4, `cast<dword>` → `Unsigned`. No parser/VM/value-type changes needed.
- **Tests:** 4 new `spec_check2` cases (dword alias typed to u32 max, `type()` → `"unsigned"`, `len()` → 4, const). `examples/scripts/len.ql` restored with a `dword` line → prints `unsigned is 4 byte`.
- **Tally:** spec_check2 310; total **427 passed, 0 failed** (all 8 suites green; `ctest` 4/4).
- **SPEC.md:** `3f1afe5b0556230e6349f8eb7ed37eb2920a2802ac4f2713bd3c839a004855d5` (1543 lines, 43928 bytes), uncommitted.
- **Still open:** `long` (int64_t) and `set` types; `set`/`long` reserved words (spec `## data types` lists them; not lexer keywords — `local a:long` → `unknown type after ':'`).

## Round 21 (2026-10-28)

Spec wave implemented: `dword` → `qword` rename, `len()` storage byte sizes, `delete` nil/external guards.

### Changes
- **`dword` → `qword` (spec errata applied):**
  - `QLangLexer.cpp` — keyword `qword` (still maps to internal `TokenType::TypeQword`).
  - `QLangValue.cpp` — `Type::Dword` type name is now `"qword"`.
  - `QValue.h` — public helper `qval_dword` → `qval_qword`.
  - `QLangObjects.cpp` — `isNumericType` accepts `"qword"`.
  - `SPEC.md` — errata note removed; data types table, reserved words, `$` operator rows, C++ API list, implementation notes all use `qword`.
  - `README.md` — type table row `dword` → `qword`.
  - Tests: `spec_check2.cpp` + `spec_cast_test.cpp` cases renamed to `qword`.
  - `dword` is now a free identifier (probe-verified). Lexer token renamed `TokenType::TypeDword` → `TokenType::TypeQword` (8 sites: header + `QLangLexer` + `QLangParser`); all internal value identifiers renamed to `Qword` (`QLangValue::Type::Qword`, `QLangValue::Qword()`, `asQword()`, `isQword()`).
- **`len()` storage byte sizes (`QLangVM::lenOf`):** char/byte 1, word/short 2, int/unsigned 4, qword/float 8, bool 1, class instance (table with `__class__`) 1, nil → throws `len: undefined/nil has no length`; string/array/blob element counts and plain-table size unchanged.
- **`delete` guards:**
  - `QLangParser::parseDelete` — `delete nil` → parse error `cannot delete nil`.
  - `QLangCompiler` — new `addExternalVars(std::set<std::string>)` + `m_externalVars`; `StmtKind::Delete` throws `cannot delete external variable '<name>'` for writable injected host vars, `delete of undeclared variable '<name>'` for everything else.
  - `QLangContext::run()` — passes `m_vars` minus `m_injectedConsts` (writable injected vars) to the compiler.
- **Tests:** 11 new `spec_check2` cases (9 `len` + 2 `delete`); `spec_check2` 295 → 306.

### Verification
- `cmake --build build` clean; `ctest` 4/4.
- Full tally: spec_check2 **306**, spec_check 33, spec_cast_test 20, test_module 7, gen_test 5, range_check 22, class_str 13, class_static 17 = **423 passed, 0 failed**.
- Probes: `dword` free identifier; `delete nil` → `cannot delete nil`; `delete` on `ctx.set()` var → `cannot delete external variable 'host_var'`; `delete` on `ctx.set(..., true)` const → `delete of undeclared variable 'host_c'`.

### SPEC.md
- SHA256: `6a0856c581ad415e0e1df8139f55ddbd8accd67f95260e05b6f71f45fd5b1fc7` (1539 lines, 43636 bytes).
- Still unimplemented from the 2026-10-28 wave: `set`/`long` reserved words; `long`/`set` types.

## Round 20 (2026-10-28)

Spec drift audit: SPEC.md moved `1b777ec6` (42557 B / 1511 lines) → `c5d69bc2` (43372 B / 1528 lines), staged uncommitted. HEAD `b564302` committed the Round 19 work (incl. the `1.0f`/`44u` SPEC examples + `range` reserved-word cleanup + `hello.ql` `printl`→`println` typo fixes).
- **New uncommitted SPEC.md wave:** `## data types` table (new rows `long`/`qword`/`set` + dword→qword errata note); `set`/`long` added to reserved words; `## strings` section moved up; declarations type-inference intro; **`len()` numbers → storage byte size**; **`delete` external/nil → error**; `sum` "coerce non-float to float" note.
- **Probed:** `len(123)` → 1 (spec: int = 4 bytes), `len(1.13)` → 1 (spec: float = 8) — **not implemented**; `local set = 1` / `local long = 1` compile — reserved words **not implemented**; `local x:long` / `x:qword` / `x:set` → `parse error: unknown type after ':'` — types **not implemented** (`dword` remains the working 64-bit type); `delete nil` → parse error (errors, wording "expected variable name"); `delete` on a `ctx.set()`-injected var → `parse error: delete of undeclared variable` (errors, wording differs).
- Harness re-run (no code changes): **295/295** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **412 total**.
- `# error reporting` remains **not implemented**.

## Round 15 (2026-10-27)

Spec drift audit: SPEC.md moved `967b17e5` (41449 B / 1471 lines) → `1edc77ac` (41751 B / 1485 lines).
- **Round 14 work committed** in `649be35` (bool literals/`isset` → `Bool`, f-string/bool snprintf segfault fix, triple-quoted string leak fix, `helpers.ql`, Round 14 compliance) — **plus the two new SPEC.md sections** `# implicit __str__` and `# error reporting`.
- **Uncommitted:** SPEC.md heading rename `# implicit __str__` → `# implicit __str__ rules` (content unchanged).
- **`# implicit __str__ rules` implemented:** `QLangVM::displayString` now renders arrays as `[e1, e2, ...]` and plain tables as `{k=v, ...}` via a new recursive `renderElement` (strings double-quoted, nested collections recurse, scalars in plain display form). All display contexts conform: print/println/printf %s/sprintf %s/f-string/join/`str()`. `str([0,1,2,3,4])` → `"[0, 1, 2, 3, 4]"`; `str({name="Bob", age=9})` → `"{age=9, name=\"Bob\"}"`; `f"{b}"` on array `b` → `[1, 2, 3]`. `valueToString` (the `+=` path) and `cast<string>` intentionally unchanged (the `+=` spec table says "any other types → string<\type>"). **Caveat:** table keys render lexicographic (`std::map`), not the spec example's insertion order — no ordering rule in the spec; flag if insertion order is required.
- 4 new `spec_check2` cases (258 → 262); 3 prior `<table>` expectations updated to `{a=1}` (`class_str_test` ×2, `spec_check2` ×1).
- Harness re-run: **262/262** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **379 total**.
- `# error reporting` remains **not implemented** (now committed in the spec).

## Round 14 (2026-10-27)

Spec drift audit: SPEC.md moved `c46efc2f` (41038 B / 1457 lines) → `967b17e5` (41449 B / 1471 lines).
- **Round 13 work committed** in `c62b8e4` ("string == object via `__str__` fix"): the `==`/`!=` example table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` (code), 6 `spec_check2` cases, CMake test targets, `moneyv2.ql`, Round 13 compliance text.
- **New uncommitted SPEC.md section `# error reporting`** (requirements + execution trace) — **not implemented**: no module name in errors, no class/function/table context, no execution trace, and 152/162 throw sites carry no line/col. See Open Items.
- Harness re-run (no code changes): **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- Working tree: `M SPEC.md` (error reporting section); untracked `docs/class_action_plan.md`.
- **Impl fixes** (from `examples/scripts/helpers.ql` bug report; no spec change, hash unchanged): (1) triple-quoted strings (`"""..."""` and `f"""..."""`) leaked the first two opening quotes into the content — `QLangLexer::stringLiteral` **and** `fstringLiteral` consumed only one opening quote in the triple case, now consume three. (2) `isset` is now a **reserved keyword** (`TokenType::Isset` in `keywordType()`); `parsePrimary()` compiles a bare-identifier arg to its name string, so `isset(xx)` on an undeclared variable probes the existing `Builtin::IsSet` runtime check instead of `parse error: use of undeclared variable`. (3) **booleans are now proper `Bool` values**: `true`/`false` literals compile to `QLangValue::Bool(1/0)` (was `Int(1/0)`) and `isset` returns `Bool` — so `type(true)` → `"bool"`, `f"{true}"` → `"true"`, `s += true` → `"...true"` (spec: `$` matrix + `+=` table); equality/arithmetic/truthiness unchanged (`isEqual`/`isTrue` already handle `Bool`). (4) **segfault fix**: `format` builtin's `Bool` branch passed a `long long` to `snprintf` with a `%s` placeholder (`f"...{b}..."` on a `:bool`-typed var crashed in libc); now `%s`-spec bools render as `"true"`/`"false"` text, numeric specs stay 1/0. Self-check: `helpers.ql` (`qlang_run`), full suite re-run green (375).

## Round 13 (2026-10-27)

`string == object` `__str__`-equality implemented (SPEC §operator == and != new example table); harness re-run: **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- SPEC.md moved `204f0702` (40335 B / 1443 lines) → `c46efc2f` (41038 B / 1457 lines): 12-row `==`/`!=` example table + "equivalency worths both ways" note + `(byte)0x255`→`(byte)0xFF` typo fix (uncommitted). Every row except `string==object` already conformed (probed against the lib: `1==1.0` true, `"3"==3` false, `' '==32` true, `-1==(byte)0xFF` false, `1==(unsigned)1` true).
- **`string == object`:** `QLangVM::valueEqual` (member) — String vs object (table with `__str__` or `__class__`) compares against `displayString`; everything else → free `isEqual`. New member `doBinaryVm` routes EQ/NE through `valueEqual` in all 4 exec loops; other opcodes still go to the static `doBinary`. No new opcodes. `"toto" == C()` → true when `__str__` returns `"toto"`, false otherwise; `1 == C()` stays false; plain-table equality unchanged.
- 6 new `spec_check2` cases (252 → 258).
- Committed since Round 12: `a6c339a` (static spec additions + plan docs), `6a40633` (static/`__str__` implementation + Round 12 compliance). Uncommitted: `==` table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` + 6 cases (code), CMake test targets, `docs/` renames, untracked `.draft/`/`.hermes/`/`docs/class_action_plan.md`.

## Round 12 (2026-10-27)

Class `static` + `__str__` display dispatch implemented (plan: `class static implementation.md`); **no new opcodes** (reuses TABGET/TABPUT/TABCREATE/CALLCLOS). Harness re-run: **252/252** (`spec_check2`), **13** (`class_str`), **17** (`class_static`), **22** (`range`), **7** (`test_module`), all green = **369 total**.
- **`__str__` display dispatch fixed** (Part 1): `print`/`println`/`printf %s`/`sprintf %s`/f-string/`join` now route through `QLangVM::displayString` (same `__str__` call + class-name fallback as `str()`), with a `m_inDisplayString` re-entrancy guard so an `__str__` that displays `self` terminates. `table_ops::ToStr` dispatch kept honest. 13 cases in `class_str_test` (ctest `class_str`).
- **`static` keyword + `::` operator** (Part 2): `TokenType::Static` in `keywordType()` (`local static = 1` → parse error); `TokenType::ScopeRes` for `::` (lexed before single `:`, `parsePostfix` handles it → `Expr.scopeStatic`). `parseClass` accepts `static` field / `static const` / `static method`; `static`+`local`/`let` and duplicate/static-instance name collisions are parse errors. f-string format-spec splitter skips `::` (so `{Money::banner}` is not split at the scope colon).
- **Static storage + codegen** (Part 3): statics live in a class-level `__statics__` table (shared `shared_ptr` map) embedded in the prototype and copied **by reference** onto every instance. The prototype is created once at class definition into a hidden `__proto_<Name>` variable (shared by all instances + `::` access). Static methods are compiled with a hidden first parameter (the `__statics__` table) so they need no self; bare static names in method bodies resolve via `self.__statics__` (instance) or the anchor (static). `C::name` → two-hop TABGET; `C::name(args)` → TABGET closure + pass the statics table as anchor. `ClassRecord` gained `staticFields`/`staticConstFields`/`staticMethodFuncIndices`/`staticMethodArities`.
- **Instance-level static dispatch** (Part 4): `table_ops::getField`/`setField`/`callMethod` fall back to `__statics__` for names not in the instance field schema (so `money.banner` / `money.banner = "Turtle"` work and mutate shared state); `setField` rejects `static const` writes. No `ObjectOps` signature change (`callMethod` re-derives from-statics from the table).
- **Deviations from the plan:** (a) the plan's "hidden capture of `__statics__`" was replaced by a **hidden anchor parameter** for static methods — a capture by frame-register reference dangles once the prototype frame is popped (`deque::pop_back` destroys it), so the anchor (a shared `shared_ptr` passed as an arg) is lifetime-safe; (b) instance-call-of-a-static-method (`money.getBanner()`) is **not** routed to pass the anchor (the compiler's `obj.method()` path emits TABGET+CALLCLOS, not the VM `callMethod`); spec is silent and it is not in the required test set — left unsupported. See plan "Risks & Ceilings".
- 17 cases in `class_static_test` (ctest `class_static`): static field via class/instance, instance-write→class, cross-instance sharing, static method via class, arity error, f-string static (class+instance), `__str__` referencing a static, `static const` read (class+instance) + write error, `static local`/`static let`/`local static` parse errors, `C::missing` error, closed-field error intact.

## Round 11 (2026-10-27)

Spec drift audit (three waves same day): SPEC.md moved `efd53057` (38991 B / 1384 lines) → `30580ccf` (39692 B / 1426 lines) → `65df6095` (40105 B / 1437 lines) → `204f0702` (40335 B / 1443 lines); working tree dirty (uncommitted spec edits).
- Wave 1: `static` reserved word; Money example gains `static banner` + `static getBanner()`/`static setBanner()` + `Money::setBanner(...)` usage + `__str__` interpolating `{banner}`; new `## static` section.
- Wave 2: `::` row added to the operator table ("scope resolution operator"); Money example extended — static **field** read via class name (`f"{Money::banner}"`), static field read via **instance** (`f"{money.banner}"`), and instance write `money.banner = "Turtle"` mutates class-level storage (`Money::banner` then reads "Turtle").
- Wave 3: `## static` gains `static`+`const` allowed / `static`+`local`/`let` forbidden, plus an explicit note that `static`/`::` is a **new extension beyond v1** — added per user direction (v1 is implemented and stands; the `## v1 restrictions` contradiction is resolved by that note, no restriction line changed).
- Probed: `static` is **not** in `keywordType()` — tokenizes as `Ident`; no `::` token/parser path; no parser/VM support for static members or `ClassName::` calls. **Not implemented.**
- Harness re-run after each wave (no code changes): **252/252** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range_check`) = **339 total**, all green.
- Open: `static` + `::` (new), duplicate-declaration warning (unchanged), `$$` (unchanged), `fetch` (unchanged).

## Round 10 (2026-10-26)

Block scope implemented (SPEC `# block scope`, committed `dde51e4`); harness re-run: **252/252** (`spec_check2`), all suites green (339 total).
- **Block scope: full conformance.** `QLangCompiler` gained `m_blockScopes` (stack of `map<name,reg>`) + `m_blockDepth`; `declareVar` pushes into the innermost block scope when depth > 0, else into `m_regs` (functions unchanged — closures/generators/mutation semantics intact). `regOf` consults the scope stack before `m_regs`; `undeclaredReg` consults it before treating a name as undeclared (plain-assignment crossing preserved). Scope pushed/popped at: `if`/`else` bodies, `for` body (loop var declared in enclosing scope per spec), `for-in` body (both range and array sub-paths — the range sub-path has an early case-exit that also pops), `while` body, and each `switch` case body + default body. 6 tests at spec_check2 L633–L640 (if isolation, nested shadow, else branch, while body, for body + loop-var visibility after loop, for-in shadow).
- No new opcodes; zero VM changes.

## Round 9 (2026-10-26)

Spec drift audit: SPEC.md moved `44e9fd2d` (38593 B / 1357 lines) → `600f11a8` (38921 B / 1382 lines), working tree dirty.
- `052b45e` (committed): `$` nil row `"false"` → `"nil"` (previously uncommitted) + compliance/handoff doc cleanup, stale docs deleted. No code changes; harness unaffected (333 total, last green run at HEAD `0c63f4b`).
- Uncommitted SPEC edits (3): duplicate-declaration error → **warning** (one example line self-contradictory); `## operator $$` "no block scope" clause removed; new `# block scope` section.
- Probed: dup-decl forms compile with no error and no warning; `local i = 3` in a block overwrites the outer `i` (`$i` → "3", spec expects "1") — **block scope not implemented**. Plain-assignment crossing works.
- Open: block scope (new section), duplicate-declaration warning (spec now asks for a warning, not an error), `$$` (unchanged), `fetch` (unchanged).

## Round 8 (2026-10-26)

Class conformance completed and the `$` operator implemented; harness re-run at HEAD `0c63f4b`: **246/246** (`spec_check2`), all suites green (333 total).
- **User-defined classes: full conformance.** Phases 3–7 landed this round: `6a4c559` (unique method/dunder names — compile error), `8c013a1` (fixed arity — constructor + method call checks), `b242645` (`__str__`), `0d7115b` (reference counting + `__destroy__` finalization). 42 class tests at spec_check2 L449–L615. Deviations from the plan documented in `class_action_plan.md` (Phase 0.2 no separate `Object` value type; Phase 9.2 `$` rejection on instances is runtime, not compile).
- **`$` dynamic-key operator implemented** (`0c63f4b`): `Dollar` token + `OP_EVALKEY` opcode; the commit fixed two bugs — EVALKEY was emitted but never dispatched (added to all 4 exec loops) and table-literal `$` keys degenerated to bare identifiers (parser now wraps the operand in a Unary node). 6 tests at spec_check2 L617–L626.
- **SPEC.md:** `$` section nil row corrected `"false"` → `"nil"` (matches implementation; **uncommitted** in working tree).
- **Re-verified gap:** duplicate-declaration compile check is still **not implemented** (`t = 5; local t = 9` compiles silently, prints `9`).
- `$$` indirect access: still **not implemented** (unchanged).

## Round 7 (2026-10-26)

Spec diff audit vs `21b1ce65` (1357 lines; `c83ebc8` + `e08005b` over `9cc0a3f`); harness re-run at HEAD `e08005b`: **204/204** (`spec_check2`), all suites green.
- `## operator $$` committed in full → tracked as **not implemented** (no `$$` token, no `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, no declaration-state tracking, no harness cases).
- `## user defined classes` expanded → the existing **prototype is non-conforming** (closed field set, `const` fields, unique-name/arity compile checks, `__str__`, reference counting, `__destroy__`, `type(x)` all missing). Conformance baseline: `docs/EXTERNAL_TYPES.md` §9 — the `ObjectOps` dispatch layer must land first (classes are its first consumer).
- **New gap:** duplicate-declaration compile check is **not implemented** (spec examples committed in `9cc0a3f`; verified 2026-10-26 — duplicates silently compile, initializer becomes a plain assignment).
- `global c` unknown-binding example: ✓ implemented (compile error; message wording differs).
- `import`/`from` reserved words: ✓ implemented in the lexer (round 2).
- Class example `spend` fix (`+=` → `-=`): no harness case covered the example; no test change needed.

