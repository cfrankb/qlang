# Plan: `deepcopy` builtin + default-share value semantics

Adopted 2026-11-01. Syntax decision: **Option A — a `deepcopy(x)` builtin,
mirroring Python `copy.deepcopy`.** Companion rule: **deep copy of
non-primitive values is always explicit**; the default for every non-primitive
is **share / pass-by-pointer**.

Spec anchors: `docs/spec/01-types.md` (type table), `docs/spec/13-bytecodes.md`
(opcode table + "bytecode encoding guidelines", lines 428–434),
`docs/spec/06-functions.md` (call/closure semantics). Precedent for the
opcode/builtin plumbing: MOVI (R30), ADDI (R31), SUBI + cmp/j* (R32) — all
followed append-to-enum + all-dispatch-loops + codec-version.

---

## Background / current state

`QLangValue` is `{ Type m_type; QLangVariant m_storage; }`
(`include/qlang/QLangValue.h:21–234`). The variant
(`include/qlang/QLangVariant.h:99–104`) stores each type either **by pointer
(shared)** or **by value (copied)**:

| Type | Variant alternative | Storage | `MOV` / call-arg does |
|---|---|---|---|
| char/byte/word/short/int/unsigned/long/qword/float/bool | raw scalar | value | copies the value (correct — primitive) |
| **table** | `TableValue = shared_ptr<map<string,QLangValue>>` | **pointer** | copies the pointer → **shared** ("self semantics", QLangVariant.h:44) |
| **object** | `ObjectValue = shared_ptr<ObjectDescriptor>` | **pointer** | copies the pointer → shared |
| **closure** | `ClosurePtr = shared_ptr<Closure>` | **pointer** | copies the pointer → shared |
| **generator** | `GeneratorValuePtr = shared_ptr<GeneratorValue>` | **pointer** | copies the pointer → shared |
| **external / bound / typeref** | ref structs (`shared_ptr`/`weak_ptr`/raw) | **pointer** | copies the ref → shared |
| **array / set** | `ArrayValue = vector<QLangValue>` | **value** | **deep-copies the whole array** |
| **blob** | `vector<uint8_t>` | **value** | **copies the byte buffer** |
| **string** | `std::string` | **value** | **copies the string buffer** |

Call convention (spec `13-bytecodes.md:51,73`): `CALL`/`CALLFN` args live in
`r[dest+1..dest+argc]`; the compiler stages each arg into those slots with a
`MOV`, and `QLangVM::pushCall` copies them into the callee's param registers
(`src/QLangVM.cpp:1344–1362`, `cf.regs[k] = callerRegs[dest+1+k]`). Because the
slot holds a full `QLangValue`, **whether a call "copies the object" is decided
by the type's variant alternative**: pointer-backed types are shared for free;
value-backed types (array/set/blob/string) are deep-copied on every move and
every call.

