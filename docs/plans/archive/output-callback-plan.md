# Plan: output callback — one sink for every message the engine emits

Status: **implemented** (2026-11-13, Round 85) — P1 (engine + suite +
demo) and P2 (spec finalize + compliance close-out) landed; see
`docs/compliance/history.md` Round 85. D4/D7 deviated: `callBuiltin` already
has `scope` + `line`, no new VM accessors.
Spec: `docs/spec/07-api.md` § "output callback (universal)" (hash
`086f8502…`, recorded in `docs/spec/README.md`); finalize the section with the
implementation round and re-record the hash (`doc_hashes` test).
Related: `docs/spec/07-api.md` § "verbose level" (the trace gating the callback
does *not* change), `docs/spec/12-errors-trace.md` (error report stays an
exception), § "external search callback" (the callback pattern this copies).

## Goal

Every byte the library writes as a message — program output (`print`,
`println`, `printf`), verbose trace, and any future warning/log — flows through
**one** `QLangContext::output()` call. A host-assigned callback receives
`(message, severity, file, line)` and can route it to a message manager (e.g.
a Qt6 widget). **No callback assigned: behavior is byte-identical to today**
(program output → `stdout`, diagnostics → `stderr`).

## Current state (explored 2026-11-13)

The engine has exactly **six** output sites; there are no raw
`fprintf(stderr,…)`/`fputs` calls left in `src/` — every stderr byte comes
from one of the three `trace()` templates:

| Channel | Site | Sink today |
|---|---|---|
| `print` | `src/QLangVM.cpp:569` (`callBuiltin`) | `std::cout` |
| `println` | `src/QLangVM.cpp:573` | `std::cout` |
| `printf` | `src/QLangVM.cpp:614` | `std::cout` |
| VM trace (tiers 1–3) | `include/qlang/QLangVM.h:323` `trace()` | `std::fputs(stderr)` |
| compiler diagnostics (`[DBG-*]`) | `include/qlang/QLangCompiler.h:343` `trace()` | `std::fputs(stderr)` |
| context diagnostics (`[MODULE]`) | `include/qlang/QLangContext.h:192` `trace()` | `std::fputs(stderr)` |

Errors (`QLangError`) are **thrown**, never printed, by the library — spec 12
puts the report format at the *top-level catch* (host side). The `qlcc`/
`qlang_run` drivers' own `fprintf(stderr,…)` calls are host code and out of
scope.

## Design decisions

