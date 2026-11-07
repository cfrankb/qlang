// Example driver for the QLang scripting-language skeleton.
//
// Parses a program written in the QLang grammar (see spec.md) through a
// QLangContext, walks the AST, compiles it to register-machine bytecode and
// dumps it, then runs it through the VM and reports final variable values.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangModules.h"
#include "qlang/QLangParser.h"

using namespace qlang;

static void printExpr(const ExprPtr &e, int indent);
static void printStmt(const StmtPtr &s, int indent);
static void dumpBytecode(const CompiledProgram &cp);

static const char *indentStr(int n)
{
  static char buf[64] = {0};
  int n2 = n * 2;
  if (n2 > 60)
    n2 = 60;
  for (int i = 0; i < n2; i++)
    buf[i] = ' ';
  buf[n2] = '\0';
  return buf;
}

static void printExpr(const ExprPtr &e, int indent)
{
  if (!e)
  {
    std::cout << indentStr(indent) << "(null expr)\n";
    return;
  }
  switch (e->kind)
  {
  case ExprKind::Literal:
    std::cout << indentStr(indent) << "literal: " << e->literal.typeName()
              << "\n";
    break;
  case ExprKind::Variable:
    std::cout << indentStr(indent) << "var: " << e->name << "\n";
    break;
  case ExprKind::Binary:
    std::cout << indentStr(indent) << "binop " << e->op.text << "\n";
    printExpr(e->left, indent + 1);
    printExpr(e->right, indent + 1);
    break;
  case ExprKind::Unary:
    std::cout << indentStr(indent) << "unary " << e->uop.text << "\n";
    printExpr(e->operand, indent + 1);
    break;
  }
}

static void printStmt(const StmtPtr &s, int indent)
{
  if (!s)
    return;
  switch (s->kind)
  {
  case StmtKind::Declaration:
    std::cout << indentStr(indent) << "local " << s->name;
    if (s->declaredType)
    {
      QLangValue tmp(*s->declaredType);
      std::cout << ":" << tmp.typeName();
    }
    std::cout << " = ";
    printExpr(s->value, indent + 1);
    break;
  case StmtKind::Assignment:
    std::cout << indentStr(indent) << s->name << " " << s->assignOp.text
              << " = ";
    printExpr(s->assignValue, indent + 1);
    break;
  case StmtKind::For:
    std::cout << indentStr(indent) << "for " << s->loopVar << " = ";
    printExpr(s->forStart, indent + 1);
    std::cout << indentStr(indent + 1) << "to\n";
    printExpr(s->forEnd, indent + 1);
    for (auto &st : s->body)
      printStmt(st, indent + 1);
    break;
  case StmtKind::While:
    std::cout << indentStr(indent) << "while\n";
    printExpr(s->cond, indent + 1);
    for (auto &st : s->whileBody)
      printStmt(st, indent + 1);
    break;
  case StmtKind::If:
    std::cout << indentStr(indent) << "if\n";
    printExpr(s->ifCond, indent + 1);
    for (auto &st : s->ifBody)
      printStmt(st, indent + 1);
    break;
  case StmtKind::ExprStmt:
    std::cout << indentStr(indent) << "expr\n";
    printExpr(s->value, indent + 1);
    break;
  }
}

// The immediate right operand of a decoded instruction as text: the payload
// says which field carries it and the form what it is (the 2/2/4 modifier byte,
// docs/spec/13-bytecodes.md `### bytecode encoding`).
static std::string immText(const DecodedInstruction &ins)
{
  switch (ins.immPayload)
  {
  case ImmPayload::Signed:
    return std::to_string(ins.imm);
  case ImmPayload::Unsigned:
    return std::to_string(ins.immUnsigned);
  case ImmPayload::Float:
  {
    // Shortest decimal text that reads back as the same double.
    char b[40];
    for (int prec = 1; prec <= 17; prec++)
    {
      std::snprintf(b, sizeof b, "%.*g", prec, ins.immFloat);
      if (std::strtod(b, nullptr) == ins.immFloat)
        break;
    }
    return std::string(b);
  }
  case ImmPayload::True:
    return "true";
  case ImmPayload::False:
    return "false";
  case ImmPayload::Nil:
    return "nil";
  case ImmPayload::None:
    break;
  }
  return "?";
}

