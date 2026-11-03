#pragma once

#include <cstddef>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangBytecode.h"
#include "QLangCompiler.h"
#include "QLangErrors.h"
#include "QLangValue.h"

namespace qlang {

class QLangContext; // owns the variable scope the VM reads/writes

// Result of one executor pass over the top frame (Phase 1 single executor).
// Returned: the top frame RETURNed (it was popped; the value is in
//           m_returnValue).
// Yielded:  a generator frame YIELDed (its state was moved into the
//           generator instance in the caller's dest register; the yielded
//           value is in m_returnValue).
enum class RunResult { Returned, Yielded };

// Register-based executor over a CompiledProgram (Phase 1 substrate,
// Phase 2 global table). All evaluation happens in per-frame register
// files; captures are shared upvalue cells (Env) and generators are
// instances that own their saved frame. ONE dispatch loop (runFrames)
// serves run(), callFunction() and generator resumption. Top-level names
// live in the context's global table: the main frame's register file
// ALIASES the table's slots, so there is no scope sync in at start or out
// at end (or on throw).
class QLangVM {
public:
  QLangVM(const CompiledProgram &program, QLangContext &scope);

  void run();

  // Call a QLang function by index from within the VM (or from the host via
  // QLangContext::call). For a generator function this creates a fresh
  // instance and runs it to the first yield; for a plain function it runs
  // to the return.
  QLangValue callFunction(int funcIndex, const std::vector<QLangValue> &args);

  // Call a function value (Function or Generator type) with the given
  // arguments. A generator value resumes that instance.
  QLangValue callClosure(const QLangValue &value,
                         const std::vector<QLangValue> &args);

  // Number of registers in the main program (for debugging / assertions).
  int registerCount() const { return m_prog.registerCount; }

  // Throw a QLangError carrying the current pc's line/col, the owning module
  // name, and the full frame chain (innermost first) — SPEC # error
  // reporting. Every VM throw site goes through one of these two.
  [[noreturn]] void vmThrow(QLangErrorKind kind, const std::string &msg) const;
  [[noreturn]] void vmThrowAt(QLangErrorKind kind, int line, int col,
                              const std::string &msg) const;

private:
  // A frame's register file (Phase 2). Normal frames own their storage;
  // the MAIN frame aliases the context's global-table slots instead, so
  // top-level names live in the table directly — no sync in at run start,
  // no sync out at run end (or on throw). `operator[]`/`size()` make the
  // dispatch loop's `frame.regs[i]` sites work unchanged.
  struct RegFile {
    std::vector<QLangValue> owned;
    std::vector<QLangValue> *aliased = nullptr;
    std::vector<QLangValue> &vec() { return aliased ? *aliased : owned; }
    const std::vector<QLangValue> &vec() const { return aliased ? *aliased : owned; }
    size_t size() const { return vec().size(); }
    bool empty() const { return vec().empty(); }
    void resize(size_t n, const QLangValue &v = QLangValue()) { vec().resize(n, v); }
    QLangValue &operator[](size_t i) { return vec()[i]; }
    const QLangValue &operator[](size_t i) const { return vec()[i]; }
    // Move the owned storage out (generator suspend). Only generator frames
    // suspend, and they always own their storage (the main frame is the
    // sole alias and never yields).
    std::vector<QLangValue> takeOwned() {
      if (aliased)
        return {};
      return std::move(owned);
    }
  };

  // One activation frame. `regs` is the frame's register file (owned, or an
  // alias of the global table for the main frame); `captures` is this
  // function's bound capture table (shared cells); `envs` are the Envs this
  // frame created (their cells alias this frame's registers until the frame
  // pops, at which point the registers move into the cells).
  struct Frame {
    int funcIndex = 0;
    RegFile regs;
    std::vector<CaptureRef> captures;
    std::vector<EnvPtr> envs;
    size_t pc = 0;
    // Caller register to receive this frame's return value (>= 0), or -1 for
    // the top-level main frame / a bare call used only for side effects.
    int callDest = -1;
    // Shared comparison flags set by CMP/CMPI (bitmask of CMP_GR/CMP_EQ/
    // CMP_LT) and consumed by the JE/JNE/JG/JGE/JL/JLE jumps. Wiped on every
    // CMP/CMPI. Lives in the frame (not the VM) so a nested call gets a clean
    // flag state and the compiler's fused cmp+j* pair is the only writer.
    int flags = 0;
  };

