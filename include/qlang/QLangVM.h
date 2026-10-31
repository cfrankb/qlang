#pragma once

#include <deque>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangBytecode.h"
#include "QLangCompiler.h"
#include "QLangErrors.h"
#include "QLangValue.h"

namespace qlang {

class QLangContext; // owns the variable scope the VM reads/writes

// Register-based executor over a CompiledProgram. All evaluation happens in a
// flat register file; the scope is synced in at start (to seed any
// pre-existing variables) and out at the end (to capture final state).
class QLangVM {
public:
  QLangVM(const CompiledProgram &program, QLangContext &scope);

  void run();

  // Call a QLang or native function by index from within the VM. Creates a
  // host caller frame, pushes the callee, runs until it returns, then pops
  // the host frame and returns the result.
  QLangValue callFunction(int funcIndex, const std::vector<QLangValue> &args);

  // Run the VM loop starting from the given frame index until the frame
  // pointer drops below startFrame. Used by both run() and callFunction().
  void runFrames(int startFrame);

  // Number of registers allocated (for debugging / assertions).
  int registerCount() const { return static_cast<int>(m_registers.size()); }

  // Throw a QLangError carrying the current pc's line/col, the owning module
  // name, and the full frame chain (innermost first) — SPEC # error
  // reporting. Every VM throw site goes through one of these two.
  [[noreturn]] void vmThrow(QLangErrorKind kind, const std::string &msg) const;
  [[noreturn]] void vmThrowAt(QLangErrorKind kind, int line, int col,
                              const std::string &msg) const;

private:
  // One activation frame. `regs` is the frame's register file; `captures` is
  // this function's explicit capture table (pointers into enclosing frames);
  // `pc` is the instruction pointer within the function's stream.
  struct Frame {
    int funcIndex = 0;
    std::vector<QLangValue> regs;
    std::vector<CaptureRef> captures;
    size_t pc = 0;
    // Caller register to receive this frame's return value (>= 0), or -1 for
    // the top-level main frame / a bare call used only for side effects.
    int callDest = -1;
    // Generator exhaustion flag: once a generator returns nil (fell off the
    // end), this stays true so every subsequent resume returns nil instead of
    // restarting the body from scratch.
    bool exhausted = false;
    // Shared comparison flags set by CMP/CMPI (bitmask of CMP_GR/CMP_EQ/
    // CMP_LT) and consumed by the JE/JNE/JG/JGE/JL/JLE jumps. Wiped on every
    // CMP/CMPI. Lives in the frame (not the VM) so a nested call gets a clean
    // flag state and the compiler's fused cmp+j* pair is the only writer.
    int flags = 0;
  };

  static bool isTrue(const QLangValue &v);
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

  // Call a closure value (Function type) with the given arguments.
  // Extracts the funcIndex from the closure and invokes callFunction.
public:
  QLangValue callClosure(const QLangValue &closure, const std::vector<QLangValue> &args);

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

  // Push a callee frame, copying args into its parameter registers, and advance
  // the frame pointer onto it. `dest` is the caller register holding the
  // closure (for CALLCLOS) / the call destination (for CALLFN).
  void pushCall(int calleeIndex, std::vector<QLangValue> &callerRegs,
                int dest, int argc);

  const CompiledProgram &m_prog;
  QLangContext &m_scope;
  std::vector<QLangValue> m_registers;
  // Frame stack. A deque keeps pointers to frames stable so captured-by-
  // reference variables remain valid across calls. `m_cur` indexes the live
  // frame; CALLFN/CALLCLOS push and advance it, RETURN pops and decrements it.
  std::deque<Frame> m_frames;
  int m_cur = -1;
  QLangValue m_returnValue; // result handed back to the caller of a call
  std::unordered_set<int> m_deleted; // registers removed by `delete`
  size_t m_steps = 0;
  // Re-entrancy guard for displayString: an `__str__` that displays `self`
  // must not recurse — the nested call skips `__str__` and falls back.
  bool m_inDisplayString = false;
  // Saved frames for active generator instances, keyed by function index.
  // Each entry is the most-recently-saved frame for that generator; a new
  // call with an empty entry creates a fresh instance.
  std::unordered_map<int, Frame> m_generatorFrames;

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
