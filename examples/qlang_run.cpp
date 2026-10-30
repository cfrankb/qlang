// qlang_run — small driver that runs a .ql (QLang) script with a selectable
// diagnostic trace level, optional stage timing, and bytecode output.
//
//   qlang_run [--trace-level <0-3>] [--profile] [--output <file.qlc>]
//             [--asm <file.qla>] [--asm-level <0-4>] file.ql
//
// The app drives the pipeline directly (parse -> compile -> execute) so it
// can time each stage and hand the compiled program to the .qlc writer.
// Trace and profile output go to stderr; the script's own program output
// (print/println) goes to stdout and is never affected.
//
// Exit codes:
//   0  script ran to completion
//   1  usage error (bad/missing arguments)
//   2  script file could not be read
//   3  QLang error (lex/parse/type/runtime)
//   4  bytecode file could not be written
//   5  disassembly file could not be written

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include <qlang/QLangBytecodeIO.h>
#include <qlang/QLangCompiler.h>
#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>
#include <qlang/QLangVM.h>

using namespace qlang;

namespace
{

  const char *kAppName = "qlang_run";

  using Clock = std::chrono::steady_clock;

  double msBetween(Clock::time_point a, Clock::time_point b)
  {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

  void printHelp(const char *prog)
  {
    std::cout << "Run a QLang (.ql) script.\n"
              << "\n"
              << "usage: " << prog << " [options] <file.ql>\n"
              << "\n"
              << "options:\n"
              << "  --trace-level <n>   diagnostic verbosity (stderr):\n"
              << "                        0 = off (default)\n"
              << "                        1 = frame/call events\n"
              << "                        2 = VM loop + opcodes + returns\n"
              << "                        3 = full detail (generators, table ops, compile)\n"
              << "  --profile           report parse / compile / execute times in ms (stderr)\n"
              << "  --output <file.qlc> write the compiled bytecode to <file.qlc>\n"
              << "  --asm <file.qla>    write a disassembly listing to <file.qla>\n"
              << "  --asm-level <n>     disassembly detail (default 1):\n"
              << "                        0 = instructions only\n"
              << "                        1 = instructions + constants + function info\n"
              << "                        2 = level 1 + interleaved source lines\n"
              << "                        3 = level 2 + const values inlined next to instructions\n"
              << "                        4 = level 3 + register map, operand names, pseudo-mnemonics\n"
              << "  -h, --help          show this help and exit\n"
              << "\n"
              << "exit codes:\n"
              << "  0  script ran to completion\n"
              << "  1  usage error\n"
              << "  2  script file could not be read\n"
              << "  3  QLang error (lex/parse/type/runtime)\n"
              << "  4  bytecode file could not be written\n"
              << "  5  disassembly file could not be written\n";
  }

  // Accept exactly "0".."3". Returns false on failure.
  bool parseTraceLevel(const std::string &s, int &out)
  {
    if (s.size() != 1 || s[0] < '0' || s[0] > '3')
      return false;
    out = s[0] - '0';
    return true;
  }

  // Accept exactly "0".."4". Returns false on failure.
  bool parseAsmLevel(const std::string &s, int &out)
  {
    if (s.size() != 1 || s[0] < '0' || s[0] > '4')
      return false;
    out = s[0] - '0';
    return true;
  }

  // ---------------------------------------------------------------------------
  // .qlc binary output: the library codec (src/QLangBytecodeIO.cpp,
  // docs/SERIALIZATION.md). nativeIds maps native name -> negative opcode id
  // (compiler.nativeFuncIds()); the slot table is written in ascending slot
  // order and lets a loader re-bind natives by name.
  bool writeBytecode(const CompiledProgram &prog,
                     const std::map<std::string, int> &nativeIds,
                     const std::string &path, size_t &written)
  {
    std::string buf;
    std::ostringstream ss(std::ios::binary);
    if (!writeQlc(ss, prog, nativeIds))
      return false;
    buf = ss.str();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!out)
      return false;
    written = buf.size();
    return true;
  }

