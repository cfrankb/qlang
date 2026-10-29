# SPEC.md / SPEC_COMPLIANCE.md Migration Plan

Move the two root monoliths into `docs/spec/` and `docs/compliance/` as
targeted modules with `README.md` entry points (per AGENTS.md migration
notice). **Pure extraction — content moves verbatim, no rewrites, no
renumbering, no section merging.**

## 1. `docs/spec/` — split SPEC.md (1712 lines) by domain

| File | SPEC.md sections (line refs) |
|------|------------------------------|
| `README.md` | index + per-module SHA256 table (new) |
| `01-types.md` | data types, strings, sets, append (16–93) |
| `02-lexical.md` | reserved words, constants, enum, comments (94–221) |
| `03-declarations.md` | boolean evaluation, declarations and assignments (222–342) |
| `04-control-flow.md` | conditions, range, switch, loops, label/goto (343–562) |
| `05-operators.md` | operators, `?`, `<`/`>`, `**`, `*`, `+=`, cast, `==`/`!=`, `$`, `$$` (563–940) |
| `06-functions.md` | built-in functions, qlang native functions (941–1082) |
| `07-api.md` | Module registration, `ctx.clear()`, verbose level (1083–1364) |
| `08-generators.md` | generators, PROTOTYPE 1, Syntax, EOF sentinel (1365–1415) |
| `09-modules.md` | modules (internal), import statement, math module (1416–1478) |
| `10-classes.md` | user defined classes incl. static, class body, …, v1 restrictions (1479–1641) |
| `11-external-types.md` | conformance with external (C++) types (1642–1683) |
| `12-errors-trace.md` | error reporting, requirements, execution trace, format (1684–1712) |

## 2. `docs/compliance/` — split SPEC_COMPLIANCE.md (845 lines) by content kind

| File | SPEC_COMPLIANCE.md sections (line refs) |
|------|------------------------------------------|
| `README.md` | index + which spec module each compliance file validates + spec-module SHA256 (new) |
| `status.md` | Current Status, Changes Since Last Audit, Legend, Working, Missing, Not Yet Implemented, Extensions Beyond Spec, Open Items (1–305, 818–845) |
| `coverage.md` | Reserved-Word Coverage, Value-Type Coverage (207–280) |
| `harness.md` | Harness, C++ Interface, Fixes Applied This Round, Current Round Summary (281–354) |
| `classes.md` | Class Implementation Details, user-defined classes conformance (739–817) |
| `history.md` | Rounds 7–29 (355–738) — append-only, never edited in place |

Per AGENTS.md, each compliance module tracks the SHA256 of the **specific
spec module** it describes (not the whole SPEC.md).

## 3. Legacy root files

After extraction, reduce `SPEC.md` and `SPEC_COMPLIANCE.md` to ~10-line
stubs: title, "migrated to docs/spec/ / docs/compliance/", last-audit date.
(Deletion is an option — stubs are the default; they keep old links and
grep habits working.)

## 4. Entry points & cross-refs

- `docs/spec/README.md` and `docs/compliance/README.md` become the entry
  points (AGENTS.md rule).
- Replace the placeholder `docs/README` with a 10-line index linking
  `spec/`, `compliance/`, `plans/`, and the existing standalone docs.
- AGENTS.md "Session start" updated to point at the two READMEs
  (**requires explicit user permission** — AGENTS.md self-rule).

## 5. Steps (each independently committable)

1. Extract `docs/spec/` modules + README (verbatim cut, hash table).
2. Extract `docs/compliance/` modules + README (verbatim cut, per-module spec hashes).
3. Stub the two root files.
4. Fill `docs/README`; update AGENTS.md pointer (with permission).

Commits: `docs: split SPEC.md into docs/spec/ modules` ·
`docs: split SPEC_COMPLIANCE.md into docs/compliance/ modules` ·
`docs: stub legacy root spec files` ·
`docs: point AGENTS.md at docs/ entry points`

## 6. Verification

- `cat docs/spec/0*.md | wc -l` + README ≈ 1712 (same for compliance ≈ 845).
- Spot-diff a few sections between root file and modules (must be verbatim).
- `sha256sum` recorded in each README matches files at commit time.
- No code/test changes → no build needed; nothing in `src/` references the
  doc files.

## Out of scope

- Rewriting/restructuring spec text (heading-level oddities like `# append`
  stay as-is; fix in a separate pass if wanted).
- Merging or reordering compliance round history.
- Automated hash tooling — `sha256sum` by hand at each commit.
