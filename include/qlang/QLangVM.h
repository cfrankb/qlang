#pragma once

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

  // Number of registers allocated (for debugging / assertions).
  int registerCount() const { return static_cast<int>(m_registers.size()); }

private:
  static bool isTrue(const QLangValue &v);
  static QLangValue doBinary(Opcode op, const QLangValue &a, const QLangValue &b);
  static QLangValue doUnary(Opcode op, const QLangValue &a);

  const CompiledProgram &m_prog;
  QLangContext &m_scope;
  std::vector<QLangValue> m_registers;
  std::unordered_set<int> m_deleted; // registers removed by `delete`
  size_t m_steps = 0;

  static constexpr std::size_t kMaxSteps = 100000000u; // infinite-loop guard
};

} // namespace qlang