  // `vm` (optional) lets the operand checks attach pc -> line/col + the frame
  // chain to the thrown error (SPEC # error reporting); null falls back to a
  // plain 0,0 error.
  static QLangValue doBinary(Opcode op, const QLangValue &a, const QLangValue &b,
                             QLangVM *vm = nullptr);
  static QLangValue doUnary(Opcode op, const QLangValue &a, QLangVM *vm = nullptr);

  // Comparison flag bits (SPEC §bytecodes: alternate conditional jump). The
  // three relations a>b, a==b, a<b are mutually exclusive and exhaustive, so
  // CMP/CMPI set exactly one bit: a>b -> GR, a==b -> EQ, a<b -> LT. The JGE /
  // JLE jumps test for "that bit or EQ" (a>=b / a<=b), never a combined
  // pattern.
  static constexpr int CMP_GR = 1;
  static constexpr int CMP_EQ = 2;
  static constexpr int CMP_LT = 4;
  // Set the flags from two values; Int operands only (throws otherwise).
  // The assignment at the call site is the "wipe" — only the matching bits
  // are set here.
  static int cmpFlags(const QLangValue &a, const QLangValue &b, QLangVM *vm);
  // True when the flag pattern `f` satisfies the conditional jump `op`.
  static bool flagsSatisfied(Opcode op, int f);

  // Truthiness of a value per SPEC §boolean evaluation: nil/eof false;
  // string/array/table/blob/set empty => false; number 0 => false;
  // generator true; function/lambda evaluated by its return value (the
  // value is called with no arguments, so it needs a live VM — hence a
  // member, not static).
  // (Public: the anonymous-namespace builtin helpers in QLangVM.cpp call
  // into these and the scope directly.)
public:
  bool isTrue(const QLangValue &v);

  // Display form of a value for print/println/printf/f-string/join: a table
  // with `__str__` calls it (re-entrancy guarded); class instances without
  // `__str__` render as their class name; everything else uses the generic
  // text form.
  std::string displayString(const QLangValue &v);
  // Implicit __str__ rendering of a value inside a collection (SPEC
  // `# implicit __str__ rules`): strings double-quoted, nested collections
  // recursive, scalars in plain display form.
  std::string renderElement(const QLangValue &v);

  // EQ/NE with object dispatch (spec §operator == and !=): a string compared
  // with an object (a table carrying `__str__` or the `__class__` meta key)
  // is equal only when the object's `__str__` value matches the string; all
  // other opcode/operand combinations delegate to the static doBinary.
  QLangValue doBinaryVm(Opcode op, const QLangValue &a, const QLangValue &b);
  bool valueEqual(const QLangValue &a, const QLangValue &b);

  // Push a callee frame, copying args into its parameter registers, and
  // advance the frame pointer onto it. Args are read from caller registers
  // `argBase+1 .. argBase+argc`; the call result lands in caller register
  // `resultDest` (usually argBase, the callee's home register — they differ
  // when the callee value was hoisted to a temp to avoid clobbering a
  // capture-aliased register).
  void pushCall(int calleeIndex, RegFile &callerRegs,
                int resultDest, int argBase, int argc);