  // ---------------------------------------------------------------------------
  // Disassembly listing (.qla). Detail levels:
  //   0 = instruction stream only (mnemonic from Bytecode::name plus raw
  //       operands a/b/c and the stamped source line:col)
  //   1 = level 0 + function metadata (params/registers/captures) and the
  //       human-readable constant pool
  //   2 = level 1 + the original source line interleaved before each run of
  //       instructions that share a line
  //   3 = level 2 + the referenced constant's value inlined next to the
  //       instruction (CONST / MAKEFUNC / MAKEGEN, whose operand b is a pool
  //       index)
  //   4 = level 3 + the variable->register map (main function), operand names
  //       for CALL (builtin/native), CALLFN (function name), CONV/CAST (target
  //       type), and a pseudo-mnemonic: the register-level meaning of each
  //       instruction (e.g. `const 1 0 0` -> `r1 = pool[0]`)
  // ---------------------------------------------------------------------------

  // Builtin id (CALL operand b, >= 0) -> name. Order mirrors the Builtin enum
  // in QLangBytecode.h.
  const char *builtinName(int id)
  {
    switch (id)
    {
    case 0:
      return "tostr";
    case 1:
      return "toint";
    case 2:
      return "type";
    case 3:
      return "print";
    case 4:
      return "println";
    case 5:
      return "printf";
    case 6:
      return "len";
    case 7:
      return "blob";
    case 8:
      return "substr";
    case 9:
      return "slice";
    case 10:
      return "range";
    case 11:
      return "join";
    case 12:
      return "isset";
    case 13:
      return "sprintf";
    case 14:
      return "format";
    case 15:
      return "assert";
    case 16:
      return "next";
    case 17:
      return "set_max_steps";
    case 18:
      return "get_max_steps";
    case 19:
      return "set";
    default:
      return nullptr;
    }
  }