| # | Topic | Decision | Rationale / consequence |
|---|-------|----------|-------------------------|
| **D1** | API shape | On `QLangContext`, mirroring `ModuleSearchCallback` (`QLangContext.h:298`):<br>`using OutputCallback = std::function<void(const std::string &message, int level, const std::string &file, int line)>;`<br>`void setOutputCallback(OutputCallback);` (assigning `nullptr` restores default routing)<br>`const OutputCallback &outputCallback() const;` | The spec prototype's `[optional]` params become the sentinel convention `file == ""` / `line == 0` (a `std::function` has no default arguments). The spec's `string &message` is refined to `const std::string &` — no receiver needs to mutate it. |
| **D2** | Severity | Enum in `QLangContext` beside `TRACE_*` (`QLangContext.h:37`): `enum { OUT_TRACE, OUT_DEBUG, OUT_INFO, OUT_WARNING, OUT_ERROR, OUT_FATAL };` (0–5, per the spec table). | Same anonymous-enum style as the trace tiers; the spec table is the contract. |
| **D3** | The single machinery | `void QLangContext::output(int level, const std::string &message, const std::string &file = "", int line = 0) const;` — **public** (hosts and future engine warnings route through it too). Body: callback set → invoke it and stop; else default route by level: `OUT_INFO` → `std::cout << message`, every other level → `std::fputs(stderr)`. | Redirect is **exclusive**: with a callback, nothing reaches stdout/stderr from these sites. Default route per channel keeps the no-callback bytes identical to today's `std::cout <<` / `fputs` writes. |
| **D4** | Site conversion | The three `trace()` templates format into their 2048 buffer as today, then call `output(OUT_TRACE, buf)` instead of `fputs` (context: `this->output`; VM: `m_scope.output`; compiler: `m_scope->output`, standalone compiler stays silent as today). The three print sites in `callBuiltin` become `vm->context().output(OUT_INFO, text, moduleName, currentLine)`. | Gating (`isAt(tier)`) and formatting are untouched — only the sink changes. A standalone `QLangCompiler` has no context and therefore no configuration; it already emits nothing (D3 note, `QLangCompiler.h:352`). |
| **D5** | Trace severity | **All** verbose trace (`TRACE_FRAME`/`TRACE_VM_LOOP`/`TRACE_FULL`, `[DBG-*]`) is delivered at `OUT_TRACE`. | The verbose level is *gating*, not severity; a message manager filters by level, the tier already decided what is interesting. One mapping, no invented precision. `OUT_DEBUG`/`OUT_WARNING` stay unused by the engine today — `output()` is public so the host and future warnings fill them. |
| **D6** | Program-output text | One callback call per `print`/`println`/`printf` call, message = the exact text that would have gone to stdout (println's includes its `\n`). No engine-side line buffering. | Fewest rules; a widget that wants line assembly buffers on its side. `ponytail:` engine-side line buffering is the upgrade if a host ever needs it. |
| **D7** | file/line for program output | `file` = `m_scope.moduleName()` (module-name convention shared with `QLangError`, `QLangVM.cpp:1066`); `line` = current instruction's line via a new `QLangVM::currentLine()` — the same `instructionIndexAt(fn, fr.pc)` + `fn.loc[idx].first` lookup `vmThrowAt` already does (`QLangVM.cpp:1049-1052`). Trace lines carry `""`/`0`. | Reuses existing pc→line plumbing; costs one array index per print call. |
| **D8** | Throw contract | The callback must not throw. The engine wraps the call in `try { … } catch (...) {}` and swallows — the module-search callback (`QLangContext.cpp:811-827`) reports a throwing callback *through its `error` out-param*; output has no channel that is not the same broken callback, so silence is the only non-recursive option. | Consistent host-contract enforcement; the `catch` is one line. |
| **D9** | Errors stay exceptions | `QLangError` is **not** converted to callback traffic. `OUT_ERROR`/`OUT_FATAL` are reserved for `output()` callers (host, future engine warnings). Hosts that want error reports in the same message manager call `QLangError::formatReport(e)` and feed it to their sink themselves. | Spec 12 puts report formatting at the top-level catch; swallowing exceptions into a fire-and-forget sink would break `run()`'s documented `throws QLangError` contract. |
| **D10** | Lifecycle & threading | The callback is **host configuration, not program state**: it survives `clear()`, exactly like the search paths and the search callback (comment at `QLangContext::clear`, `QLangContext.cpp:1092`). It fires synchronously on the thread driving the context — the "one context, one thread" contract (`07-api.md` § threading) already covers it; nothing new. | Hosts do not re-arm it after `clear()`. |

## Impact assessment

| Area | File(s) | Change | Risk |
|---|---|---|---|
| API | `include/qlang/QLangContext.h`, `src/QLangContext.cpp` | `OutputCallback` + setter/getter + `output()` + severity enum; `setOutputCallback` calls `bindThread()` like the search-callback setter | Low |
| Trace sites | `include/qlang/QLangContext.h`, `include/qlang/QLangVM.h`, `include/qlang/QLangCompiler.h` | `trace()` bodies: `fputs(buf, stderr)` → `output(OUT_TRACE, buf)` (3 lines) | Low — gate and format untouched |
| Print sites | `src/QLangVM.cpp` | 3 `std::cout` sites → `context().output(OUT_INFO, …)`; new `currentLine()` helper; new public `QLangContext &context() const { return m_scope; }` on `QLangVM` (no accessor exists today) | Low |
| Tests | **new** `tests/unit/output_callback_test.cpp` + `add_executable`/`add_test(NAME output_callback …)` in `CMakeLists.txt` | see Test plan | — |
| Demo | **new** `examples/output_callback.cpp` (target `output_callback`), mirroring `examples/module_search.cpp` for the search callback | tiny: capture into a `vector`, print the captured table | — |
| Docs | `docs/spec/07-api.md` § "output callback (universal)" finalized with the round (defaults, exclusive redirect, `""`/`0` sentinels, survives `clear()`); re-record hash in `docs/spec/README.md`; compliance open item flipped to verified per AGENTS.md | — |

Not affected: lexer/parser/compiler/bytecode (no syntax, no opcodes), `.qlc`
format, verbose-level semantics, `QLangError` propagation, the `qlcc`/
`qlang_run` drivers (host code keeps its own `stderr` reports).

## Phases

### P1 — core (one compliance round)

1. `QLangContext`: severity enum, `OutputCallback`, `setOutputCallback` /
   `outputCallback()`, `output()` (D1–D3), member
   `OutputCallback m_outputCallback;` beside `m_moduleSearchCallback`
   (`QLangContext.h:464`); survives `clear()` (extend the existing comment).
2. `QLangVM`: `context()` accessor, `currentLine()` (D7), reroute the three
   `trace()` sites and the three print sites (D4).
3. `QLangCompiler::trace()` reroute (D4).
4. Test `output_callback_test` (below); demo; build
   `cmake -B build && cmake --build build`, run `ctest --test-dir build`.

### P2 — docs close-out

Spec 07 section finalized against the shipped API, hash re-recorded in
`docs/spec/README.md`, compliance entry flipped to verified (per AGENTS.md:
test first, then hash). Move this plan to `docs/plans/archive/`.

## Test plan (`tests/unit/output_callback_test.cpp`)

1. **Default routing** (no callback): capture stdout — `println`/`printf`
   bytes identical to today's stream; `setVerboseLevel(1)` trace lines still
   land on stderr (capture or simply assert non-crash + callback tests cover
   routing).
2. **Redirect is exclusive**: callback set, run
   `println("hi"); printf("%d", 42)` → captured exactly `hi\n` and `42`,
   nothing on stdout.
3. **Attributes**: message carries `level == OUT_INFO`, `file == "main"`,
   `line > 0` at the `println` statement's line.
4. **Trace routing**: `setVerboseLevel(2)` + callback → messages arrive with
   `level == OUT_TRACE`, nothing on stderr.
5. **Gating unchanged**: level 0 → callback receives no trace messages.
6. **Contract**: assigning `nullptr` restores defaults; a throwing callback
   does not escape (program keeps running).
7. **Survives `clear()`**: callback still fires after `ctx.clear()`.

## Out of scope (named, not promised)

- Engine-side line buffering of program output into whole-line messages (D6).
- Routing `QLangError` reports through the callback (D9) — host-side bridge is
  one `formatReport` call away.
- Per-thread/process-global sink; multiple callbacks per channel; QLang-level
  (`log(...)`) builtins emitting at `OUT_INFO`/`OUT_WARNING` — add when a
  script-side logging need is real.
