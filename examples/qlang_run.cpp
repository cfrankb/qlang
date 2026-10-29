// qlang_run — small driver that runs a .ql (QLang) script with a selectable
// diagnostic trace level, optional stage timing, and bytecode output.
//
//   qlang_run [--trace-level <0-3>] [--profile] [--output <file.qlc>]
//             [--asm <file.qla>] file.ql
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

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

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

// ---------------------------------------------------------------------------
// .qlc binary writer.
//
// Format (little-endian), version 1:
//   magic "QLC1", u32 version
//   u32 funcCount, then per function:
//     u32 nameLen + name bytes
//     i32 paramCount, i32 registerCount, u8 isGenerator
//     u32 captureCount, then i32 reg each
//     u32 instrCount, then per instruction: u8 op, i32 a, i32 b, i32 c,
//                                            i32 line, i32 col
//     u32 constCount, then serialized QLangValue each
//   u32 varCount, then per var: u32 nameLen + name, i32 register
//   u32 constVarCount, then per name: u32 nameLen + name
//   i32 registerCount
//
// QLangValue serialization:
//   u8 type, then payload:
//     Nil / Eof: none
//     Bool / Int: i32
//     Char / Short: i32
//     Byte: u8    Word: u32    Unsigned: u32
//     Qword: u64  Float: f64
//     String / Blob: u32 len + bytes
//     Array: u32 count + values
//     Table: u32 count + (u32 keyLen + key + value)
//     Function: i32 funcIndex + u32 captureCount + i32 reg each
//               (capture frame pointers are runtime-only, not persisted)
//     Generator: stored as Nil (not durable)
//
// Native (C++) function bindings are NOT part of the file; a loader re-binds
// them from its own context.
// ---------------------------------------------------------------------------
struct QlcWriter
{
  std::string buf;

  void u8(uint8_t v) { buf += static_cast<char>(v); }
  void u32(uint32_t v) { bytes(&v, sizeof v); }
  void i32(int32_t v) { bytes(&v, sizeof v); }
  void u64(uint64_t v) { bytes(&v, sizeof v); }
  void f64(double v) { bytes(&v, sizeof v); }
  void bytes(const void *p, size_t n) { buf.append(static_cast<const char *>(p), n); }
  void str(const std::string &s) { u32(static_cast<uint32_t>(s.size())); bytes(s.data(), s.size()); }

  void value(const QLangValue &v)
  {
    using T = QLangValue::Type;
    T t = v.type();
    if (t == T::Generator)
      t = T::Nil; // not durable
    u8(static_cast<uint8_t>(t));
    switch (t)
    {
    case T::Nil:
    case T::Eof:
      break;
    case T::Bool:
    case T::Int:
      i32(v.asInt());
      break;
    case T::Char:
      i32(v.asChar());
      break;
    case T::Short:
      i32(v.asShort());
      break;
    case T::Byte:
      u8(v.asByte());
      break;
    case T::Word:
      u32(v.asWord());
      break;
    case T::Unsigned:
      u32(v.asUnsigned());
      break;
    case T::Qword:
      u64(v.asQword());
      break;
    case T::Long:
      u64(static_cast<uint64_t>(v.asLong()));
      break;
    case T::Float:
      f64(v.asFloat());
      break;
    case T::String:
      str(v.asString());
      break;
    case T::Blob:
    {
      const auto &b = v.asBlob();
      u32(static_cast<uint32_t>(b.size()));
      bytes(b.data(), b.size());
      break;
    }
    case T::Array:
    {
      const auto &a = v.asArray();
      u32(static_cast<uint32_t>(a.size()));
      for (const auto &e : a)
        value(e);
      break;
    }
    case T::Table:
    {
      const auto &t = v.asTable();
      u32(static_cast<uint32_t>(t->size()));
      for (const auto &[k, val] : *t)
      {
        str(k);
        value(val);
      }
      break;
    }
    case T::Function:
    {
      const auto &c = v.asFunction();
      i32(static_cast<int32_t>(c.funcIndex));
      u32(static_cast<uint32_t>(c.captures.size()));
      for (const auto &cap : c.captures)
        i32(cap.reg);
      break;
    }
    }
  }

  size_t write(const CompiledProgram &prog)
  {
    bytes("QLC1", 4);
    u32(1); // format version

    const auto &fns = prog.functions;
    u32(static_cast<uint32_t>(fns.size()));
    for (const auto &f : fns)
    {
      str(f.name);
      i32(f.paramCount);
      i32(f.registerCount);
      u8(f.isGenerator ? 1 : 0);
      u32(static_cast<uint32_t>(f.captures.size()));
      for (const auto &cap : f.captures)
        i32(cap.reg);
      u32(static_cast<uint32_t>(f.instructions.size()));
      for (const auto &ins : f.instructions)
      {
        u8(static_cast<uint8_t>(ins.op));
        i32(ins.a);
        i32(ins.b);
        i32(ins.c);
        i32(ins.line);
        i32(ins.col);
      }
      u32(static_cast<uint32_t>(f.constants.size()));
      for (const auto &c : f.constants)
        value(c);
    }

    u32(static_cast<uint32_t>(prog.vars.size()));
    for (const auto &[name, reg] : prog.vars)
    {
      str(name);
      i32(reg);
    }
    u32(static_cast<uint32_t>(prog.constVars.size()));
    for (const auto &name : prog.constVars)
      str(name);
    i32(prog.registerCount);
    return buf.size();
  }
};

bool writeBytecode(const CompiledProgram &prog, const std::string &path, size_t &written)
{
  QlcWriter w;
  written = w.write(prog);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(w.buf.data(), static_cast<std::streamsize>(w.buf.size()));
  return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// Disassembly listing (.qla): one section per function with its metadata,
// constant pool (human-readable), and instruction stream (mnemonic from
// Bytecode::name plus raw operands a/b/c and the stamped source line:col).
// ---------------------------------------------------------------------------
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
    return "\"" + v.asString() + "\"";
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

std::string disassemble(const CompiledProgram &prog)
{
  std::ostringstream ss;
  ss << "QLang disassembly (v1)\n";
  ss << "registers: " << prog.registerCount << "\n\n";
  for (size_t i = 0; i < prog.functions.size(); i++)
  {
    const auto &f = prog.functions[i];
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
    ss << "  constants:\n";
    for (size_t j = 0; j < f.constants.size(); j++)
      ss << "    [" << j << "] " << valueText(f.constants[j]) << "\n";
    ss << "  code:\n";
    for (size_t j = 0; j < f.instructions.size(); j++)
    {
      const auto &ins = f.instructions[j];
      ss << "  " << std::setw(4) << j << "  " << ins.line << ":" << ins.col
         << "  " << std::left << std::setw(10) << Bytecode::name(ins.op) << std::right
         << "  " << ins.a << " " << ins.b << " " << ins.c << "\n";
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
    size_t written = 0;
    if (!writeBytecode(compiled, output, written))
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
    const std::string text = disassemble(compiled);
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