  // Pseudo-mnemonic: the register-level meaning of one instruction (level 4),
  // e.g. "r1 = pool[0]" for `const 1 0 0`.
  std::string pseudoText(const CompiledProgram &prog, const Instruction &ins)
  {
    std::ostringstream ss;
    auto rn = [](int r)
    { return std::string("r") + std::to_string(r); };
    auto args = [&ins](int a, int c)
    {
      std::string s;
      for (int i = 1; i <= c; i++)
      {
        if (i > 1)
          s += ", ";
        s += "r" + std::to_string(a + i);
      }
      return s;
    };
    auto type = [](int t)
    {
      return std::string(QLangValue(static_cast<QLangValue::Type>(t)).typeName());
    };
    auto bin = [&ss, &rn, &ins](const char *op)
    {
      ss << rn(ins.a) << " = " << rn(ins.b) << " " << op << " " << rn(ins.c);
    };
    auto cmp = [&ss, &rn, &ins](const char *op)
    {
      ss << rn(ins.a) << " = (" << rn(ins.b) << " " << op << " " << rn(ins.c) << ")";
    };
    switch (ins.op)
    {
    case Opcode::CONST:
      ss << rn(ins.a) << " = pool[" << ins.b << "]";
      break;
    case Opcode::MOV:
      ss << rn(ins.a) << " = " << rn(ins.b);
      break;
    case Opcode::CONV:
      ss << rn(ins.a) << " = convert(" << rn(ins.b) << ", " << type(ins.c) << ")";
      break;
    case Opcode::CAST:
      ss << rn(ins.a) << " = cast(" << rn(ins.b) << ", " << type(ins.c) << ")";
      break;
    case Opcode::NEG:
      ss << rn(ins.a) << " = -" << rn(ins.b);
      break;
    case Opcode::INC:
      ss << rn(ins.a) << " += 1";
      break;
    case Opcode::DEC:
      ss << rn(ins.a) << " -= 1";
      break;
    case Opcode::ADD:
      bin("+");
      break;
    case Opcode::SUB:
      bin("-");
      break;
    case Opcode::MUL:
      bin("*");
      break;
    case Opcode::DIV:
      bin("/");
      break;
    case Opcode::MOD:
      bin("%");
      break;
    case Opcode::AND:
      bin("&");
      break;
    case Opcode::OR:
      bin("|");
      break;
    case Opcode::XOR:
      bin("^");
      break;
    case Opcode::SHL:
      bin("<<");
      break;
    case Opcode::SHR:
      bin(">>");
      break;
    case Opcode::POW:
      bin("**");
      break;
    case Opcode::LAND:
      ss << rn(ins.a) << " = truth(" << rn(ins.b) << ") && truth(" << rn(ins.c) << ")";
      break;
    case Opcode::LOR:
      ss << rn(ins.a) << " = truth(" << rn(ins.b) << ") || truth(" << rn(ins.c) << ")";
      break;
    case Opcode::NOT:
      ss << rn(ins.a) << " = !truth(" << rn(ins.b) << ")";
      break;
    case Opcode::EQ:
      cmp("==");
      break;
    case Opcode::NE:
      cmp("!=");
      break;
    case Opcode::LT:
      cmp("<");
      break;
    case Opcode::GT:
      cmp(">");
      break;
    case Opcode::LE:
      cmp("<=");
      break;
    case Opcode::GE:
      cmp(">=");
      break;
    case Opcode::IN:
      ss << rn(ins.a) << " = (" << rn(ins.b) << " in " << rn(ins.c) << ")";
      break;
    case Opcode::CALL:
      if (ins.b >= 0)
      {
        const char *bn = builtinName(ins.b);
        ss << rn(ins.a) << " = " << (bn ? bn : "builtin?") << "(" << args(ins.a, ins.c) << ")";
      }
      else
        ss << rn(ins.a) << " = native" << (-1 - ins.b) << "(" << args(ins.a, ins.c) << ")";
      break;
    case Opcode::DELETE:
      ss << "delete " << rn(ins.a);
      break;
    case Opcode::ARRGET:
    case Opcode::SEQGET:
    case Opcode::TABGET:
    case Opcode::INDGET:
      ss << rn(ins.a) << " = " << rn(ins.b) << "[" << rn(ins.c) << "]";
      break;
    case Opcode::ARRPUT:
    case Opcode::TABPUT:
    case Opcode::INDPUT:
      ss << rn(ins.a) << "[" << rn(ins.b) << "] = " << rn(ins.c);
      break;
    case Opcode::ARRAPP:
      ss << rn(ins.a) << " += " << rn(ins.c);
      break;
    case Opcode::ARRAPPV:
      ss << rn(ins.a) << " += " << rn(ins.b);
      break;
    case Opcode::ARRLEN:
    case Opcode::LEN:
      ss << rn(ins.a) << " = len(" << rn(ins.b) << ")";
      break;
    case Opcode::TABCREATE:
      ss << rn(ins.a) << " = {}";
      break;
    case Opcode::TABFINALIZE:
      ss << "finalize(" << rn(ins.a) << ")";
      break;
    case Opcode::EVALKEY:
      ss << rn(ins.a) << " = str(" << rn(ins.b) << ")";
      break;
    case Opcode::JMP:
    case Opcode::CONTINUE:
    case Opcode::BREAK:
      ss << "pc = " << ins.i32;
      break;
    case Opcode::JMPIFFALSE:
      ss << "if !truth(" << rn(ins.a) << ") pc = " << ins.i32;
      break;
    case Opcode::JMPIFTRUE:
      ss << "if truth(" << rn(ins.a) << ") pc = " << ins.i32;
      break;
    case Opcode::CALLFN:
    {
      std::string fn = "fn" + std::to_string(ins.b);
      if (ins.b >= 0 && static_cast<size_t>(ins.b) < prog.functions.size())
        fn = prog.functions[static_cast<size_t>(ins.b)].name;
      ss << rn(ins.a) << " = " << fn << "(" << args(ins.a, ins.c) << ")";
      break;
    }
    case Opcode::CALLCLOS:
      ss << rn(ins.a) << " = " << rn(ins.a) << "(" << args(ins.a, ins.c) << ")";
      break;
    case Opcode::RETURN:
      ss << "return " << rn(ins.a);
      break;
    case Opcode::MAKEFUNC:
      ss << rn(ins.a) << " = closure(pool[" << ins.b << "])";
      break;
    case Opcode::MAKEGEN:
      ss << rn(ins.a) << " = generator(pool[" << ins.b << "])";
      break;
    case Opcode::REFLOAD:
      ss << rn(ins.a) << " = cap[" << ins.b << "]";
      break;
    case Opcode::REFSTORE:
      ss << "cap[" << ins.b << "] = " << rn(ins.a);
      break;
    case Opcode::YIELD:
      ss << "yield " << rn(ins.b);
      break;
    }
    return ss.str();
  }

