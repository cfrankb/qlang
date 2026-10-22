#pragma once

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangBytecode.h"
#include "QLangCompiler.h"
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
  };

  static bool isTrue(const QLangValue &v);
  static QLangValue doBinary(Opcode op, const QLangValue &a, const QLangValue &b);
  static QLangValue doUnary(Opcode op, const QLangValue &a);

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

  static constexpr std::size_t kMaxSteps = 100000000u; // infinite-loop guard
};

} // namespace qlang
