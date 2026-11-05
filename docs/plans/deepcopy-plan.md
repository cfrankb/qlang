# Plan: `deepcopy` builtin + default-share value semantics

Adopted 2026-11-01. Syntax decision: **Option A — a `deepcopy(x)` builtin,
mirroring Python `copy.deepcopy`.** Companion rule: **deep copy of
non-primitive values is always explicit**; the default for every non-primitive
is **share / pass-by-pointer**.

Spec anchors (all landed **2026-11-05**, marked *Proposed* until Phase 1 runs):

| Anchor | Content |
|---|---|
| `docs/spec/01-types.md` `## value semantics` (:41–65) | copy/share matrix, `string` exception, implementation-status bullet |
| `docs/spec/06-functions.md` `### deepcopy` (:36–85) | by-type result table, cycles, "never mutates", arity rule, worked example |
| `docs/spec/10-classes.md` `## deepcopy` (:149–165) | instance copy rules, `__deepcopy__` override, `__destroy__` note |
| `docs/spec/13-bytecodes.md` | `DEEPCOPY` operand row (:100), `## OpCode deepcopy` (:335–350), "bytecode encoding guidelines" (:494–503) |

The two locked encoding rules are already in the spec (`13-bytecodes.md:501–502`):
*favour passing objects by pointer (arrays, tables, sets, objects, blobs);
primitives and strings by value* and *deep copy must be explicit; `string` is
the exception — copied, immutable*.

Precedent for the plumbing: MOVI (R30), ADDI (R31), SUBI + cmp/j\* (R32) —
append to the enum, register the operand count, handle it in the **single**
dispatch switch, extend the codec identity test. `.qlc` stays **v5**.

> **Audit note (2026-11-05).** This plan was re-checked against the tree before
> implementation. The line references and the class-instance model below are
> the corrected ones; older wording ("all four dispatch loops", `usesI32()`,
> "`ObjectDescriptor` is the instance representation") was stale and has been
> replaced.
>
> **Known drift in other docs (not fixed here).** The MOVI/ADDI sections of
> `docs/spec/13-bytecodes.md:104–109,158–166` still describe `usesI32()` and
> "all four exec loops", and `docs/compliance/status.md` repeats the four-loops
> wording in several historical rows. Those are historical records of rounds
> 30/31 (accurate at the time); the current tree has `baseOperandCount()` and a
> single `runFrames()` executor. Fixing them means re-hashing the spec module —
> do it in its own docs round, not inside this feature.

---

## Background / current state

`QLangValue` is `{ Type m_type; QLangVariant m_storage; }`
(`include/qlang/QLangValue.h:21–234`). The variant
(`include/qlang/QLangVariant.h:137–142`) stores each type either **by pointer
(shared)** or **by value (copied)**:

| Type | Variant alternative | Storage | `MOV` / call-arg does |
|---|---|---|---|
| char/byte/word/short/int/unsigned/long/qword/float/bool | raw scalar | value | copies the value (correct — primitive) |
| **table** | `TableValue = shared_ptr<map<string,QLangValue>>` (`QLangVariant.h:70`) | **pointer** | copies the pointer → **shared** ("self semantics") |
| **object** | `ObjectValue = shared_ptr<ObjectDescriptor>` (`QLangVariant.h:82–90`) | **pointer** | copies the pointer → shared |
| **closure / function** | `ClosurePtr = shared_ptr<Closure>` | **pointer** | copies the pointer → shared |
| **generator** | `GeneratorValuePtr = shared_ptr<GeneratorState>` | **pointer** | copies the pointer → shared |
| **external / bound / typeref** | ref structs (`shared_ptr`/`weak_ptr`/raw) | **pointer** | copies the ref → shared |
| **array / set** | `ArrayValue = vector<QLangValue>` (`QLangVariant.h:52`) | **value** | **deep-copies the whole array** |
| **blob** | `vector<uint8_t>` | **value** | **copies the byte buffer** |
| **string** | `std::string` | **value** | **copies the string buffer** |
| nil / eof | `monostate` / tag only | value | copies (nothing to share) |

### Class instances are tables, not `ObjectDescriptor`

**This is the single most important correction to the original plan.** A
user-defined class instance is **not** a `Type::Object`. Instantiation codegen
(`src/QLangCompiler.cpp:3199–3266`) builds a **`TableValue`** and stamps it with
reserved meta keys:

| Key | Constant | Meaning |
|---|---|---|
| `__class__` | `kClassMetaKey` (`src/QLangCompiler.cpp:174`) | class name (closed-field-set error messages) |
| `__fields__` | `kFieldMetaKey` (`:171`) | table of the closed field set |
| `__const_fields__` | `kConstFieldMetaKey` (`:178`) | table of read-only fields (enforced by `SETFIELD`) |
| `__field_types__` | `kFieldTypeMetaKey` (`:183`) | table of inferred field types |
| `__statics__` | literal (`:1299`, `:2509`, `:3092`, `:3199`, `:3558`) | shared static anchor for the class |

Methods live in that same table as closure values. `Type::Object` /
`ObjectDescriptor` (`{ const ObjectOps* ops; map<string,QLangValue> fields; }`)
is the **raw / external ops substrate**: no language construct creates one
today — only C++ embedders and unit tests do. Consequences for this plan:

- `deepcopy` of a *script* instance is a **table** copy (see "Instance
  semantics").
- the `__deepcopy__` override is a **QLang method resolved through the
  instance table**, not an `ObjectOps` function pointer (Phase 3).
- `Type::Object` still needs a `deepCopyValue` branch; the defensible default
  is **share the handle** (open decision #2).

Call convention (spec `13-bytecodes.md:59,81`): `CALL`/`CALLFN` args live in
`r[dest+1..dest+argc]`; the compiler stages each arg into those slots with a
`MOV`, and `QLangVM::pushCall` copies them into the callee's param registers
(`cf.regs[k] = callerRegs[dest+1+k]`). Because the slot holds a full
`QLangValue`, **whether a call "copies the object" is decided by the type's
variant alternative**: pointer-backed types are shared for free; value-backed
types (array/set/blob/string) are deep-copied on every move and every call.

That is the inconsistency this plan removes: `table` is shared but `array`/
`set`/`blob` are copied, even though all are the spec's `varying`
(non-primitive) class (`01-types.md:18–36`). **`string` is the deliberate
exception** — it is immutable, so it is *meant* to be copied (by value), not
shared; it stays a value type (Design decision #3).

---

## Design decisions (locked)

1. **`deepcopy(x)` builtin** (Option A). Python-like, composes anywhere an
   expression is allowed. This is the *only* way to get an independent copy of a
   non-primitive.
2. **Default = share.** Every non-primitive (`array`, `set`, `table`, class
   instance, and the ref types) — **except `string`** — is shared on assignment
   and on call. No implicit deep copy anywhere.
3. **String is the exception.** `string` is **immutable**, so it is **copied by
   value** on assignment and on call — like a primitive, not shared. Copying an
   immutable value is safe (no aliasing hazard) and sharing buys nothing (there
   is no mutation to observe). `string` stays a value type; it is never
   pointerized.
4. **Primitives = value.** The fixed-size scalars (`01-types.md` rows with a
   byte count) are copied by value; that is correct and needs no keyword.
5. **`deepcopy` is recursive** — it copies everything *reachable* through the
   value (nested arrays/sets/tables/instances), not just the top container.
6. **Cycle-safe** — the deep copy carries an identity map
   (`original → copy`) so a value that transitively contains itself terminates
   and the cycle is preserved in the copy. **Scope:** the map is keyed on the
   `shared_ptr` payload address, so it only applies to pointer-backed payloads
   (table, `ObjectDescriptor`). See "Cycles".
7. **Instance rules** (see "Instance semantics"): copy the data fields
   recursively, **share** the class-level meta keys and methods, **share**
   fields holding closures/generators/external refs, honour `__deepcopy__`.
8. **Breaking change** — `array`/`set`/`blob` go from copy-on-assign/call to
   **share-on-assign/call**. Scripts relying on "assigning an array gives me an
   independent copy" will now alias. Migration note required (Phase 4).
   (`string` is unaffected — it is already copied and stays copied.)

---

## Semantics

### `deepcopy(x)` by type

| Input `x` | Result |
|---|---|
| nil / bool / char / byte / word / short / int / unsigned / long / qword / float | a new value equal to `x` (value copy; cheap) |
| eof | the eof sentinel (nothing to copy) |
| string | a new string with the same bytes (value copy — **string is the immutable exception: always copied, never shared**) |
| array / set | a new container; **each element is itself `deepcopy`'d** (recursive) |
| table (plain) | a new map; **each value is itself `deepcopy`'d** (recursive) |
| **class instance** (table + meta keys) | a new table of the same class: data fields copied recursively, meta keys and methods shared, `__deepcopy__` honoured, own `__destroy__` finalization |
| `Type::Object` (`ObjectDescriptor`) | **share the handle** by default (open decision #2) |
| closure / function / generator | **shared, not copied** — `deepcopy` returns the same handle |
| external / bound method / typeref | **shared, not copied** (views over C++ objects) — same handle |

`deepCopyValue` must cover **every** `QLangValue::Type` alternative
(`include/qlang/QLangValue.h:23–49`) — including `Eof`, `Function`,
`BoundMethod` and `TypeRef`, which the first draft of this plan omitted. A
`switch` with no `default` (or a `default` that throws) makes the omission
impossible to ship silently.

### Cycles

The recursive walk keeps `std::unordered_map<const void*, QLangValue> seen`
keyed on the **payload address** (the `shared_ptr`'d map / descriptor). On
entering a container already in `seen`, emit the *existing* copy instead of
re-walking — terminates, and preserves the cycle in the result.

**Limitation until Phase 2.** `array`, `set` and `blob` are stored **by value**
(`vector<QLangValue>`), so they have no stable address to key on: two copies of
the same array are indistinguishable. Consequences:

- Today a cycle *through* an array cannot even be constructed — an array
  literal cannot contain itself, and `a[] = a` stores a copy, so the graph is
  always finite. Cycle-safety for arrays is therefore **unobservable in Phase
  1**; the spec's "cycles are preserved" guarantee (`06-functions.md:79–81`) is
  testable only through **tables** until Phase 2 makes arrays pointer-backed.
- Phase 1 must still key tables in `seen` (a table cycle is constructible today
  via `t.self = t`), and must not key value payloads. Caveat: QLang is purely
  refcounted, so a script-visible self-reference is also a **leak** — the test
  must not assert on collection counts, and the plan does not change that
  (see "Explicitly out of scope": no GC).
- Phase 2 adds the array/set identity keys; the cycle test then extends to
  arrays.

### Instance semantics (table-based)

For a `TableValue` carrying `__class__` (i.e. a class instance), `deepcopy`:

1. **`__deepcopy__` override first.** If the instance table resolves
   `__deepcopy__` to a callable, run it **on the original** and take its return
   value as the copy; it may throw to refuse (`10-classes.md:164–165`). Guard:
   if the hook returns the original handle, that is a script error (the copy
   must be distinct) — see Phase 3.
2. **Else, build a new table:**
   - **Meta keys are shared, never copied** — `__fields__`,
     `__const_fields__`, `__field_types__` and `__statics__` describe the
     *class*, and `10-classes.md:154–155` requires the class (field layout,
     method table) to be shared. Copying them would fork the closed field set
     and the static anchor. `__class__` is a string → copied by value.
   - **Methods / function-valued fields are shared** (same closure handle) —
     `10-classes.md:158–159`.
   - **Data fields** (the keys declared in `__fields__`) are `deepcopy`'d:
     primitives and strings by value, nested arrays/sets/tables/instances
     recursively, `const` fields with their values (the copy re-reads
     `__const_fields__`, so the same const rules apply).
3. **`__destroy__` caveat.** Finalizers are attached by re-wrapping the table's
   `shared_ptr` with a deleter (`attachFinalizer`, `src/QLangVM.cpp:109–124`),
   and the ownership subtlety is documented there (`:118–123` — a second
   control block would dangle the map). A deep copy is a **new table**, so it
   must get its **own** finalizer registration through the same path, and the
   original's registration must not be duplicated (`10-classes.md:162–163`:
   "the copy is a new instance with its own count and its own `__destroy__`
   finalization"). Reusing `attachFinalizer` on the copy is the intended
   route; do **not** hand-roll a second `shared_ptr(raw, deleter)`.
4. **Plain tables** (no `__class__`) are copied as maps, values recursed —
   including any `__destroy__` key, which is a method value and therefore
   shared, with the copy registered through `attachFinalizer`.

---

## Phases

Each phase is independently shippable with `ctest` green.

### Phase 1 — `deepcopy` builtin + `DEEPCOPY` opcode (additive, S–M, low risk)

Pure addition: no existing behavior changes. `deepcopy` works on top of the
current storage (it copies payloads directly, pointer- or value-backed).

1. **Opcode** — append `DEEPCOPY` after `EMIT` in the `Opcode` enum
   (`include/qlang/QLangBytecode.h:212`), giving identity **70**; bump
   `kOpcodeUsedCount` **70 → 71** (`:217`, comment "defined identities 0..69" →
   "0..70"). `isValidOpcode()` is `op < kOpcodeUsedCount`
   (`src/QLangBytecode.cpp:14`) — no other gate. Two register operands
   (`a` = dest, `b` = src), register-register like `MOV`. Add it to the
   two-operand case in `baseOperandCount()` (`src/QLangBytecode.cpp`) — that is
   the **only** arity declaration site; `usesI32()` no longer exists (removed by
   the escape-byte format migration). Add the mnemonic to the opcode-name switch
   (`src/QLangBytecode.cpp:816–817` region) so the disassembler prints
   `deepcopy`.
2. **VM** — implement `QLangValue deepCopyValue(const QLangValue&,
   std::unordered_map<const void*, QLangValue>&)` in `src/QLangVM.cpp` (or a
   small `QLangDeepCopy.cpp` unit) covering **every** variant alternative per
   the table above, with the identity map for pointer-backed payloads only.
   `DEEPCOPY` handler: `regs[a] = deepCopyValue(regs[b], seen)` with a fresh
   `seen` per instruction. **One** dispatch site: the tree has a single executor
   (`QLangVM::runFrames()`, one switch) since Remediation Phase 1 — the "four
   exec loops" note in `external-modules-plan.md` is stale.
3. **Builtin** — append `Deepcopy` to the `Builtin` enum
   (`include/qlang/QLangBytecode.h:25–47`, currently `ToStr = 0 … Release = 20`,
   so `Deepcopy` = **21**; ids ride in `CALL.b`, so no format change); register
   the name in `QLangCompiler::builtinId()` (`src/QLangCompiler.cpp:3829`) and
   in the disassembler `builtinName()` (`examples/qlang_run.cpp:190`).
4. **Arity** — the spec says `deepcopy()` and `deepcopy(a, b)` are **compile**
   errors (`06-functions.md:83`), but the engine has **no compile-time arity
   table**: every builtin checks at runtime with `need(n, name)` inside
   `callBuiltin` (`src/QLangVM.cpp:432`). Pick one and record it (open decision
   #1): either add the first compile-time arity check in the compiler (new
   mechanism, consistent with the spec wording) or emit a runtime `need(1, …)`
   error and soften the spec line to "runtime error".
5. **Compiler lowering** — a `deepcopy(x)` call lowers **directly to
   `DEEPCOPY dest, src`** (compile the arg into a register, emit `DEEPCOPY`) —
   *not* a `CALL` into arg slots. Precedent for direct builtin→opcode lowering:
   `len()` → `Opcode::LEN` (`src/QLangCompiler.cpp:1934`). This honours the
   "avoid temporary register / copy directly to the final register" guidelines
   (`13-bytecodes.md:497–498`) and avoids staging a possibly large value
   through `dest+1..`. Note the builtin `CALL` lowering
   (`src/QLangCompiler.cpp:3399`) and the `release` write-back special case in
   the VM (`src/QLangVM.cpp:1893`) — a direct lowering avoids needing a second
   such special case.
   Fallback if direct lowering is fiddly: keep it a normal builtin `CALL` and
   handle `Deepcopy` inside `callBuiltin` (`src/QLangVM.cpp:432`) by calling
   `deepCopyValue` — same result, one extra call frame, and the arity check
   comes for free.
6. **Codec test (must change)** — `tests/unit/codec_check.cpp:835–841` builds a
   "not a defined opcode" case by writing byte **70** and the comment still
   says `kOpcodeUsedCount == 69`. With `DEEPCOPY = 70` that byte becomes
   **valid**, so the negative test would silently stop rejecting. Bump the byte
   to **71** and fix the comment to `kOpcodeUsedCount == 71`. Add a
   `DEEPCOPY` round-trip case.
7. **Tests** — `spec_check2`: `deepcopy` of each type; independence (mutate
   original, copy unchanged); nested table + array; **instance** with mixed data
   + method fields (meta keys shared, `__class__` preserved, `type()` equal);
   `deepcopy` of a closure/generator/external returns the same handle; table
   cycle (`t.self = t`).
   **Known blocker for the independence tests:** array/table **index
   assignment** (`ab[0] = 55`, `tbl["age"] = 33`) currently throws
   `unsupported operator ''` — `QLangCompiler::compileAssignTarget()` does an
   unconditional read-modify-write when `target->indexExpr` is set, so
   `binOp(op.text.substr(0, op.text.size()-1))` is called with `""` and throws
   (`src/QLangCompiler.cpp:4006`); `ab[0] += 5` works. The spec allows index
   assignment (`03-declarations.md`) and `docs/compliance/status.md:107` marks
   it verified. Either fix it in this round (small `isCompound` guard) or write
   the independence tests with append (`a[] = v`) / member (`t.f = v`)
   mutation, which do work.
8. **Docs** — flip the two **Proposed** markers to implemented:
   `13-bytecodes.md:100` (operand row) and `:337`; `01-types.md:60–65`
   implementation-status bullet (drop the "`deepcopy` is not yet implemented"
   clause); `docs/compliance/status.md` Open Item (:196) → verified. Compliance
   hashes after the green run (AGENTS.md).

### Phase 2 — pointer-backed storage for `array` / `set` / `blob` (breaking, L, high risk)

The change that makes the **default** share-by-pointer for the value-backed
types. This is the breaking phase — keep it standalone so any regression is
easy to bisect.

1. **`include/qlang/QLangVariant.h`**
   - `ArrayValue` → `std::shared_ptr<std::vector<QLangValue>>` (set reuses the
     same alternative, as today).
   - `Blob` → `std::shared_ptr<std::vector<uint8_t>>`.
   - **String — not changed** (locked exception, Design decision #3): stays
     `std::string` by value. Immutable → copied on move/call; never
     pointerized. Out of scope for this phase.
2. **`include/qlang/QLangValue.h`** — `Array()`/`Set()`/`Blob()` factories
   wrap in `make_shared`; `asArray()/asSet()/asBlob()` return
   `const shared_ptr<...>&` (and the mutable overload). Audit every call site
   that derefs the old by-value form.
3. **`src/QLangVM.cpp`** — **one** executor (`runFrames()`) plus every
   `asArray/asBlob` use site; element access goes through the `shared_ptr`;
   array/blob mutations now mutate the shared payload (that is the point).
4. **`src/QLangCompiler.cpp`** — array/blob literal + constructor emission
   builds the `shared_ptr` payload; no semantic change to codegen.
5. **`deepCopyValue`** — arrays/sets/blobs now have a stable payload address:
   add them to the cycle identity map and extend the cycle tests (removes the
   Phase 1 limitation in "Cycles").
6. **`src/QLangBytecodeIO.cpp`** — `.qlc` **durable format is unchanged**: a
   value still serializes its *payload* (the pointer is an in-memory detail).
   Durable tags (0–19) stay stable. Reader/writer just (de)reference the
   `shared_ptr` around the existing payload encode/decode. Bump the format
   version only if the on-disk layout actually changes (it should not).
7. **Breaking-change audit** — grep every script + test for array/blob
   assignment and call-arg patterns that assumed copy semantics; update
   expectations. `spec_check2` + `codec_check` + the harness binaries
   (`range_check`, `test_gen_simple`, `test_module`) all re-run green.
8. **Docs** — `01-types.md`: flip the implementation-status bullet to
   shared-by-default; `SERIALIZATION.md`: pointer-backed in-memory rep (durable
   format unchanged); migration note for the copy→share change. Compliance
   hashes after green run.

### Phase 3 — `__deepcopy__` override (M, medium risk)

The override is a **QLang method**, not an `ObjectOps` entry — script classes
have no `ObjectOps` (see "Class instances are tables, not `ObjectDescriptor`").

1. **Resolution** — in `deepCopyValue`, before the default field copy, look up
   `__deepcopy__` in the instance table. The precedent is the `__str__` path
   (`src/QLangVM.cpp:1145–1150`): `find("__str__")` → require
   `Type::Function` → `callClosure(it->second, {v})`; `attachFinalizer`
   (`:124–130`) resolves `__destroy__` the same way. If `__deepcopy__` is
   present and callable, invoke it **on the original** and take the return
   value as the copy.
2. **Guard** — if the hook returns the original handle (or nil), raise a
   runtime error: a hook must return a distinct instance or throw
   (`10-classes.md:164–165`). Calling script code from inside `deepCopyValue`
   needs the VM's call path, so the hook dispatch lives in the `DEEPCOPY`
   handler (or a VM-aware helper), not in a free function.
3. **`Type::Object`** — leave the ops-based path alone unless an embedder asks
   for an `ObjectOps::deepcopy` hook (open decision #2). Do not add an unused
   `ObjectOps` entry.
4. **Tests** — default instance copy (fields independent, class shared, method
   field shared, `__const_fields__` still enforced on the copy); custom
   `__deepcopy__`; a hook that throws (refuse to copy); a hook returning
   `self` (must error); instance holding `__destroy__` (copy finalizes
   independently, original untouched).
5. **Docs** — `10-classes.md` `## deepcopy` already specifies the override;
   drop the "planned" framing once implemented and re-hash.

### Phase 4 — migration note + tracking

Items 1–3 of the original Phase 4 (spec text for `01-types.md` value
semantics, the `13-bytecodes.md` `DEEPCOPY` row + locked encoding rules, and
`06-functions.md` call-argument semantics) **landed 2026-11-05** and are no
longer open work. Remaining:

1. **Migration note** — a short `docs/compliance/` entry: array/set/blob
   assignment and call arguments changed from copy to share in this round;
   scripts that needed an independent copy must now write `deepcopy(x)`.
2. **Tracking** — flip the `deepcopy` Open Item in `docs/compliance/status.md`
   (:196) as each phase lands; append an audit round to
   `docs/compliance/history.md`.
3. **Hashes** — SHA256 rows in `docs/spec/README.md` /
   `docs/compliance/README.md` updated **only after** the green run (AGENTS.md:
   testing proves correctness, the hash merely detects change).

---

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites incl. `codec_check`, `spec_check2`,
  `range_check`, `test_gen_simple`, `test_module`)
- Compliance hashes updated only after the green run.

---

## Open decisions

1. **Arity check: compile-time or runtime** — the spec says compile error
   (`06-functions.md:83`); the engine only has runtime `need()` in
   `callBuiltin` (`src/QLangVM.cpp:432`). Decide: introduce the first
   compile-time arity table, or amend the spec to "runtime error".
2. **`Type::Object` (`ObjectDescriptor`) handling** — share the handle
   (recommended: no language construct creates one, so there is no user-visible
   behavior to define yet) vs deep-copy `fields` and share `ops` vs add an
   `ObjectOps::deepcopy` hook now.
3. **Hook mechanism** — QLang `__deepcopy__` method resolved through the
   instance table (recommended, matches `10-classes.md`) vs an `ObjectOps`
   function pointer (only reachable from C++ embedders).
4. **Index-assignment defect** — fix `compileAssignTarget()` in Phase 1 (small
   `isCompound` guard, unblocks the spec examples and the independence tests)
   or defer to its own round. It is a pre-existing bug, not caused by this
   plan.
5. **`shallowcopy(x)` companion** — copy the top container, share elements.
   Cheap and useful; defer to a follow-up round (not required for this plan).
6. **`copy` parameter specifier (Option D)** — `def f(copy x)` to deep-copy a
   param on entry. Deferred: Option A (`deepcopy` builtin) was chosen; the
   specifier is a natural later sugar over it.
7. **Rollout of the breaking change** — version bump / compat flag for old
   `.qlc` (copy semantics) vs a clean break (the spec's compat rule is "exact
   same compiler version", `13-bytecodes.md:492`, so a clean break is
   defensible). Decide in the Phase 2 round.

---

## Explicitly out of scope

- `shallowcopy` and the `copy` param specifier (open decisions #5, #6) — defer.
- An immutability / `frozen` type keyword.
- A garbage collector — QLang is refcounted (`shared_ptr`); `deepcopy` bumps
  refs, it does not allocate a GC graph.
- Deep-copying closures/generators/external refs — by design shared (see
  "`deepcopy(x)` by type"); a real closure clone is a separate feature.
- Changing the `.qlc` durable value layout — Phase 2 keeps the on-disk format
  stable (pointer is an in-memory detail).
- Pointerizing `string` — it is the immutable exception and stays a value type
  (Design decision #3).
- Rewriting class instances on top of `ObjectDescriptor` — out of scope; this
  plan works with the table-based instance model as it stands.