  // Which operands are meaningful per opcode; the rest are omitted from the
  // listing. Bits: 1=a, 2=b, 4=c, 8=combined i32 (jump targets). Default:
  // a/b/c. (Matches the operand semantics in QLangBytecode.h, e.g.
  // ARRAPP/CALLCLOS skip b.)
  int operandMask(Opcode op)
  {
    switch (op)
    {
    case Opcode::INC:
    case Opcode::DEC:
    case Opcode::DELETE:
    case Opcode::TABCREATE:
    case Opcode::TABFINALIZE:
    case Opcode::RETURN:
    case Opcode::JMPIFFALSE: // condition in a; target is the i32 slot
    case Opcode::JMPIFTRUE:  // condition in a; target is the i32 slot
      return 1;
    case Opcode::JMP: // 32-bit pc target in the combined i32 slot
    case Opcode::CONTINUE:
    case Opcode::BREAK:
      return 8;
    case Opcode::CONST:
    case Opcode::MOV:
    case Opcode::NEG:
    case Opcode::NOT:
    case Opcode::ARRAPPV:
    case Opcode::ARRLEN:
    case Opcode::LEN:
    case Opcode::EVALKEY:
    case Opcode::MAKEFUNC:
    case Opcode::MAKEGEN:
    case Opcode::REFLOAD:
    case Opcode::REFSTORE:
      return 3;
    case Opcode::ARRAPP:
    case Opcode::CALLCLOS:
      return 5; // a and c only
    default:
      return 7;
    }
  }

  // Comma-separated operands, skipping unused ones; b is decimal when
  // negative (native-call index, -1 - id).
  std::string operandText(const Instruction &ins)
  {
    auto fmt = [](int v)
    {
      char b[8];
      std::snprintf(b, sizeof b, v < 0 ? "%d" : "%.2x", v);
      return std::string(b);
    };
    std::string s;
    auto add = [&s, &fmt](int v)
    {
      if (!s.empty())
        s += ", ";
      s += fmt(v);
    };
    const int m = operandMask(ins.op);
    if (m & 1)
      add(ins.a);
    if (m & 2)
      add(ins.b);
    if (m & 4)
      add(ins.c);
    if (m & 8)
      add(ins.i32);
    return s;
  }