static void dumpBytecode(const CompiledProgram &cp)
{
  if (cp.functions.empty())
  {
    std::cout << "\nBytecode (no functions):\n";
    return;
  }
  const auto &fn = cp.functions[0]; // main
  const auto &consts = fn.constants;
  std::cout << "\nBytecode (" << fn.instrOffsets.size() << " instructions, "
            << cp.registerCount << " registers, " << consts.size()
            << " constants):\n";
  for (size_t i = 0; i < fn.instrOffsets.size(); i++)
  {
    DecodedInstruction ins;
    decodeInstruction(fn.code.data(), fn.instrOffsets[i], fn.code.size(), ins);
    std::cout << "  " << i << ": " << Bytecode::name(ins.op);
    if (isJumpOpcode(ins.op))
    {
      std::cout << " -> " << ins.jumpOffset;
    }
    else if (baseOperandCount(ins.op) > 0)
    {
      std::cout << " r[" << ins.a << "]";
      if (ins.op == Opcode::CALL)
      {
        // b is the signed function id (negative = native call), c the
        // argument count; neither is read through the kind.
        std::cout << " fn[" << ins.b << "] r[" << ins.c << "]";
      }
      else
      {
        size_t nOps = baseOperandCount(ins.op);
        if (nOps >= 2)
        {
          switch (ins.rightKind)
          {
          case RightKind::Immediate:
            std::cout << " imm=" << immText(ins);
            break;
          case RightKind::ConstPool:
            std::cout << " = const[" << ins.b << "]";
            if (ins.b >= 0 && static_cast<size_t>(ins.b) < consts.size())
              std::cout << " (" << consts[ins.b].typeName() << ")";
            break;
          case RightKind::Register:
          case RightKind::Reserved:
            std::cout << " r[" << ins.b << "]";
            break;
          }
        }
        if (nOps == 3 && !isInPlaceForm(ins.op, ins.rightKind))
          std::cout << " r[" << ins.c << "]";
      }
    }
    std::cout << "\n";
  }
  std::cout << "\nConstants:\n";
  for (size_t i = 0; i < consts.size(); i++)
    std::cout << "  [" << i << "] " << consts[i].typeName() << "\n";
}

int main()
{
  // Program mirroring the grammar in spec.md.
  std::string src = R"(
local a = 32                 # int
local b:byte = 32            # byte
local s = "hello"            # string
local sum = 0                # int

a += 12                      # compound assignment -> 44
s += " world"                # string append

println(s)

for i = 1 to 5               # for auto-increments i, inclusive
  sum = sum + i
next i

if a == 44 then
  s += " ok"
end

local n = 3
while (n > 0)                 # while with manual decrement
  n = n - 1
end

printf("Louis is TOTO\n")

local hello = (name)[] {
  printf("hello %s\n", name)
  return true
}

hello("Sonia")

for i in [1,2,3,4] {
  printf("coundown: %d\n",i)
}

for c in "string" {
   printf("char: %c\n", c)
}

aa = 5
if (aa==4) {

} else if aa == 5{
  println("ELSE")
}

local events:table = {
  onLoad = (name)[] {
    printf("onLOaD -->%s\n", name);
  },
  onDeath = ()[] {
  }
}

events.onLoad("Tam Tam")


local human = {
  name = "Denise",
  age = 65
}

printf("human: %s age=%d\n", human.name, human.age)

printf("space %d\n", 0x20)

println([1,2,3])
ar = [1,2,3]
ar += [4,5]
for i in ar {
  printf("%d", i)
}
println("")

printf("%s --> len %d\n", join(",", ar), len(ar))

println(sprintf("Today is %s %d", "monday", 2026))

println(f"Hello {human.name}. Formatted")

local x = 1024 / 10
println(f"1024 / 10 = {x}")  # 102
local x = 1024 % 10
println(f"1024 %% 10 = {x}") # 4

local x = 0x678 & 0x70
println(f"0x678 & 0x70 = {x:#04x}") #  expected: 0x678 & 0x70 = 0x0070
println(f"{255:#06x}") # expected: "0x00ff"

#assert(1==0, "this is TRUE")

println("this is the END")

import math

local mx = max(3, 7, -1)
local mn = min(3, 7, -9)
local sm = sum(1, 2, 3)
println(sprintf("max= %d, min=%d, sum=%d", mx, mn, sm))

)";

  QLangContext ctx;
  ctx.setVerboseLevel(qlang::TRACE_OFF);
  try
  {
    std::vector<StmtPtr> program = ctx.parse(src);
    std::cout << "Parsed " << program.size() << " statements.\n";
    for (auto &st : program)
      printStmt(st, 0);

    QLangCompiler compiler(program);
    compiler.addModules(ctx.modules());
    CompiledProgram cp = compiler.compile();
    dumpBytecode(cp);
  }
  catch (const QLangError &e)
  {
    std::cerr << e.what() << "\n";
    return 1;
  }

  try
  {
    ctx.run(src);
    std::cout << "\nAfter run:\n";
    std::cout << "  a    = " << ctx.get("a").asInt() << "\n";
    std::cout << "  s    = \"" << ctx.get("s").asString() << "\"\n";
    std::cout << "  sum  = " << ctx.get("sum").asInt() << "\n";
    std::cout << "  n    = " << ctx.get("n").asInt() << "\n";

    // Self-check: the VM must reproduce these results.
    bool ok = ctx.get("a").asInt() == 44 &&
              ctx.get("s").asString() == "hello world ok" &&
              ctx.get("sum").asInt() == 15 && ctx.get("n").asInt() == 0;
    std::cout << "\nVM result: " << (ok ? "OK" : "MISMATCH") << "\n";
    if (!ok)
      return 1;
  }
  catch (const QLangError &e)
  {
    std::cerr << e.what() << "\n";
    return 1;
  }

  return 0;
}
