# Class Implementation Action Plan

Based on the conformance table, here's the implementation plan. The baseline is `docs/EXTERNAL_TYPES.md` §9 (`ObjectOps` dispatch layer) — classes are its first consumer.

## Phase 0: ObjectOps Dispatch Layer (Prerequisite)
**Goal:** Implement the shared object-dispatch foundation that both `Object` and `Class` types will use.

| Task | Description |
|------|-------------|
| 0.1 | Add `ObjectOps` struct to `QLangValue.h` with `GETFIELD`/`SETFIELD`/`CALLMETHOD`/`TOSTRING` function pointers |
| 0.2 | Extend `QLangValue::Type` enum: add `Object` (for class instances) |
| 0.3 | Implement `ObjectDescriptor` in `QLangValue.cpp` — holds `ObjectOps` + field/method bindings |
| 0.4 | Update `QLangVM.cpp` to use `ObjectOps` for member access (dispatch via `obj->ops->getfield(...)`) |
| 0.5 | Add `type(x)` native function returning `obj->typeDescriptor->name` |

## Phase 1: Closed Field Set Enforcement
**Goal:** Instances reject undeclared field access.

| Task | Description |
|------|-------------|
| 1.1 | In `parseClass()`, track declared field names in a `HashSet<String>` |
| 1.2 | At codegen, emit field metadata (names → indices) into the class prototype |
| 1.3 | In `GETFIELD`/`SETFIELD`, check if field exists; if not, emit runtime error `field 'x' not found on type 'T'` |
| 1.4 | Add test: `obj.f` on undeclared field → runtime error |

## Phase 2: `const` Fields
**Goal:** Read-only fields enforced at runtime.

| Task | Description |
|------|-------------|
| 2.1 | Extend field descriptor: add `isConst` flag |
| 2.2 | In `SETFIELD`, check `isConst`; if true, emit runtime error `cannot assign to const field 'x'` |
| 2.3 | Parser: recognize `const` keyword in class body |
| 2.4 | Add test: `obj.c = 1` on const field → runtime error |

## Phase 3: Unique Method/Dunder Names
**Goal:** Compile-time check for duplicate method definitions.

| Task | Description |
|------|-------------|
| 3.1 | In `parseClass()`, maintain `HashMap<String, SourceLoc>` for method names |
| 3.2 | On duplicate, emit compile error `duplicate method 'm' defined at line X and line Y` |
| 3.3 | Add test: `class C { m()[] {} m()[] {} }` → compile error |

## Phase 4: Fixed Arity Checking
**Goal:** Constructor and method calls must match declared arity.

| Task | Description |
|------|-------------|
| 4.1 | Store method arity in `MethodBinding` |
| 4.2 | In `CALLMETHOD`, compare arg count vs arity; mismatch → runtime error |
| 4.3 | Constructor: check `ClassName(args)` vs `__init__` arity |
| 4.4 | Class without `__init__`: enforce zero-arg constructor |
| 4.5 | Add tests: wrong arg count → runtime error |

## Phase 5: `__str__` Implementation
**Goal:** `str(obj)` returns class name or custom string representation.

| Task | Description |
|------|-------------|
| 5.1 | Add `__str__` to `ObjectOps` table |
| 5.2 | Default: return class name |
| 5.3 | Parser: recognize `__str__(self)[] { return ... }` |
| 5.4 | In `TOSTRING`, call user-defined `__str__` if present |
| 5.5 | Add test: `str(obj)` → class name or custom string |

## Phase 6: Reference Counting
**Goal:** Instances are shared, never copied.

| Task | Description |
|------|-------------|
| 6.1 | Instances use `std::shared_ptr<ObjectDescriptor>` |
| 6.2 | Assignment copies the shared_ptr (reference, not data) |
| 6.3 | Add test: `local a = obj; local b = a; b = nil; a` → still valid |

## Phase 7: `__destroy__` Finalization
**Goal:** `__destroy__` runs exactly once at final release.

| Task | Description |
|------|-------------|
| 7.1 | Add `isDestroyed` flag to `ObjectDescriptor` |
| 7.2 | In `TOSTRING`/`GETFIELD`/`SETFIELD`, check `isDestroyed`; if true, emit error |
| 7.3 | When `shared_ptr` ref-count hits zero, call `__destroy__` if defined |
| 7.4 | Add test: `__destroy__` called exactly once on final release |

## Phase 8: `type(x)` for Instances
**Goal:** `type(x)` returns the class name for instances.

| Task | Description |
|------|-------------|
| 8.1 | `type(x)` on `Object` type → return `obj->typeDescriptor->name` |
| 8.2 | Add test: `type(instance)` → class name string |

## Phase 9: Instances Are NOT Tables
**Goal:** The `$` dynamic-key pipeline does not apply to instances.

| Task | Description |
|------|-------------|
| 9.1 | Instances use `ObjectDescriptor`, not `TableValue` |
| 9.2 | `$` operator on instances → compile error `cannot use $ on non-table type` |
| 9.3 | Add test: `obj.$"x"` → compile error |

## Verification & Testing
After each phase, run:
- `spec_check2` (204 cases)
- Add new test cases for each feature
- Verify no regressions in existing tests

## Build Order
1. ObjectOps dispatch layer (Phase 0)
2. Closed field set (Phase 1)
3. `const` fields (Phase 2)
4. Unique methods (Phase 3)
5. Arity checking (Phase 4)
6. `__str__` (Phase 5)
7. Reference counting (Phase 6)
8. `__destroy__` (Phase 7)
9. `type(x)` (Phase 8)
10. Instances ≠ tables (Phase 9)

This plan addresses all 8 non-conforming items in the table. The `ObjectOps` layer (Phase 0) is the critical foundation — it must land before any class-specific features.