  // Render a string with escapes (\n, \r, \t, \", \\, \xNN for other
  // non-printables) so it stays on one line in the listing.
  std::string quoteString(const std::string &s)
  {
    std::string out = "\"";
    for (char ch : s)
    {
      switch (ch)
      {
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      default:
      {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc < 0x20 || uc == 0x7f)
        {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\x%02x", uc);
          out += buf;
        }
        else
          out += ch;
      }
      }
    }
    out += "\"";
    return out;
  }
  std::string valueText(const QLangValue &v, int depth = 0)
  {
    using T = QLangValue::Type;
    switch (v.type())
    {
    case T::Nil:
      return "nil";
    case T::Eof:
      return "eof";
    case T::Bool:
      return v.asInt() ? "true" : "false";
    case T::Char:
      return std::to_string(v.asChar());
    case T::Byte:
      return std::to_string(v.asByte());
    case T::Word:
      return std::to_string(v.asWord());
    case T::Short:
      return std::to_string(v.asShort());
    case T::Int:
      return std::to_string(v.asInt());
    case T::Unsigned:
      return std::to_string(v.asUnsigned());
    case T::Qword:
      return std::to_string(v.asQword());
    case T::Long:
      return std::to_string(v.asLong());
    case T::Float:
    {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", v.asFloat());
      return buf;
    }
    case T::String:
      return quoteString(v.asString());
    case T::Blob:
      return "<" + std::to_string(v.asBlob().size()) + " bytes>";
    case T::Array:
    case T::Set:
    {
      if (depth)
        return "[...]";
      std::string s = "[";
      const auto &a = v.asArray();
      for (size_t i = 0; i < a.size(); i++)
      {
        if (i)
          s += ", ";
        s += valueText(a[i], depth + 1);
      }
      return s + "]";
    }
    case T::Table:
    {
      if (depth)
        return "{...}";
      std::string s = "{";
      const auto &t = v.asTable();
      bool first = true;
      for (const auto &[k, val] : *t)
      {
        if (!first)
          s += ", ";
        first = false;
        s += k + ": " + valueText(val, depth + 1);
      }
      return s + "}";
    }
    case T::Function:
      return "function<" + std::to_string(v.asFunction().funcIndex) + ">";
    case T::Generator:
      return "generator";
    case T::Object:
      return "object";
    }
    return "?";
  }

  std::string disassemble(const CompiledProgram &prog, const std::vector<std::string> &srcLines,
                          int level)
  {
    std::ostringstream ss;
    if (level >= 1)
    {
      ss << "QLang disassembly (v1)\n";
      ss << "registers: " << prog.registerCount << "\n\n";
    }
    // Level 4: widest pseudo-mnemonic in the whole listing, so the "; ..."
    // and "# ..." columns align on every line.
    size_t maxPseudo = 0;
    if (level >= 4)
      for (const auto &fn : prog.functions)
        for (const auto &k : fn.instructions)
          maxPseudo = std::max(maxPseudo, pseudoText(prog, k).size());
    for (size_t i = 0; i < prog.functions.size(); i++)
    {
      const auto &f = prog.functions[i];
      if (level >= 1)
      {
        ss << "function #" << i << " '" << f.name << "'\n";
        ss << "  params: " << f.paramCount
           << "  registers: " << f.registerCount
           << (f.isGenerator ? "  generator" : "") << "\n";
        if (!f.captures.empty())
        {
          ss << "  captures:";
          for (const auto &cap : f.captures)
            ss << " r" << cap.reg;
          ss << "\n";
        }
        // Level 4: variable -> home-register map (main only; function locals
        // and params have no name->register table in the CompiledProgram).
        if (level >= 4 && i == 0 && !prog.vars.empty())
        {
          std::vector<std::pair<int, std::string>> regs;
          for (const auto &[name, reg] : prog.vars)
            regs.emplace_back(reg, name);
          std::sort(regs.begin(), regs.end());
          ss << "  vars:\n";
          for (const auto &[reg, name] : regs)
            ss << "    r" << std::setw(3) << reg << "  " << name << "\n";
        }
        ss << "  constants:\n";
        for (size_t j = 0; j < f.constants.size(); j++)
          ss << "    [" << j << "] " << valueText(f.constants[j]) << "\n";
        ss << "  code:\n";
      }
      else
        ss << "function #" << i << " '" << f.name << "':\n";

      int lastLine = -1;
      for (size_t j = 0; j < f.instructions.size(); j++)
      {
        const auto &ins = f.instructions[j];
        if (level >= 2 && ins.line != lastLine && ins.line >= 1 &&
            static_cast<size_t>(ins.line) <= srcLines.size())
          ss << "  L" << std::right << std::setw(2) << ins.line << ":  "
             << srcLines[static_cast<size_t>(ins.line) - 1] << "\n";
        lastLine = ins.line;
        // Level 3: append a copy of the referenced constant (operand b is the
        // pool index for CONST / MAKEFUNC / MAKEGEN).
        std::string constNote;
        if (level >= 3)
        {
          switch (ins.op)
          {
          case Opcode::CONST:
          case Opcode::MAKEFUNC:
          case Opcode::MAKEGEN:
            if (ins.b >= 0 && static_cast<size_t>(ins.b) < f.constants.size())
              constNote = valueText(f.constants[static_cast<size_t>(ins.b)]);
            break;
          default:
            break;
          }
        }
        // Level 4: name the operands of CALL / CALLFN / CONV / CAST.
        if (level >= 4)
        {
          switch (ins.op)
          {
          case Opcode::CALL:
            if (ins.b >= 0)
            {
              const char *bn = builtinName(ins.b);
              constNote = std::string("builtin:") + (bn ? bn : "unknown");
            }
            else
              constNote = "native:" + std::to_string(-1 - ins.b);
            break;
          case Opcode::CALLFN:
            if (ins.b >= 0 && static_cast<size_t>(ins.b) < prog.functions.size())
              constNote = "fn:" + prog.functions[static_cast<size_t>(ins.b)].name;
            break;
          case Opcode::CONV:
          case Opcode::CAST:
            constNote = "type:" +
                        std::string(QLangValue(static_cast<QLangValue::Type>(ins.c)).typeName());
            break;
          default:
            break;
          }
        }
        char addr[32];
        snprintf(addr, sizeof(addr), "%.4x", j);

        // Level 4: pseudo-mnemonic (register-level meaning), padded so the
        // trailing "#" note starts in the same column on every line.
        std::string pseudo;
        if (level >= 4)
        {
          pseudo = " ; " + pseudoText(prog, ins);
          if (pseudo.size() < maxPseudo + 3)
            pseudo.append(maxPseudo + 3 - pseudo.size(), ' ');
        }
        const std::string note = constNote.empty() ? std::string() : "  # " + constNote;
        ss << "  " << std::setw(4) << addr
           << "  " << std::left << std::setw(11) << Bytecode::name(ins.op)
           << "  " << std::setw(10) << operandText(ins)
           << pseudo << note << "\n";
      }
      ss << "\n";
    }
    return ss.str();
  }

} // namespace