  const CompiledProgram &m_prog;
  QLangContext &m_scope;
  // Frame stack. A deque keeps pointers to frames stable so that cell owner
  // links (Env::owner) remain valid while the owning frame is live.
  // `m_cur` indexes the live frame; CALLFN/CALLCLOS push and advance it,
  // RETURN pops and decrements it.
  std::deque<Frame> m_frames;
  int m_cur = -1;
  QLangValue m_returnValue; // result handed back to the caller of a call
  size_t m_steps = 0;
  // Re-entrancy guard for displayString: an `__str__` that displays `self`
  // must not recurse — the nested call skips `__str__` and falls back.
  bool m_inDisplayString = false;
  // Prototype closure per function index, registered by MAKEFUNC. Def
  // statements materialize their closure at def time; the map lets host-
  // driven calls (callFunction) and CALLFN reach the def-time cell bindings.
  std::unordered_map<int, ClosurePtr> m_funcClosures;
  // Suspended generator state stashed by YIELD when the generator frame has
  // no caller dest register holding its instance (host-driven
  // resumeGenerator); consumed by resumeGenerator() on the next Yielded.
  std::optional<GeneratorState> m_suspended;

  // The single dispatch loop (Phase 1): runs the top frame
  // (m_frames[m_cur]) until it RETURNs (popping it) or a generator frame
  // YIELDs (saving its state into the instance). A step-limit violation
  // throws QLangError.
  RunResult runFrames();

  // Pop the top frame, releasing its capture aliases into the owning Envs
  // first.
  void popFrame();

  // Bind a compile-time capture spec (env==null, reg set) to live cells;
  // appends the bound refs to `out`. Two kinds (Phase 2):
  //  - GLOBAL: the capture name is a top-level name (in m_prog.vars) — the
  //    bound ref aliases the context's table slot directly
  //    (CaptureRef{nullptr, slot, -1}), so it stays live across frames,
  //    VM instances and run() boundaries (fixes S2/P3).
  //  - LOCAL: a fresh Env cell seeded — and live-aliased — from `frame`
  //    (nullptr when there is no creating frame: nil cells, so
  //    REFLOAD/REFSTORE stay safe).
  void bindCaptures(Frame *frame, const Function &fn,
                    const std::vector<CaptureRef> &spec,
                    std::vector<CaptureRef> &out);

  // Live cell access: a global capture (env==null) aliases the context's
  // table slot; a local capture aliases the owning frame's register while
  // it is alive, and is the stored value after the frame pops.
  QLangValue &cellAt(const CaptureRef &cr);

  // Move a frame's captured registers into its Envs and clear their owner
  // links. Called on every frame pop (RETURN, fall-off end, YIELD) and, via
  // releaseAllCaptures, on exception unwind so no Env outlives a dead frame.
  void releaseFrame(Frame &frame);
  void releaseAllCaptures();

  // Reconcile a standalone-compiled program's main-program name->register
  // map with the context's global table (Phase 2): names the table does not
  // know get adopted at their program register; a name already in the table
  // at a different slot keeps its value, relocated to the program register.
  // No-op for programs compiled against this context (registers already are
  // table slots).
  void adoptGlobals(const CompiledProgram &prog);

  // Resume a generator instance to its next yield (or exhaustion): moves the
  // saved registers onto a live frame, runs the single loop, and moves the
  // frame back. Returns the yielded value, or nil when exhausted.
  QLangValue resumeGenerator(GeneratorState &gen);

  // Default infinite-loop guard. The live limit is owned by the context
  // (QLangContext::maxSteps), which the VM reads at every step; this constant
  // is the default seed for it. Override at runtime via set_max_steps(n)
  // (QLang) or QLangContext::setMaxSteps(n) (C++).
  static constexpr std::size_t kMaxSteps = 100000u;

  // Gated diagnostic emit. Prints printf-style output to stderr only when the
  // context's verbose level is >= minLevel. Program output (print/println) is
  // unaffected. Tiers: 1 = frame/call events, 2 = VM loop + opcodes,
  // 3 = full detail (generators, table ops).
  template <typename... Args>
  void trace(int minLevel, const char *fmt, Args &&...args) const {
    if (!m_scope.isAt(minLevel)) return;
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    if (n > 0) std::fputs(buf, stderr);
  }
};

} // namespace qlang