That is the inconsistency this plan removes: `table` is shared but `array`/
`set`/`blob` are copied, even though all are the spec's `varying`
(non-primitive) class (`01-types.md:18–36`). **`string` is the deliberate
exception** — it is immutable, so it is *meant* to be copied (by value), not
shared; it stays a value type (see Design decision #3).

The adopted rule (from the encoding-guidelines discussion): **when passing
objects, favour pass-by-pointer for anything that is not a primitive (does not
fit in a register pair, ~16 B).** Pointer-backed types already satisfy it; the
value-backed ones do not.

---

## Design decisions (locked)

1. **`deepcopy(x)` builtin** (Option A). Python-like, composes anywhere an
   expression is allowed. This is the *only* way to get an independent copy of a
   non-primitive.
2. **Default = share.** Every non-primitive (`array`, `set`, `table`, `object`,
   and the ref types) — **except `string`** — is shared on assignment and on
   call. No implicit deep copy anywhere.
3. **String is the exception.** `string` is **immutable**, so it is **copied by
   value** on assignment and on call — like a primitive, not shared. Copying an
   immutable value is safe (no aliasing hazard) and sharing buys nothing (there
   is no mutation to observe). `string` stays a value type; it is never
   pointerized.
4. **Primitives = value.** The fixed-size scalars (`01-types.md` rows with a
   byte count) are copied by value; that is correct and needs no keyword.
5. **`deepcopy` is recursive** — it copies everything *reachable* through the
   value (nested arrays/sets/tables/objects), not just the top container.
6. **Cycle-safe** — the deep copy carries an identity map
   (`original → copy`) so a value that transitively contains itself terminates
   and the cycle is preserved in the copy.
7. **Object rules** (see "Object semantics" below): copy `fields` recursively,
   **share** the `ObjectDescriptor` (ops/type/methods), **share** fields that
   are functions/closures/generators/external refs, and honour an optional
   `__deepcopy__` hook.
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
| string | a new string with the same bytes (value copy — **string is the immutable exception: always copied, never shared**) |
| array / set | a new container; **each element is itself `deepcopy`'d** (recursive) |
| table | a new map; **each value is itself `deepcopy`'d** (recursive) |
| object | new instance of the **same class**: `fields` copied recursively, `ObjectDescriptor` shared, non-data fields shared, `__deepcopy__` hook honoured |
| closure / generator | **shared, not copied** (a closure captures live refs; a generator has state) — `deepcopy` returns the same handle |
| external / bound method / typeref | **shared, not copied** (views over C++ objects) — same handle |

### Cycles

The recursive walk keeps `std::unordered_map<const void*, QLangValue> seen`
keyed on the **payload address** (the `shared_ptr`'d map/vector/descriptor).
On entering a container already in `seen`, emit the *existing* copy instead of
re-walking — terminates, and preserves the cycle in the result.

### Object semantics

`ObjectDescriptor` = `{ const ObjectOps* ops; std::map<string,QLangValue> fields; }`
(QLangVariant.h:62–65). `deepcopy` of an instance:

1. If the class defines `__deepcopy__`, call it with the original and let it
   return the copy (full control — including throwing to refuse).
2. Else: allocate a new instance of the same class (same `ops`), then for each
   field: data fields (primitive/string/array/set/table/object) are
   `deepcopy`'d; **function/closure/generator/external/bound/typeref fields are
   shared** (copied as the same handle).
3. The `ObjectDescriptor` (type + method table) is **never** deep-copied — the
   copy is the same class.

---

## Phases

Each phase is independently shippable with `ctest` green.

### Phase 1 — `deepcopy` builtin + `DEEPCOPY` opcode (additive, S–M, low risk)

Pure addition: no existing behavior changes. `deepcopy` works on top of the
current storage (it copies payloads directly, pointer- or value-backed).

1. **Opcode** — append `DEEPCOPY` before `kOpcodeCount` in
   `include/qlang/QLangBytecode.h` (`Opcode` enum; ~68 ops today). Two
   register operands (`a` = dest, `b` = src), register-register like `MOV` —
   **no** `usesI32()` entry (precedent: `CMP`).
2. **VM** — implement `QLangValue deepCopyValue(const QLangValue&,
   std::unordered_map<const void*, QLangValue>&)` in `src/QLangVM.cpp`
   (or a small `QLangDeepCopy.cpp` unit) covering every variant alternative
   per the table above, with the identity map for cycles. `DEEPCOPY` handler:
   `regs[a] = deepCopyValue(regs[b], seen)`. Land in **all four** dispatch
   loops (`run`, `runFrames`, and the two in `callFunction` — see
   `external-modules-plan.md` Background for the four sites).
3. **Builtin** — add `Deepcopy` to the `Builtin` enum
   (`include/qlang/QLangBytecode.h:25`, currently `ToStr=0…Set=19`); register
   the name in `QLangCompiler::builtinId()` (`src/QLangCompiler.cpp`, ~3056);
   the disassembler `builtinName()` (`examples/qlang_run.cpp`).
4. **Compiler lowering** — a `deepcopy(x)` call lowers **directly to
   `DEEPCOPY dest, src`** (compile the arg to a temp, emit `DEEPCOPY`, move to
   the real dest) — *not* a `CALL` into arg slots. This honours the
   "avoid temporary register / copy directly to the final register" guideline
   and avoids staging the (possibly large) value through `dest+1..`.
   Fallback if direct lowering is fiddly: keep it a normal builtin `CALL` and
   handle `Deepcopy` inside `callBuiltin` (`src/QLangVM.cpp:385`) by calling
   `deepCopyValue` — same result, one extra call frame.
5. **Tests** — `spec_check2`: `deepcopy` of each type; independence (mutate
   original, copy unchanged); nested + cyclic table/array; object with mixed
   data + function fields; `deepcopy` of a closure/generator returns the same
   handle. `codec_check`: `DEEPCOPY` round-trips in `.qlc`.
6. **Docs** — `13-bytecodes.md`: `DEEPCOPY` operand row + status marker;
   `06-functions.md`: `deepcopy` note. Compliance hashes after green run
   (AGENTS.md).

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
3. **`src/QLangVM.cpp`** (all four loops + every `asArray/asBlob` use) —
   element access goes through the `shared_ptr`; array/blob mutations now
   mutate the shared payload (that is the point).
4. **`src/QLangCompiler.cpp`** — array/blob literal + constructor emission
   builds the `shared_ptr` payload; no semantic change to codegen.
5. **`src/QLangBytecodeIO.cpp`** — `.qlc` **durable format is unchanged**: a
   value still serializes its *payload* (the pointer is an in-memory detail).
   Durable tags (0–19) stay stable. Reader/writer just (de)reference the
   `shared_ptr` around the existing payload encode/decode. Bump the format
   version only if the on-disk layout actually changes (it should not).
6. **Breaking-change audit** — grep every script + test for array/blob
   assignment and call-arg patterns that assumed copy semantics; update
   expectations. `spec_check2` + `codec_check` + the harness binaries
   (`range_check`, `test_gen_simple`, `test_module`) all re-run green.
7. **Docs** — `01-types.md`: storage-semantics note (non-primitives shared by
   default); `SERIALIZATION.md`: pointer-backed in-memory rep (durable format
   unchanged); migration note for the copy→share change. Compliance hashes
   after green run.

### Phase 3 — object `__deepcopy__` hook (M, medium risk)

1. **`include/qlang/QLangObjects.h`** — add an optional `deepcopy` hook to
   `ObjectOps` (or a `__deepcopy__` method resolved via the existing method
   table). Signature: `QLangValue (*)(const QLangValue& self)` returning the
   copy, or `nullptr` for the default field-copy behavior.
2. **VM `deepCopyValue`** — for the `Object` alternative: if the hook is set,
   call it; else run the default (new same-class instance, recursive field
   copy, share non-data fields). Guard against a hook that returns the original
   (document: a hook must return a distinct instance or throw).
3. **Tests** — default object copy (fields independent, class shared, function
   field shared); custom `__deepcopy__`; a hook that throws (refuse to copy);
   cyclic object graph.
4. **Docs** — `10-classes.md`: `__deepcopy__` hook reference.

### Phase 4 — spec finalization + migration note

1. `01-types.md` — add a "value semantics" subsection: primitives by value,
   non-primitives shared by default, `deepcopy` for an explicit independent
   copy. Mark `string`'s current (by-value) treatment and the deprecation row.
2. `13-bytecodes.md` — `DEEPCOPY` row in the operand map; add to the
   "bytecode encoding guidelines" the locked rule: *"favour pass-by-pointer for
   non-primitive objects; deep copy only via explicit `deepcopy`. `string` is
   the exception — copied (immutable)."*
3. `06-functions.md` — call-argument passing: shared by default; `deepcopy(x)`
   at the call site for an independent copy.
4. **Migration note** — a short `docs/compliance/` entry: array/set/blob
   assignment and call arguments changed from copy to share in this round;
   scripts that needed an independent copy must now write `deepcopy(x)`.
5. Compliance SHA256 hashes + tallies in `docs/spec/README.md` /
   `docs/compliance/status.md` — updated **only after** the green run.

---

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites incl. `codec_check`, `spec_check2`,
  `range_check`, `test_gen_simple`, `test_module`)
- Compliance hashes updated only after the green run.

---

## Open decisions

1. **`DEEPCOPY` opcode vs builtin-dispatch** — dedicated opcode (recommended;
   no arg-slot staging, matches the "avoid temp" guideline) vs handling
   `deepcopy` inside `callBuiltin` (simpler, one extra call frame).
2. **`shallowcopy(x)` companion** — copy the top container, share elements.
   Cheap and useful; defer to a follow-up round (not required for this plan).
3. **`copy` parameter specifier (Option D)** — `def f(copy x)` to deep-copy a
   param on entry. Deferred: Option A (`deepcopy` builtin) was chosen; the
   specifier is a natural later sugar over it.
4. **Rollout of the breaking change** — version bump / compat flag for old
   `.qlc` (copy semantics) vs a clean break (spec's compat rule is "exact same
   compiler version", so a clean break is defensible). Decide in the Phase 2
   round.

---

## Explicitly out of scope

- `shallowcopy` and the `copy` param specifier (Open decisions #3, #4) — defer.
- An immutability / `frozen` type keyword.
- A garbage collector — QLang is refcounted (`shared_ptr`); `deepcopy` bumps
  refs, it does not allocate a GC graph.
- Deep-copying closures/generators/external refs — by design shared (decision
  in "Object semantics"); a real closure clone is a separate feature.
- Changing the `.qlc` durable value layout — Phase 2 keeps the on-disk format
  stable (pointer is an in-memory detail).
- Pointerizing `string` — it is the immutable exception and stays a value type
  (Design decision #3).