int main(int argc, char **argv)
{
  int traceLevel = TRACE_OFF;
  bool profile = false;
  int asmLevel = 1;
  std::string file;
  std::string output;
  std::string asmOut;

  for (int i = 1; i < argc; i++)
  {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help")
    {
      printHelp(argv[0]);
      return 0;
    }

    if (arg == "--trace-level")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --trace-level requires a value (0-3)\n", kAppName);
        return 1;
      }
      if (!parseTraceLevel(argv[++i], traceLevel))
      {
        std::fprintf(stderr, "%s: invalid --trace-level '%s' (expected 0, 1, 2, or 3)\n",
                     kAppName, argv[i]);
        return 1;
      }
      continue;
    }

    if (arg.rfind("--trace-level=", 0) == 0)
    {
      if (!parseTraceLevel(arg.substr(14), traceLevel))
      {
        std::fprintf(stderr, "%s: invalid --trace-level '%s' (expected 0, 1, 2, or 3)\n",
                     kAppName, arg.c_str());
        return 1;
      }
      continue;
    }

    if (arg == "--profile")
    {
      profile = true;
      continue;
    }

    if (arg == "--output")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --output requires a file name\n", kAppName);
        return 1;
      }
      output = argv[++i];
      continue;
    }

    if (arg.rfind("--output=", 0) == 0)
    {
      output = arg.substr(9);
      continue;
    }

    if (arg == "--asm")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --asm requires a file name\n", kAppName);
        return 1;
      }
      asmOut = argv[++i];
      continue;
    }

    if (arg.rfind("--asm=", 0) == 0)
    {
      asmOut = arg.substr(6);
      continue;
    }

    if (arg == "--asm-level")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --asm-level requires a value (0-3)\n", kAppName);
        return 1;
      }
      if (!parseAsmLevel(argv[++i], asmLevel))
      {
        std::fprintf(stderr, "%s: invalid --asm-level '%s' (expected 0, 1, 2, or 3)\n",
                     kAppName, argv[i]);
        return 1;
      }
      continue;
    }

    if (arg.rfind("--asm-level=", 0) == 0)
    {
      if (!parseAsmLevel(arg.substr(12), asmLevel))
      {
        std::fprintf(stderr, "%s: invalid --asm-level '%s' (expected 0, 1, 2, or 3)\n",
                     kAppName, arg.c_str());
        return 1;
      }
      continue;
    }

    if (!arg.empty() && arg[0] == '-' && arg != "-")
    {
      std::fprintf(stderr, "%s: unknown option '%s' (see --help)\n", kAppName, arg.c_str());
      return 1;
    }

    if (!file.empty())
    {
      std::fprintf(stderr, "%s: unexpected extra argument '%s' (one script file expected)\n",
                   kAppName, arg.c_str());
      return 1;
    }
    file = arg;
  }

  if (file.empty())
  {
    std::fprintf(stderr, "%s: missing script file (see --help)\n", kAppName);
    return 1;
  }

  std::ifstream in(file, std::ios::binary);
  if (!in)
  {
    std::fprintf(stderr, "%s: cannot open script file '%s'\n", kAppName, file.c_str());
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string source = ss.str();

  QLangContext ctx;
  ctx.setVerboseLevel(traceLevel);

  // --- Parse ---
  const auto t0 = Clock::now();
  std::vector<StmtPtr> program;
  try
  {
    program = ctx.parse(source);
  }
  catch (const QLangError &e)
  {
    std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), e.what());
    return 3;
  }
  const size_t stmtCount = program.size();
  const auto t1 = Clock::now();

  // --- Compile ---
  QLangCompiler compiler(std::move(program), &ctx);
  // The app never injects host constants (ctx.set(name, value, true)), so the
  // external-const set is empty.
  compiler.addExternalConsts({});
  std::unordered_set<std::string> nativeNames;
  for (const auto &[name, _] : ctx.nativeFuncs())
    nativeNames.insert(name);
  compiler.addExternalFuncs(nativeNames);
  compiler.addModules(ctx.modules());

  CompiledProgram compiled;
  try
  {
    compiled = compiler.compile();
  }
  catch (const QLangError &e)
  {
    std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), e.what());
    return 3;
  }
  // Wire native functions exactly as QLangContext::run() does.
  for (const auto &[name, func] : ctx.nativeFuncs())
  {
    auto it = compiler.nativeFuncIds().find(name);
    if (it != compiler.nativeFuncIds().end())
    {
      int idx = -1 - it->second;
      while (compiled.nativeFuncs.size() <= static_cast<size_t>(idx))
        compiled.nativeFuncs.push_back(nullptr);
      compiled.nativeFuncs[static_cast<size_t>(idx)] = func;
    }
  }
  const auto &resolved = compiler.nativeFuncsResolved();
  for (size_t i = 0; i < resolved.size(); i++)
  {
    while (compiled.nativeFuncs.size() <= i)
      compiled.nativeFuncs.push_back(nullptr);
    if (resolved[i])
      compiled.nativeFuncs[i] = resolved[i];
  }
  const auto t2 = Clock::now();

  // --- Bytecode output ---
  if (!output.empty())
  {
    std::map<std::string, int> nativeIds(compiler.nativeFuncIds().begin(),
                                          compiler.nativeFuncIds().end());
    size_t written = 0;
    if (!writeBytecode(compiled, nativeIds, output, written))
    {
      std::fprintf(stderr, "%s: cannot write bytecode file '%s'\n", kAppName, output.c_str());
      return 4;
    }
    std::fprintf(stderr, "bytecode written: %s (%zu bytes, %zu functions)\n",
                 output.c_str(), written, compiled.functions.size());
  }

  // --- Disassembly output ---
  if (!asmOut.empty())
  {
    std::vector<std::string> srcLines;
    {
      std::istringstream ls(source);
      std::string line;
      while (std::getline(ls, line))
        srcLines.push_back(line);
    }
    const std::string text = disassemble(compiled, srcLines, asmLevel);
    std::ofstream out(asmOut, std::ios::trunc);
    if (!out)
    {
      std::fprintf(stderr, "%s: cannot write disassembly file '%s'\n", kAppName, asmOut.c_str());
      return 5;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
    {
      std::fprintf(stderr, "%s: cannot write disassembly file '%s'\n", kAppName, asmOut.c_str());
      return 5;
    }
    std::fprintf(stderr, "disassembly written: %s (%zu functions)\n",
                 asmOut.c_str(), compiled.functions.size());
  }

  // --- Execute ---
  const size_t instrCount = compiled.functions.empty()
                                ? 0
                                : compiled.functions[0].instructions.size();
  QLangVM vm(compiled, ctx);
  const auto t3 = Clock::now();
  try
  {
    vm.run();
  }
  catch (const QLangError &e)
  {
    std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), e.what());
    return 3;
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), e.what());
    return 3;
  }
  const auto t4 = Clock::now();

  if (profile)
  {
    size_t constCount = 0;
    for (const auto &f : compiled.functions)
      constCount += f.constants.size();
    std::fprintf(stderr, "profile: parse    %.3f ms  (%zu statements)\n",
                 msBetween(t0, t1), stmtCount);
    std::fprintf(stderr, "profile: compile  %.3f ms  (%zu instructions, %zu constants, %zu functions)\n",
                 msBetween(t1, t2), instrCount, constCount, compiled.functions.size());
    std::fprintf(stderr, "profile: execute  %.3f ms  (%zu instructions)\n",
                 msBetween(t3, t4), instrCount);
    std::fprintf(stderr, "profile: total    %.3f ms\n", msBetween(t0, t4));
  }
  return 0;
}
