// qlang_run — the .ql / .qlc loader (SPEC §qlang-run): runs a QLang script or
// a compiled QLC1 v9 image with a selectable diagnostic trace level, optional
// stage timing, and optional artifacts on disk.
//
//   qlang_run [--trace-level <0-3>] [--profile] [-c] [-a] [-r]
//             [--output-path <dir>] [--asm-level <0-5>]
//             [--module-path <dir>]... [--no-module-search]
//             [--import <lib>]... file.ql | file.qlc [lib]...
//   qlang_run --qlmeta-dump <file.qlm>
//
// A .ql input drives the pipeline directly (parse -> compile -> execute) so
// the app can time each stage and hand the compiled program to the .qlc
// writer. A .qlc input (QLC1 magic) is loaded with readQlc, its native
// functions are re-bound by name, and the image is executed as-is.
//
// Artifacts are named, not specified: -c saves the entry's compiled image and
// -a writes a disassembly listing, both as <entry stem>.qlc / <entry stem>.qla
// in --output-path, or in the entry source's own directory when no
// --output-path is given. -r rotates those two names only (cmp.qlc ->
// cmp_202611010513.qlc). An artifact is never written over the file the entry
// was read from.
//
// Library modules are found, not named: an `import <name>` the context cannot
// answer from its registry is resolved against the module search paths
// (SPEC §api "module search paths"; plans C8a + C8b) — the entry file's own
// directory, then the working directory, then each --module-path — where a
// <name>.qlc is taken in as an image and a <name>.ql is compiled into one. A
// discovered module is registered under its own declared name and its own
// imports are resolved with it, so `qlang_run app.ql` needs no library
// arguments at all. --import (and
// extra positional library arguments) still loads a module explicitly — useful
// for a library outside every search path — and now accepts a name, a
// directory, or a .ql/.qlc path: the .qlc is used when it exists, carries the
// current kQlcVersion and is not older than its .ql; otherwise the .ql is
// compiled, its image is saved next to the source (never rotated) and that
// image is loaded. With -a every image generated this way also gets a .qla
// listing beside it. The entry's cross-module references are then merged with
// every loaded image by QLangLinker; -c writes the entry's own module image
// (the pre-link program), since a merged program is not serializable.
// Trace output goes to stderr; profile output goes to stderr and, when a
// .qla listing is requested, is appended to that file as well. The script's
// own program output (print/println) goes to stdout and is never affected.
//
// Exit codes:
//   0  script ran to completion
//   1  usage error (bad/missing arguments)
//   2  a required file could not be read
//   3  QLang error (lex/parse/type/runtime)
//   4  bytecode file could not be written
//   5  disassembly file could not be written

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <qlang/QLangBytecodeIO.h>
#include <qlang/QLangCompiler.h>
#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>
#include <qlang/QLangLinker.h>
#include <qlang/QLangManifest.h>
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
    std::cout << "Run a QLang (.ql) script, a compiled .qlc image (QLC1 v9) or a\n"
                 "portable module (.qpm, QPM1).\n"
              << "\n"
              << "usage: " << prog
              << " [options] <file.ql|file.qlc|file.qpm> [library ...]\n"
              << "       " << prog << " --qlmeta-dump <file.qlm>\n"
              << "       " << prog
              << " --qlmeta <file.qlm> [options] <file.ql> [-c]\n"
              << "       " << prog
              << " [options] <file.ql> --package <file.qpm>\n"
              << "\n"
              << "options:\n"
              << "  --trace-level <n>   diagnostic verbosity (stderr):\n"
              << "                        0 = off (default)\n"
              << "                        1 = frame/call events\n"
              << "                        2 = VM loop + opcodes + returns\n"
              << "                        3 = full detail (generators, table ops, compile)\n"
              << "  --profile           report parse / compile / execute times in ms (stderr,\n"
              << "                        also appended to the -a .qla listing when given)\n"
              << "  -c                  save the compiled entry image as <stem>.qlc in the\n"
              << "                        output path (see --output-path). The entry's own\n"
              << "                        module image is written, not the linked program\n"
              << "  -a                  save a disassembly listing as <stem>.qla in the output\n"
              << "                        path; every image --import generates gets one too\n"
              << "  -r                  alias for --rotate\n"
              << "  --output-path <dir> directory the -c/-a artifacts are written to (created\n"
              << "                        when missing); defaults to the entry file's directory\n"
              << "  --asm-level <n>     disassembly detail (default 1):\n"
              << "                        0 = instructions only\n"
              << "                        1 = instructions + constants + function info\n"
              << "                        2 = level 1 + interleaved source lines\n"
              << "                        3 = level 2 + const values inlined next to instructions\n"
              << "                        4 = level 3 + register map, operand names, pseudo-mnemonics\n"
              << "                        5 = level 4 + end-of-compile register breakdown\n"
              << "                            (per register: named var / free pool / quarantine /\n"
              << "                             deferred / live temp / call-arg slot / unallocated)\n"
              << "                            + module breakdown: the module's\n"
              << "                             name, the functions its `import`\n"
              << "                             statements bound (native slot,\n"
              << "                             call counts), its export table\n"
              << "                             (kind + resolved ref) and the\n"
              << "                             top-level defs it did not export;\n"
              << "                             for a linked program also the\n"
              << "                             merged import table and one block\n"
              << "                             per linked module (where the\n"
              << "                             linker placed it, its exports)\n"
              << "  --module-path <dir> add <dir> to the module search paths,\n"
              << "                        which `import <name>` resolves against\n"
              << "                        after the registry (repeatable). The\n"
              << "                        entry file's own directory is searched\n"
              << "                        first, then the working directory;\n"
              << "                        a directory is searched for <name>.qlc\n"
              << "                        before <name>.ql, which is compiled when\n"
              << "                        it is what is found; one that does not\n"
              << "                        exist is skipped. A library's own\n"
              << "                        imports use these same paths — a\n"
              << "                        library's directory is searched only\n"
              << "                        when it is the entry's or is listed\n"
              << "  --no-module-search  search no paths at all: `import <name>`\n"
              << "                        resolves only against C++ modules and\n"
              << "                        images named on this command line\n"
              << "  --import <lib>      load one module explicitly instead of letting\n"
              << "                        it be found (repeatable; extra positional\n"
              << "                        library arguments are the same). <lib> is a\n"
              << "                        bare name, a directory holding <name>.qlc /\n"
              << "                        <name>.ql, or a .ql/.qlc path. A .qlc is used\n"
              << "                        when it exists, carries this build's .qlc\n"
              << "                        version and is not older than its .ql; the .ql\n"
              << "                        is otherwise compiled, its image saved beside\n"
              << "                        the source (never rotated) and then loaded\n"
              << "  --qlmeta-dump <file.qlm>\n"
              << "                        standalone mode: write every QL_OBJECT\n"
              << "                        type this binary registers to <file.qlm> as a\n"
              << "                        QLM1 type manifest (the qlc --qlmeta input),\n"
              << "                        report the type count on stdout and exit; no\n"
              << "                        script is run and no other mode flag combines\n"
              << "  --qlmeta <file.qlm> compile with a QLM1 type manifest as the\n"
              << "                        external-type contract: .ql sources are checked\n"
              << "                        against it (member/method/ctor/signal shape)\n"
              << "                        with no bindings linked; ignored for .qlc input\n"
              << "  --package <file.qpm>  write a QPM1 portable module holding the entry\n"
              << "                        image, every library image this invocation loaded\n"
              << "                        and the --qlmeta manifest (empty when none was\n"
              << "                        given). Load one with: qlang_run file.qpm — the\n"
              << "                        load handshake requires this binary to register\n"
              << "                        every manifest type unchanged, so a manifest built\n"
              << "                        from another host's bindings loads only there\n"
              << "  --rotate            append a YYYYMMDDHHMM timestamp to the -c/-a\n"
              << "                        file names (e.g. cmp.qlc -> cmp_202611010513.qlc);\n"
              << "                        images generated by --import are never rotated\n"
              << "  -h, --help          show this help and exit\n"
              << "\n"
              << "exit codes:\n"
              << "  0  script ran to completion\n"
              << "  1  usage error\n"
              << "  2  a required file could not be read\n"
              << "  3  QLang error (lex/parse/type/runtime)\n"
              << "  4  bytecode or portable module file could not be written\n"
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

  // Accept exactly "0".."5". Returns false on failure.
  bool parseAsmLevel(const std::string &s, int &out)
  {
    if (s.size() != 1 || s[0] < '0' || s[0] > '5')
      return false;
    out = s[0] - '0';
    return true;
  }

  // Current local time as "YYYYMMDDHHMM" (e.g. "202611010513").
  std::string rotateStamp()
  {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now); // MSVC: reversed argument order vs. localtime_r
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
  }

  // Insert "_<stamp>" before the extension: "cmp.qlc" -> "cmp_202611010513.qlc".
  // Paths with no extension get the stamp appended; directory components are
  // left untouched.
  std::string rotatePath(const std::string &path)
  {
    const size_t slash = path.find_last_of("/\\");
    const std::string dir = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
      return dir + base + "_" + rotateStamp();
    return dir + base.substr(0, dot) + "_" + rotateStamp() + base.substr(dot);
  }

  // The module name a source file implies: its basename without the extension
  // ("tests/scripts/hello.ql" -> "hello"). SPEC §bytecodes "keyword export"
  // makes the name a property of the module itself (`export const MODULE_NAME`),
  // which the compiler honours over this driver-supplied default; a file with no
  // usable stem yields "" so the compiler falls back to "main".
  std::string moduleStem(const std::string &path)
  {
    const size_t slash = path.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
      return base;
    return base.substr(0, dot);
  }

  // Case-insensitive suffix test used to tell an entry source file from a
  // library image argument (".ql" / ".qlc").
  bool hasExtension(const std::string &path, const std::string &ext)
  {
    if (path.size() < ext.size())
      return false;
    return std::equal(ext.rbegin(), ext.rend(), path.rbegin(),
                      [](char a, char b)
                      { return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b)); });
  }

  // ---------------------------------------------------------------------------
  // Artifact paths (SPEC §qlang-run). -c/-a name their files, they are not
  // named on the command line: <entry stem>.qlc / <entry stem>.qla inside
  // --output-path, or inside the entry file's own directory when that flag is
  // absent ("the full qualified path of the original module").
  // ---------------------------------------------------------------------------

  // The directory a file lives in, made absolute ("tests/scripts/hello.ql" ->
  // "<cwd>/tests/scripts"). A path with no directory component ("" for a bare
  // "hello.ql") becomes the working directory, spelled ".".
  std::string moduleDirectory(const std::string &path)
  {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    if (ec)
      p = std::filesystem::path(path);
    const std::filesystem::path dir = p.parent_path();
    return dir.empty() ? std::string(".") : dir.string();
  }

  // One artifact path: <dir>/<stem><ext>, with the -r stamp inserted before the
  // extension when rotation was asked for.
  std::string outputName(const std::string &dir, const std::string &stem,
                         const std::string &ext, bool rotateNames)
  {
    const std::string name = stem + ext;
    const std::string path = (std::filesystem::path(dir) / name).string();
    return rotateNames ? rotatePath(path) : path;
  }

  // True when two spellings name the same file. Guards against writing an
  // artifact over the file it was read from (`qlang_run -c lib.qlc` with the
  // default output path would otherwise rewrite the image in place). Falls back
  // to a string compare when the filesystem cannot canonicalise either path.
  bool samePath(const std::string &a, const std::string &b)
  {
    std::error_code ec;
    const std::filesystem::path pa = std::filesystem::absolute(a, ec);
    if (ec)
      return a == b;
    const std::filesystem::path pb = std::filesystem::absolute(b, ec);
    if (ec)
      return a == b;
    const std::filesystem::path ra = std::filesystem::weakly_canonical(pa, ec);
    if (ec)
      return pa == pb;
    const std::filesystem::path rb = std::filesystem::weakly_canonical(pb, ec);
    return ec ? (pa == pb) : (ra == rb);
  }

  // Write a whole byte buffer, creating or truncating the file. Returns false
  // with a message when the file cannot be opened or the write does not land.
  bool writeBinaryFile(const std::string &path, const std::string &bytes,
                       std::string &err)
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      err = "cannot open '" + path + "' for writing";
      return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out)
    {
      err = "cannot write '" + path + "'";
      return false;
    }
    return true;
  }

  // The .qlc format version a file on disk carries: the header is "QLC1" plus a
  // little-endian uint32 (docs/SERIALIZATION.md §4). 0 when the file cannot be
  // opened, is shorter than the header, or has no QLC1 magic, so a caller
  // comparing against kQlcVersion treats a corrupt or foreign file as stale.
  uint32_t qlcFileVersion(const std::string &path)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return 0;
    unsigned char hdr[8] = {0};
    in.read(reinterpret_cast<char *>(hdr), 8);
    if (in.gcount() < 8 || std::memcmp(hdr, "QLC1", 4) != 0)
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
      v |= static_cast<uint32_t>(hdr[4 + i]) << (8 * i);
    return v;
  }

  // SPEC §qlang-run "--import <lib>": reuse the image only when it is there,
  // was written by this build of the codec, and is at least as new as the source
  // it came from. Anything else — missing, v1-v8 or v10+, a touched .ql — means
  // recompiling. `qlPath` is empty for a module that has no source beside its
  // image, which leaves the version check as the only test. A filesystem error
  // reading a timestamp counts as "not current" rather than as a failure: the
  // source is then simply compiled again.
  bool imageIsCurrent(const std::string &qlcPath, const std::string &qlPath)
  {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(qlcPath, ec))
      return false;
    if (qlcFileVersion(qlcPath) != kQlcVersion)
      return false;
    if (qlPath.empty())
      return true;
    const auto src = std::filesystem::last_write_time(qlPath, ec);
    if (ec)
      return false;
    const auto img = std::filesystem::last_write_time(qlcPath, ec);
    if (ec)
      return false;
    return !(src > img);
  }

  // Where a `--import <lib>` (or positional library) argument points: the module
  // name it implies and the two files it can be satisfied from; either may be
  // empty when it is not there.
  struct ModuleFiles
  {
    std::string name; // name the argument implies (the file's stem)
    std::string ql;   // source to compile when the image is missing or stale
    std::string qlc;  // image to load when it is current
  };

  // Resolve a library argument to files. Two shapes:
  //   a path with a .ql/.qlc suffix ("libs/gfx.ql", "./gfx.qlc") -- that file is
  //            the one asked for, and the sibling extension is still probed so a
  //            source newer than its image still forces a recompile;
  //   any other name ("gfx", "libs/gfx") -- tried as spelled (relative to the
  //            working directory) and then on every module search path,
  //            <name>.qlc before <name>.ql, exactly as the engine's own resolver
  //            orders them (SPEC §api "module search paths").
  // The name is always the file's stem. SPEC §bytecodes "keyword export" makes
  // the real name a property of the module (`export const MODULE_NAME`), which
  // the loader honours over this guess; the stem only picks the file.
  bool resolveModuleFiles(const std::string &spec,
                          const std::vector<std::string> &searchPaths,
                          ModuleFiles &out, std::string &err)
  {
    // Both extensions beside one base name, image first.
    auto probe = [](const std::string &base)
    {
      ModuleFiles m;
      m.name = moduleStem(base);
      std::error_code ec;
      if (std::filesystem::is_regular_file(base + ".qlc", ec))
        m.qlc = base + ".qlc";
      if (std::filesystem::is_regular_file(base + ".ql", ec))
        m.ql = base + ".ql";
      return m;
    };

    if (hasExtension(spec, ".qlc") || hasExtension(spec, ".ql"))
    {
      const bool image = hasExtension(spec, ".qlc");
      ModuleFiles m = probe(spec.substr(0, spec.size() - (image ? 4 : 3)));
      if (image)
        m.qlc = spec;
      else
        m.ql = spec;
      if (m.qlc.empty() && m.ql.empty())
      {
        err = "module file '" + spec + "' does not exist";
        return false;
      }
      out = m;
      return true;
    }

    std::vector<std::string> bases;
    bases.push_back(spec);
    for (const std::string &dir : searchPaths)
      bases.push_back((std::filesystem::path(dir.empty() ? "." : dir) / spec).string());
    for (const std::string &base : bases)
    {
      ModuleFiles m = probe(base);
      if (!m.qlc.empty() || !m.ql.empty())
      {
        out = m;
        return true;
      }
    }
    // Wording note: the engine prefixes a callback failure with "module '<n>': ",
    // so the names below carry the module rather than repeating it.
    err = "no '" + spec + ".qlc' or '" + spec + ".ql' on the module search paths";
    return false;
  }

  // ---------------------------------------------------------------------------
  // .qlc binary output: the library codec (src/QLangBytecodeIO.cpp,
  // docs/SERIALIZATION.md). nativeIds maps native name -> negative opcode id
  // (compiler.nativeFuncIds()); the slot table is written in ascending slot
  // order and lets a loader re-bind natives by name.
  // The .qlc image in memory (the same codec writeBytecode uses); --package
  // embeds images instead of writing them out.
  bool qlcBytes(const CompiledProgram &prog,
                const std::map<std::string, int> &nativeIds, std::string &bytes)
  {
    std::ostringstream ss(std::ios::binary);
    if (!writeQlc(ss, prog, nativeIds))
      return false;
    bytes = ss.str();
    return true;
  }

  bool writeBytecode(const CompiledProgram &prog,
                     const std::map<std::string, int> &nativeIds,
                     const std::string &path, size_t &written)
  {
    std::string buf;
    if (!qlcBytes(prog, nativeIds, buf))
      return false;
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
  //       instruction (MOV kind 2 / MAKEFUNC / MAKEGEN, whose b is a pool
  //       index)
  //   4 = level 3 + the variable->register map (main function), operand names
  //       for CALL (builtin/native), CALLFN (function name), CONV/CAST (target
  //       type), and a pseudo-mnemonic: the register-level meaning of each
  //       instruction (e.g. `const 1 0 0` -> `r1 = pool[0]`)
  //   5 = level 4 + an end-of-compile register breakdown: for every register
  //       in the main program's frame, where the compiler's allocator left it
  //       (named var / dead-temp pool / dead-named pool / quarantine /
  //       deferred / live temp / call-arg slot / unallocated gap) plus a
  //       category summary, followed by a module breakdown (level 5 only): the
  //       module's name (SPEC §bytecodes "keyword export" — the source file's
  //       stem unless the program declares `export const MODULE_NAME`), the
  //       functions the `import` statements bound (module -> native slot ->
  //       call count), the module's export table (`export` -> kind -> function
  //       index or register), and the top-level `def`s the module defines
  //       without exporting (class methods/prototypes and closures are
  //       excluded from both lists)
  // ---------------------------------------------------------------------------

  // One `import`-bound function (module provenance from the compiler; see
  // QLangCompiler::ImportedModuleFunc). `slot` is the native slot the name
  // resolves to (-1 when the compiler never assigned one). `fromClass` marks
  // a class method or prototype function: it shares the compiler's
  // function-building path but is neither an import nor a main-module export.
  struct ModuleImport
  {
    std::string module;
    std::string func;
    int slot = -1;
    bool fromClass = false;
  };

  // Builtin id (CALL operand b, >= 0) -> name comes from the library table
  // (qlang::builtinName, qlang/QLangBytecode.h): the disassembler no longer
  // hardcodes the names (SPEC §bytecodes "table for build in function").

  // Shortest decimal text that reads back as the same double (1.5, 0.1,
  // 3e+100), so a float immediate prints exactly as many digits as it needs.
  std::string doubleText(double d)
  {
    char b[40];
    for (int prec = 1; prec <= 17; prec++)
    {
      std::snprintf(b, sizeof b, "%.*g", prec, d);
      if (std::strtod(b, nullptr) == d)
        break;
    }
    return std::string(b);
  }

  // The immediate right operand of a decoded instruction as text. The payload
  // says which field carries it and the form what it is (plan §2.3), so the
  // signed `Int` forms, the unsigned forms, the `float`, the zero-byte
  // `true`/`false`/`nil` and the `Long`/`Qword` forms all print distinctly.
  // Returns "?" when the instruction has no immediate (e.g. CALL, whose
  // function id is carried in `b`).
  std::string immediateText(const DecodedInstruction &ins)
  {
    switch (ins.immPayload)
    {
    case ImmPayload::Signed:
      return std::to_string(ins.imm);
    case ImmPayload::Unsigned:
      return std::to_string(ins.immUnsigned);
    case ImmPayload::Float:
      return doubleText(ins.immFloat);
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

  // Pseudo-mnemonic: the register-level meaning of one instruction (level 4),
  // e.g. "r1 = pool[0]" for `mov r1, pool[0]`.
  std::string pseudoText(const CompiledProgram &prog, const DecodedInstruction &ins)
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
    // The right operand as a register-level expression, selected by its kind
    // (plan §2.2): a register prints as a register, a pool index as pool[N], an
    // immediate as its value.
    auto right = [&rn, &ins]() -> std::string
    {
      switch (ins.rightKind)
      {
      case RightKind::Immediate:
        return immediateText(ins);
      case RightKind::ConstPool:
        return "pool[" + std::to_string(ins.b) + "]";
      case RightKind::Register:
      case RightKind::Reserved:
        break;
      }
      return rn(ins.b);
    };
    // The two source operands of a binary op. A right operand that is not a
    // register means the in-place form r[a] = r[a] op right (plan §2.6), so the
    // destination doubles as the left operand and `c` is not in the stream.
    auto sources = [&rn, &right, &ins](std::string &lhs, std::string &rhs)
    {
      if (isInPlaceForm(ins.op, ins.rightKind))
      {
        lhs = rn(ins.a);
        rhs = right();
      }
      else
      {
        lhs = rn(ins.b);
        rhs = rn(ins.c);
      }
    };
    auto bin = [&ss, &rn, &sources, &ins](const char *op)
    {
      std::string l, r;
      sources(l, r);
      ss << rn(ins.a) << " = " << l << " " << op << " " << r;
    };
    auto cmp = [&ss, &rn, &sources, &ins](const char *op)
    {
      std::string l, r;
      sources(l, r);
      ss << rn(ins.a) << " = (" << l << " " << op << " " << r << ")";
    };
    // The flag-setting compares (`eqf`..`gtef`) keep `cmp`'s operand shape --
    // the left operand is always r[a], the right one is selected by its kind
    // -- but they write the flags instead of a register, so neither `bin` nor
    // `cmp` fits: both print a destination that is never written. `sources()`
    // is wrong here as well: it implements the 3-operand ALU convention
    // (r[a] = r[b] op r[c]), while the §2.6 arity rule keeps `c` out of a
    // flag compare's stream.
    auto flagCmp = [&ss, &rn, &right, &ins](const char *op)
    {
      ss << "flags = (" << rn(ins.a) << " " << op << " " << right() << ")";
    };

    switch (ins.op)
    {
    // The copy family: the right operand is a register, a pool entry or an
    // immediate of any form (the modifier's kind field selects which).
    case Opcode::MOV:
      ss << rn(ins.a) << " = " << right();
      break;
    case Opcode::DEEPCOPY:
      ss << rn(ins.a) << " = deepcopy(" << right() << ")";
      break;
    case Opcode::CONV:
      ss << rn(ins.a) << " = convert(" << right() << ", " << type(ins.c) << ")";
      break;
    case Opcode::CAST:
      ss << rn(ins.a) << " = cast(" << right() << ", " << type(ins.c) << ")";
      break;
    case Opcode::NEG:
      ss << rn(ins.a) << " = -" << right();
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
    {
      std::string l, r;
      sources(l, r);
      ss << rn(ins.a) << " = truth(" << l << ") && truth(" << r << ")";
      break;
    }
    case Opcode::LOR:
    {
      std::string l, r;
      sources(l, r);
      ss << rn(ins.a) << " = truth(" << l << ") || truth(" << r << ")";
      break;
    }
    case Opcode::NOT:
      ss << rn(ins.a) << " = !truth(" << right() << ")";
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
    case Opcode::INx:
    {
      std::string l, r;
      sources(l, r);
      ss << rn(ins.a) << " = (" << l << " in " << r << ")";
      break;
    }
    case Opcode::CALL:
      if (ins.b >= 0)
      {
        const char *bn = builtinName(ins.b);
        ss << rn(ins.a) << " = " << (bn ? bn : "builtin?") << "(" << args(ins.a, ins.c) << ")";
      }
      else
        ss << rn(ins.a) << " = native" << (-1 - ins.b) << "(" << args(ins.a, ins.c) << ")";
      break;
    case Opcode::DELETEx:
      ss << "delete " << rn(ins.a);
      break;
    case Opcode::ARRGET:
    case Opcode::SEQGET:
    case Opcode::TABGET:
    case Opcode::INDGET:
    case Opcode::INDEXGET:
      ss << rn(ins.a) << " = " << rn(ins.b) << "[" << rn(ins.c) << "]";
      break;
    case Opcode::ARRPUT:
    case Opcode::TABPUT:
    case Opcode::INDPUT:
    case Opcode::INDEXPUT:
      ss << rn(ins.a) << "[" << rn(ins.b) << "] = " << rn(ins.c);
      break;
    case Opcode::ARRAPP:
    case Opcode::ARRAPPV:
      ss << rn(ins.a) << " += " << right();
      break;
    case Opcode::ARRLEN:
    case Opcode::LEN:
      ss << rn(ins.a) << " = len(" << right() << ")";
      break;
    case Opcode::TABCREATE:
      ss << rn(ins.a) << " = {}";
      break;
    case Opcode::TABFINALIZE:
      ss << "finalize(" << rn(ins.a) << ")";
      break;
    case Opcode::EVALKEY:
      ss << rn(ins.a) << " = str(" << right() << ")";
      break;
    case Opcode::JMP:
    case Opcode::CONTINUE:
    case Opcode::BREAK:
    case Opcode::JE:
    case Opcode::JNE:
    case Opcode::JLE:
    case Opcode::JGE:
    case Opcode::JG:
    case Opcode::JL:
      ss << "pc += " << ins.jumpOffset;
      break;
    case Opcode::Nop:
      ss << "noop";
      break;
    case Opcode::CMP:
      ss << "cmp " << rn(ins.a) << ", " << right();
      break;
    // The flag-flow family (SPEC §bytecodes "extended OpCode eq, lt, gt, lte,
    // gte" and "### OpCode tst"): the pseudo-mnemonic names the flags as the
    // destination, matching the spec's own listings.
    case Opcode::EQF:
      flagCmp("==");
      break;
    case Opcode::LTF:
      flagCmp("<");
      break;
    case Opcode::GTF:
      flagCmp(">");
      break;
    case Opcode::LTEF:
      flagCmp("<=");
      break;
    case Opcode::GTEF:
      flagCmp(">=");
      break;
    case Opcode::TST:
      ss << "flags = truth(" << rn(ins.a) << ") ? eq : 0";
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
    case Opcode::EMIT:
      ss << "emit " << rn(ins.a) << ".pool[" << ins.b << "](" << args(ins.a, ins.c) << ")";
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

  // Comma-separated operands for one decoded instruction, mirroring what the
  // encoder wrote (plan §2.2, §2.6): the left register, the right operand
  // rendered through its kind (register index / pool[0xNN] / immediate of any
  // form), and the third operand only when the instruction carries one.
  // Registers and pool indices are hex (negative b is decimal: native-call
  // index, -1 - id); immediates are decimal; jumps show the signed relative
  // offset plus the absolute target byte offset. Escaped forms are
  // prefixed with the modifier byte (e=0xNN).
  std::string operandText(const DecodedInstruction &ins, size_t at)
  {
    auto fmt = [](int v)
    {
      char b[12];
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
    auto addText = [&s](const std::string &t)
    {
      if (!s.empty())
        s += ", ";
      s += t;
    };
    if (ins.escaped)
    {
      char m[8];
      std::snprintf(m, sizeof m, "e=0x%02x", ins.modifier);
      s += m;
    }
    if (isJumpOpcode(ins.op))
    {
      add(ins.jumpOffset);
      char t[16];
      std::snprintf(t, sizeof t, " -> %.4x",
                    static_cast<unsigned>(at + ins.size + ins.jumpOffset));
      s += t;
      return s;
    }
    // CALL names its operands instead of reading them through the kind: its
    // right operand is the signed function id carried in `b` (negative = native
    // call, -1 - id) and `c` is the argument count (plan §2.8).
    if (ins.op == Opcode::CALL)
    {
      add(ins.a);
      add(ins.b);
      add(ins.c);
      return s;
    }
    size_t nOps = baseOperandCount(ins.op);
    if (nOps == 0)
      return s; // `nop` carries no operand at all
    add(ins.a);
    if (nOps >= 2)
    {
      switch (ins.rightKind)
      {
      case RightKind::Immediate:
        addText(immediateText(ins));
        break;
      case RightKind::ConstPool:
        addText("pool[" + fmt(ins.b) + "]");
        break;
      case RightKind::Register:
      case RightKind::Reserved:
        add(ins.b);
        break;
      }
    }
    // The third operand is in the stream only when the opcode has one and the
    // right operand did not collapse it into the in-place form.
    if (nOps == 3 && !isInPlaceForm(ins.op, ins.rightKind))
      add(ins.c);
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
      return "<" + std::to_string(v.asBlob()->size()) + " bytes>";
    case T::Array:
    case T::Set:
    {
      if (depth)
        return "[...]";
      std::string s = "[";
      const auto &a = *v.asArray();
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

  // `libs` are the library images a linked program was merged from: level 5
  // prints one block per module of `prog.initSequence`, which needs each
  // image's own export table (its refs are module-local, the merged ones come
  // from the linker's bases).
  std::string disassemble(const CompiledProgram &prog, const std::vector<std::string> &srcLines,
                          int level, const RegisterReport *report = nullptr,
                          const std::vector<ModuleImport> &imports = {},
                          const std::vector<std::shared_ptr<LoadedModule>> &libs = {})
  {
    std::ostringstream ss;
    if (level >= 1)
    {
      ss << "QLang disassembly (v1)\n";
      ss << "registers: " << prog.registerCount << "\n\n";
      // The module constant pool (SPEC §bytecodes "unified constant table"):
      // one table shared by every function, so it is printed once for the
      // program rather than once per function.
      ss << "constants:\n";
      for (size_t j = 0; j < prog.constants.size(); j++)
        ss << "    [" << j << "] " << valueText(prog.constants[j]) << "\n";
      ss << "\n";
    }
    // Widest operand text and (level 4) pseudo-mnemonic in the whole
    // listing, so the operand, "; ...", and "# ..." columns align on
    // every line.
    size_t maxOperand = 0;
    size_t maxPseudo = 0;
    for (const auto &fn : prog.functions)
      for (size_t j = 0; j < fn.instrOffsets.size(); j++)
      {
        DecodedInstruction k;
        decodeInstruction(fn.code.data(), fn.instrOffsets[j], fn.code.size(), k);
        maxOperand = std::max(maxOperand, operandText(k, fn.instrOffsets[j]).size());
        if (level >= 4)
          maxPseudo = std::max(maxPseudo, pseudoText(prog, k).size());
      }
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
            ss << "    r" << reg << "  " << name << "\n";
        }
        ss << "  code:\n";
      }
      else
        ss << "function #" << i << " '" << f.name << "':\n";

      int lastLine = -1;
      for (size_t j = 0; j < f.instrOffsets.size(); j++)
      {
        DecodedInstruction ins;
        decodeInstruction(f.code.data(), f.instrOffsets[j], f.code.size(), ins);
        const uint16_t insLine = (j < f.loc.size()) ? f.loc[j].first : 0;
        if (level >= 2 && insLine != lastLine && insLine >= 1 &&
            static_cast<size_t>(insLine) <= srcLines.size())
          ss << "\n  L" << insLine << ":  "
             << srcLines[static_cast<size_t>(insLine) - 1] << "\n";
        lastLine = insLine;
        // Level 3: append a copy of the referenced constant. Operand b names a
        // pool entry whenever the instruction's right kind is `ConstPool`
        // (`mov` kind 2 / `makefunc` / `makegen`, plan §2.5); `emit` is the
        // one opcode whose b is a pool index though its kind stays register.
        std::string constNote;
        if (level >= 3)
        {
          bool poolOperand = ins.rightKind == RightKind::ConstPool;
          if (ins.op == Opcode::EMIT)
            poolOperand = true;
          if (poolOperand && ins.b >= 0 &&
              static_cast<size_t>(ins.b) < prog.constants.size())
            constNote = valueText(prog.constants[static_cast<size_t>(ins.b)]);
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
        snprintf(addr, sizeof(addr), "%.4x", static_cast<unsigned>(f.instrOffsets[j]));

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
           << "  " << std::setw(10) << operandText(ins, f.instrOffsets[j])
           << pseudo << note << "\n";
      }
      ss << "\n";
    }
    // Level 5: end-of-compile register breakdown (main program allocator).
    // The snapshot is taken after the last top-level statement, so live
    // temps, slot ranges, and the quarantine are normally all closed; what
    // usually remains is named vars, the dead-temp pool, the dead-named
    // pool, and any deferred temps.
    if (level >= 5 && report)
    {
      auto inVec = [](int r, const std::vector<int> &v)
      { return std::find(v.begin(), v.end(), r) != v.end(); };
      auto inSet = [](int r, const std::set<int> &s) { return s.count(r) != 0; };
      // 0 named, 1 free (temp), 2 free (named), 3 quarantine, 4 deferred,
      // 5 live temp, 6 call-arg slot, 7 unallocated
      int counts[8] = {0};
      ss << "\n";
      ss << "================================================================\n";
      ss << "register breakdown @ end of compile (main, " << report->registerCount
         << " registers)\n";
      ss << "================================================================\n";
      for (int r = 0; r < report->registerCount; r++)
      {
        const char *label = "unallocated";
        std::string extra = "gap";
        auto nit = report->named.find(r);
        if (inSet(r, report->namedRegs) || nit != report->named.end())
        {
          label = "named";
          counts[0]++;
          extra = (nit != report->named.end())
                      ? ("'" + nit->second + "'" +
                         (prog.constVars.count(nit->second) ? " (const)" : ""))
                      : "";
        }
        else if (inSet(r, report->quarantined))
        {
          label = "quarantine";
          counts[3]++;
          extra = "call-arg slot";
        }
        else if (inSet(r, report->deferred))
        {
          label = "deferred";
          counts[4]++;
          extra = "dead temp below high temp";
        }
        else if (inVec(r, report->freeRegs))
        {
          label = "free pool";
          counts[1]++;
          extra = "dead temp";
        }
        else if (inVec(r, report->freeNamedRegs))
        {
          label = "free pool";
          counts[2]++;
          extra = "dead named";
        }
        else if (inVec(r, report->liveTmps))
        {
          label = "live temp";
          counts[5]++;
          extra = inSet(r, report->protectedTmps) ? "protected" : "";
        }
        else
        {
          for (const auto &rng : report->slotRanges)
            if (r >= rng.first && r < rng.second)
            {
              label = "call-arg slot";
              counts[6]++;
              extra = "";
              break;
            }
        }
        if (label == "unallocated")
          counts[7]++;
        ss << "  r" << std::setw(3) << r << "  " << std::left << std::setw(14) << label
           << (extra.empty() ? "" : "  " + extra) << "\n";
      }
      ss << "  summary: " << counts[0] << " named, " << counts[1] << " free pool (dead temp), "
         << counts[2] << " free pool (dead named), " << counts[3] << " quarantine, "
         << counts[4] << " deferred, " << counts[5] << " live temp, " << counts[6]
         << " call-arg slot, " << counts[7] << " unallocated\n";
      ss << "  high water: m_nextReg=" << report->registerCount
         << "  m_nextNamedReg=" << report->nextNamedReg << "\n";
    }
    // Level 5: module breakdown. Imports are the names the `import`
    // statements bound (module provenance + native slot from the driver);
    // exports are the main module's top-level `def`s (no `export` syntax
    // exists yet, so this lists what the main module defines and could hand
    // to another module). Call counts are scanned from the instruction
    // streams: a native call is `call rA, -1-slot, argc` (negative b), a
    // user call `callfn rA, fnIdx, argc`; `emit` dispatches a method through
    // a function index in b, so it counts toward the callee too.
    if (level >= 5)
    {
      std::map<int, int> nativeCalls; // native slot -> CALL count
      std::map<int, int> fnCalls;     // function index -> CALLFN/EMIT count
      for (const auto &f : prog.functions)
        for (size_t j = 0; j < f.instrOffsets.size(); j++)
        {
          DecodedInstruction k;
          decodeInstruction(f.code.data(), f.instrOffsets[j], f.code.size(), k);
          if (k.op == Opcode::CALL && k.b < 0)
            nativeCalls[-1 - k.b]++;
          else if ((k.op == Opcode::CALLFN || k.op == Opcode::EMIT) && k.b >= 0)
            fnCalls[k.b]++;
        }
      std::unordered_set<std::string> classFuncs; // class methods/prototypes
      // Names bound from a loaded `.ql` image: they are not natives, so the
      // driver's slot lookup has nothing to say about them — the import table
      // does (after a link its `ref` is the merged function index or cell).
      std::map<std::string, const ImportEntry *> qlNames;
      for (const auto &ie : prog.imports)
        qlNames[ie.name] = &ie;
      std::vector<std::string> importLines;
      std::unordered_set<std::string> listed, listedSlots;
      for (const auto &im : imports)
      {
        if (im.fromClass)
        {
          classFuncs.insert(im.func);
          continue;
        }
        if (!listed.insert(im.func).second)
          continue; // first binding wins (mirrors the compiler)
        std::ostringstream line;
        line << "    " << std::left << std::setw(16) << im.func << " <- "
             << std::setw(12) << im.module;
        if (im.slot >= 0)
        {
          listedSlots.insert(std::to_string(im.slot));
          auto cit = nativeCalls.find(im.slot);
          line << "native slot " << std::setw(4) << im.slot << "("
               << (cit == nativeCalls.end() ? 0 : cit->second) << " calls)";
        }
        else
        {
          auto qit = qlNames.find(im.func);
          if (qit != qlNames.end())
            line << (qit->second->kind == ExportKind::Func ? "fn#" : "cell r")
                 << qit->second->ref << " (loaded .ql module)";
          else
            line << "(not referenced)";
        }
        importLines.push_back(line.str());
      }
      std::sort(importLines.begin(), importLines.end());
      ss << "\n";
      ss << "================================================================\n";
      ss << "module breakdown (" << prog.moduleName << ")\n";
      ss << "================================================================\n";
      ss << "  module: " << prog.moduleName << "\n";
      ss << "  imports:\n";
      if (importLines.empty())
        ss << "    (none)\n";
      for (const auto &l : importLines)
        ss << l << "\n";
      // Native slots used by the program that no import line covered: a
      // host-registered function called without `import`, or a .qlc image
      // whose provenance is the slot table itself.
      for (const auto &[slot, count] : nativeCalls)
      {
        if (listedSlots.count(std::to_string(slot)))
          continue;
        std::string name = "?";
        for (const auto &im : imports)
          if (im.slot == slot)
            name = im.func;
        ss << "    " << std::left << std::setw(16) << name << " <- "
           << std::setw(12) << "?"
           << "native slot " << std::setw(4) << slot << "(" << count << " calls)\n";
      }
      // The module's export table (SPEC §bytecodes "export table (lookup)"):
      // the names `export` published and what each resolves to — a function
      // index for a `def`, the register (the shared cell of SPEC §bytecodes
      // "technical specifications" 3) for a variable or constant.
      std::unordered_set<std::string> exportedNames;
      for (const auto &e : prog.exports)
        exportedNames.insert(e.name);
      ss << "  exports:\n";
      if (prog.exports.empty())
        ss << "    (none)\n";
      for (const auto &e : prog.exports)
      {
        const char *kind = e.kind == ExportKind::Func
                               ? "fn"
                               : (e.kind == ExportKind::Const ? "const" : "var");
        ss << "    " << std::left << std::setw(19) << e.name << std::setw(6)
           << kind;
        if (e.kind == ExportKind::Func)
        {
          auto cit = fnCalls.find(e.ref);
          const std::string fname = (e.ref >= 0 &&
                                     static_cast<size_t>(e.ref) < prog.functions.size())
                                        ? prog.functions[e.ref].name
                                        : "?";
          const int params = (e.ref >= 0 &&
                              static_cast<size_t>(e.ref) < prog.functions.size())
                                 ? prog.functions[e.ref].paramCount
                                 : 0;
          ss << "fn#" << e.ref << "  params " << std::setw(3) << params << "("
             << (cit == fnCalls.end() ? 0 : cit->second) << " calls)  " << fname;
        }
        else
        {
          ss << "r" << e.ref << "  "
             << (prog.constVars.count(e.name) ? "readonly" : "shared cell");
        }
        ss << "\n";
      }
      // Top-level `def`s the module defines but did not export: still visible
      // inside the module, invisible to an importer. A linked program's function
      // table also holds the merged libraries, so the scan stops where the first
      // one begins (the linker concatenates in run order).
      int entryFuncEnd = static_cast<int>(prog.functions.size());
      if (!prog.initSequence.empty())
        entryFuncEnd = prog.initSequence.front().funcIndex;
      ss << "  unexported defs:\n";
      bool anyExport = false;
      for (size_t i = 1; i < static_cast<size_t>(entryFuncEnd); i++)
      {
        const auto &f = prog.functions[i];
        if (f.name == "closure" || f.name == "ternary" || f.isGenerator ||
            classFuncs.count(f.name) || exportedNames.count(f.name))
          continue; // closures, generator bodies and class members are not
                    // top-level defs of the module
        anyExport = true;
        auto cit = fnCalls.find(static_cast<int>(i));
        ss << "    " << std::left << std::setw(19) << f.name << "fn#" << i
           << "  params " << std::setw(3) << f.paramCount << "("
           << (cit == fnCalls.end() ? 0 : cit->second) << " calls)\n";
      }
      if (!anyExport)
        ss << "    (none)\n";
      // Names bound from a loaded `.ql` module (SPEC §bytecodes "import table
      // (lookup)"). After a link every `ref` is a merged handle, so the listing
      // shows the defining module's name next to the number the code uses.
      if (!prog.imports.empty())
      {
        ss << "  imported .ql names:\n";
        for (const auto &im : prog.imports)
        {
          const char *kind = im.kind == ExportKind::Func
                                 ? "fn"
                                 : (im.kind == ExportKind::Const ? "const" : "var");
          ss << "    " << std::left << std::setw(19) << im.name << std::setw(6)
             << kind << im.module << "." << im.name << " -> ";
          if (im.kind == ExportKind::Func)
          {
            const bool ok = im.ref >= 0 &&
                            static_cast<size_t>(im.ref) < prog.functions.size();
            auto cit = fnCalls.find(im.ref);
            ss << "fn#" << im.ref << "  params " << std::setw(3)
               << (ok ? prog.functions[im.ref].paramCount : 0) << "("
               << (cit == fnCalls.end() ? 0 : cit->second) << " calls)  "
               << (ok ? prog.functions[im.ref].name : "?");
          }
          else
            ss << "r" << im.ref;
          ss << "\n";
        }
      }
      ss << "\n";
      // One block per module the linker merged in (plan v4 Phase 3b): where its
      // code and its slice of the shared global register area landed, and what
      // it publishes — each export shown as the module-local ref the image
      // stores and the merged ref the running program uses.
      if (!prog.initSequence.empty())
      {
        ss << "================================================================\n";
        ss << "linked modules (" << prog.initSequence.size() << ")\n";
        ss << "================================================================\n";
        for (const ModuleInit &mi : prog.initSequence)
        {
          const LoadedModule *lm = nullptr;
          for (const auto &sh : libs)
            if (sh && sh->program.moduleName == mi.module)
              lm = sh.get();
          ss << "  module: " << mi.module << "\n";
          ss << "    init: fn#" << mi.funcIndex << "  registers " << mi.regBase << ".."
             << (mi.regBase + mi.registerCount - 1) << "\n";
          if (!lm)
          {
            ss << "    (image not available)\n\n";
            continue;
          }
          const CompiledProgram &mp = lm->program;
          ss << "    exports:\n";
          if (mp.exports.empty())
            ss << "      (none)\n";
          for (const auto &e : mp.exports)
          {
            const char *kind = e.kind == ExportKind::Func
                                   ? "fn"
                                   : (e.kind == ExportKind::Const ? "const" : "var");
            ss << "      " << std::left << std::setw(19) << e.name << std::setw(6) << kind
               << "r/f" << e.ref << " -> ";
            if (e.kind == ExportKind::Func)
            {
              const int merged = mi.funcIndex + e.ref;
              const bool ok = e.ref >= 0 &&
                              static_cast<size_t>(e.ref) < mp.functions.size();
              ss << "fn#" << merged << "  params " << std::setw(3)
                 << (ok ? mp.functions[e.ref].paramCount : 0) << "  "
                 << (ok ? mp.functions[e.ref].name : "?");
            }
            else
              ss << "r" << mi.regBase + e.ref;
            ss << "\n";
          }
          ss << "    imports:\n";
          if (mp.imports.empty())
            ss << "      (none)\n";
          for (const auto &im : mp.imports)
            ss << "      " << std::left << std::setw(19) << im.name << "from "
               << im.module << "\n";
          ss << "    natives:\n";
          if (lm->nativeSlots.empty())
            ss << "      (none)\n";
          for (const auto &[slot, name] : lm->nativeSlots)
            ss << "      " << std::left << std::setw(19) << name << "slot " << slot
               << "\n";
          ss << "\n";
        }
      }
    }
    return ss.str();
  }

  // ---------------------------------------------------------------------------
  // Module loading for --import and the positional library arguments
  // (SPEC §qlang-run). An argument names a module, not a file: its current .qlc
  // is loaded, and its .ql is otherwise compiled, saved beside the source (never
  // rotated) and then loaded.
  // ---------------------------------------------------------------------------

  // What the loader needs to know from the command line.
  struct ModuleLoader
  {
    std::vector<std::string> searchPaths;  // where a bare module name is found
    int traceLevel = TRACE_OFF;            // trace level of a scratch context
    int asmLevel = 1;                      // detail of a generated .qla
    bool listing = false;                  // -a: every image gets a .qla
  };

  // Why a module could not be loaded, and the exit code the driver reports: 2
  // when a file the argument names is simply not there, 3 for anything the
  // engine rejected (a malformed image, a compile error, a duplicate name).
  struct LoadFailure
  {
    std::string message;
    int exitCode = 3;
  };

  std::shared_ptr<LoadedModule> ensureModule(
      QLangContext &target, const std::string &spec,
      const ModuleLoader &loader, LoadFailure &failure);

  // Bring a module's own dependencies into `target` too. The engine does this for
  // a module it discovers itself (QLangContext::findModule walks the image's
  // import table), but a module the driver hands over through the search
  // callback is registered as-is, and QLangLinker needs every module a merged
  // image refers to present. Each dependency is resolved by the same rule as its
  // dependent: a current image, otherwise compile the source.
  bool resolveImportedModules(
      QLangContext &target, const std::shared_ptr<LoadedModule> &mod,
      const ModuleLoader &loader, LoadFailure &failure)
  {
    std::unordered_set<std::string> seen;
    for (const ImportEntry &imp : mod->program.imports)
    {
      if (imp.module.empty() || !seen.insert(imp.module).second)
        continue;
      // A C++ module the host registered is bound as natives, never as an image
      // (and never appears in an image's import table, but do not rely on it).
      bool native = false;
      for (const auto &m : target.modules())
        if (m.name == imp.module)
          native = true;
      if (native || target.loadedModule(imp.module))
        continue;
      if (!ensureModule(target, imp.module, loader, failure))
      {
        failure.message =
            "module '" + mod->program.moduleName + "' imports " + failure.message;
        return false;
      }
    }
    return true;
  }

  // Compile one library source into its .qlc image (and, with -a, a .qla listing
  // beside it).
  //
  // The compile runs on a private context, not on the run context: a library's
  // top level declares globals and the global table belongs to the context, so
  // compiling there would leak them into the entry's global area. The private
  // context gets the same search paths plus a search callback that compiles a
  // dependency's own `.ql` on the spot — a library may depend on a library that
  // has no image yet.
  //
  // Library mode is what makes the result loadable at all: the top level becomes
  // `<init>` and the module has to be namable — its own `export const
  // MODULE_NAME`, or the name the driver is writing the file under.
  bool compileLibrary(const std::string &qlPath, const std::string &qlcPath,
                      const ModuleLoader &loader, std::string &err)
  {
    std::ifstream in(qlPath, std::ios::binary);
    if (!in)
    {
      err = "cannot open module source '" + qlPath + "'";
      return false;
    }
    std::ostringstream src;
    src << in.rdbuf();
    const std::string source = src.str();

    QLangContext scratch;
    scratch.setVerboseLevel(loader.traceLevel);
    scratch.setModuleSearchPaths(loader.searchPaths);
    scratch.setModuleSearchCallback(
        [&scratch, &loader](const std::string &name, std::string &error)
            -> std::shared_ptr<LoadedModule>
        {
          LoadFailure failure;
          std::shared_ptr<LoadedModule> mod =
              ensureModule(scratch, name, loader, failure);
          // The contract is "must not throw": report through `error` instead
          // (QLangContext.h, "external search callback").
          if (!mod && error.empty())
            error = failure.message;
          return mod;
        });

    std::vector<StmtPtr> ast;
    try
    {
      ast = scratch.parse(source);
    }
    catch (const QLangError &e)
    {
      err = qlPath + ": " + e.what();
      return false;
    }

    QLangCompiler compiler(std::move(ast), &scratch);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : scratch.nativeFuncs())
      nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(scratch.modules());
    compiler.setModuleName(moduleStem(qlPath));
    compiler.setLibraryMode(true);

    CompiledProgram prog;
    try
    {
      prog = compiler.compile();
    }
    catch (const QLangError &e)
    {
      err = qlPath + ": " + e.what();
      return false;
    }

    // Wire the natives exactly as the entry's compile does, so the image's
    // native-slot table names functions this (or any) context can re-resolve.
    for (const auto &[name, func] : scratch.nativeFuncs())
    {
      auto it = compiler.nativeFuncIds().find(name);
      if (it == compiler.nativeFuncIds().end())
        continue;
      const int idx = -1 - it->second;
      while (prog.nativeFuncs.size() <= static_cast<size_t>(idx))
        prog.nativeFuncs.push_back(nullptr);
      prog.nativeFuncs[static_cast<size_t>(idx)] = func;
    }
    const auto &resolved = compiler.nativeFuncsResolved();
    for (size_t i = 0; i < resolved.size(); i++)
    {
      while (prog.nativeFuncs.size() <= i)
        prog.nativeFuncs.push_back(nullptr);
      if (resolved[i])
        prog.nativeFuncs[i] = resolved[i];
    }

    // The codec wants the slot table ordered by name (docs/SERIALIZATION.md
    // §4.3); the compiler keeps it hashed.
    const std::map<std::string, int> nativeIds(compiler.nativeFuncIds().begin(),
                                               compiler.nativeFuncIds().end());
    std::ostringstream os(std::ios::binary);
    if (!writeQlc(os, prog, nativeIds))
    {
      err = qlPath + ": cannot build a .qlc image for module '" + prog.moduleName + "'";
      return false;
    }
    const std::string bytes = os.str();
    if (!writeBinaryFile(qlcPath, bytes, err))
      return false;
    std::fprintf(stderr, "module compiled: %s -> %s (%zu bytes, %zu functions)\n",
                 qlPath.c_str(), qlcPath.c_str(), bytes.size(),
                 prog.functions.size());

    // SPEC §qlang-run: "if -a is defined then a disassembly listing is generated
    // for all generated qlc files" — beside the image, never in --output-path.
    if (loader.listing)
    {
      std::vector<std::string> srcLines;
      std::istringstream ls(source);
      std::string line;
      while (std::getline(ls, line))
        srcLines.push_back(line);
      std::vector<ModuleImport> imports;
      for (const auto &imp : compiler.importedModuleFuncs())
      {
        int slot = -1;
        auto nit = compiler.nativeFuncIds().find(imp.func);
        if (nit != compiler.nativeFuncIds().end())
          slot = -1 - nit->second;
        imports.push_back({imp.module, imp.func, slot, imp.fromClass});
      }
      const std::string text =
          disassemble(prog, srcLines, loader.asmLevel,
                      &compiler.registerReport(), imports,
                      scratch.loadedModules());
      const std::string listingPath =
          (std::filesystem::path(moduleDirectory(qlcPath)) /
           (moduleStem(qlcPath) + ".qla"))
              .string();
      if (!writeBinaryFile(listingPath, text, err))
        return false;
      std::fprintf(stderr, "disassembly written: %s (%zu functions)\n", listingPath.c_str(),
                   prog.functions.size());
    }
    return true;
  }

  // Load one module into `target`, building its image first when it is missing,
  // unreadable, of another .qlc version, or older than its source.
  std::shared_ptr<LoadedModule> ensureModule(
      QLangContext &target, const std::string &spec,
      const ModuleLoader &loader, LoadFailure &failure)
  {
    ModuleFiles files;
    std::string err;
    if (!resolveModuleFiles(spec, loader.searchPaths, files, err))
    {
      failure = {err, 2};
      return nullptr;
    }
    // The image always lives beside the source, under the name the file implies.
    // It is never rotated: -r is for the entry's artifacts, and a rotated
    // dependency image could never be found again by name.
    const std::string home =
        !files.qlc.empty() ? files.qlc : files.ql;
    const std::string image =
        (std::filesystem::path(moduleDirectory(home)) /
         (files.name + ".qlc"))
            .string();

    // Already taken in from this exact file (the same module named twice, or a
    // dependency shared by two libraries): reuse it, since loading a module name
    // twice is an error.
    for (const auto &sh : target.loadedModules())
      if (!sh->path.empty() && samePath(sh->path, image))
        return sh;

    std::shared_ptr<LoadedModule> mod;
    if (imageIsCurrent(image, files.ql))
    {
      try
      {
        mod = target.loadProgramFile(image);
      }
      catch (const QLangError &e)
      {
        failure = {image + ": " + e.what(), 3};
        return nullptr;
      }
    }
    else
    {
      if (files.ql.empty())
      {
        failure = {image + ": not a current .qlc image and no '" + files.name +
                       ".ql' to compile",
                   3};
        return nullptr;
      }
      if (!compileLibrary(files.ql, image, loader, err))
      {
        failure = {err, 3};
        return nullptr;
      }
      try
      {
        mod = target.loadProgramFile(image);
      }
      catch (const QLangError &e)
      {
        failure = {image + ": " + e.what(), 3};
        return nullptr;
      }
    }
    if (!resolveImportedModules(target, mod, loader, failure))
      return nullptr;
    return mod;
  }

  // Profile report as plain text — the same lines the --profile flag prints
  // to stderr — so the block can also be appended to the .qla listing when
  // both --profile and --asm are given.
  std::string profileText(double parseMs, size_t stmtCount, double compileMs,
                          size_t instrCount, size_t constCount, size_t funcCount,
                          double executeMs, double totalMs)
  {
    char buf[160];
    std::string s;
    snprintf(buf, sizeof buf, "profile: parse    %.3f ms  (%zu statements)\n", parseMs,
             stmtCount);
    s += buf;
    snprintf(buf, sizeof buf,
             "profile: compile  %.3f ms  (%zu instructions, %zu constants, %zu functions)\n",
             compileMs, instrCount, constCount, funcCount);
    s += buf;
    snprintf(buf, sizeof buf, "profile: execute  %.3f ms  (%zu instructions)\n", executeMs,
             instrCount);
    s += buf;
    snprintf(buf, sizeof buf, "profile: total    %.3f ms\n", totalMs);
    s += buf;
    return s;
  }

} // namespace

int main(int argc, char **argv)
{
  int traceLevel = TRACE_OFF;
  bool profile = false;
  bool rotate = false;
  int asmLevel = 1;
  // -c / -a: save the entry's image and/or its disassembly listing. The names
  // are derived (SPEC §qlang-run), only the directory is given (--output-path).
  bool saveImage = false;
  bool saveListing = false;
  std::string outputPath;
  // --qlmeta-dump: standalone manifest extraction mode (portable-module-plan
  // P3) — writes this binary's QL_OBJECT registry and exits; see --help.
  std::string qlmetaDump;
  // --qlmeta: QLM1 manifest to validate the .ql entry against at compile
  // time (portable-module-plan P4); see --help.
  std::string qlmetaFile;
  // --package: QPM1 portable module to write after the entry is compiled
  // (portable-module-plan P5); see --help.
  std::string packageFile;
  std::string file;
  // Library modules the entry may `import` from (--import, extra positional
  // arguments). Resolved into the context's module registry before the entry is
  // compiled, so the compiler can resolve against their export tables.
  std::vector<std::string> moduleFiles;
  // Directories `import <name>` searches after the registry (--module-path),
  // and the switch that turns file discovery off entirely (--no-module-search).
  std::vector<std::string> modulePaths;
  bool noModuleSearch = false;

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

    // -r is the short form of --rotate (SPEC §qlang-run).
    if (arg == "--rotate" || arg == "-r")
    {
      rotate = true;
      continue;
    }

    if (arg == "-c")
    {
      saveImage = true;
      continue;
    }

    if (arg == "-a")
    {
      saveListing = true;
      continue;
    }

    if (arg == "--output-path")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --output-path requires a directory\n", kAppName);
        return 1;
      }
      outputPath = argv[++i];
      continue;
    }

    if (arg.rfind("--output-path=", 0) == 0)
    {
      outputPath = arg.substr(14);
      continue;
    }

    if (arg == "--qlmeta-dump")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --qlmeta-dump requires a .qlm file\n", kAppName);
        return 1;
      }
      qlmetaDump = argv[++i];
      continue;
    }

    if (arg.rfind("--qlmeta-dump=", 0) == 0)
    {
      qlmetaDump = arg.substr(14);
      continue;
    }

    if (arg == "--qlmeta")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --qlmeta requires a .qlm file\n", kAppName);
        return 1;
      }
      qlmetaFile = argv[++i];
      continue;
    }

    if (arg.rfind("--qlmeta=", 0) == 0)
    {
      qlmetaFile = arg.substr(9);
      continue;
    }

    if (arg == "--package")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --package requires a .qpm file\n", kAppName);
        return 1;
      }
      packageFile = argv[++i];
      continue;
    }

    if (arg.rfind("--package=", 0) == 0)
    {
      packageFile = arg.substr(10);
      continue;
    }

    if (arg == "--asm-level")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --asm-level requires a value (0-5)\n", kAppName);
        return 1;
      }
      if (!parseAsmLevel(argv[++i], asmLevel))
      {
        std::fprintf(stderr, "%s: invalid --asm-level '%s' (expected 0, 1, 2, 3, 4, or 5)\n",
                     kAppName, argv[i]);
        return 1;
      }
      continue;
    }

    if (arg.rfind("--asm-level=", 0) == 0)
    {
      if (!parseAsmLevel(arg.substr(12), asmLevel))
      {
        std::fprintf(stderr, "%s: invalid --asm-level '%s' (expected 0, 1, 2, 3, 4, or 5)\n",
                     kAppName, arg.c_str());
        return 1;
      }
      continue;
    }

    if (arg == "--import")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --import requires a .qlc module file\n", kAppName);
        return 1;
      }
      moduleFiles.push_back(argv[++i]);
      continue;
    }

    if (arg.rfind("--import=", 0) == 0)
    {
      moduleFiles.push_back(arg.substr(9));
      continue;
    }

    if (arg == "--module-path")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "%s: --module-path requires a directory\n", kAppName);
        return 1;
      }
      modulePaths.push_back(argv[++i]);
      continue;
    }

    if (arg.rfind("--module-path=", 0) == 0)
    {
      modulePaths.push_back(arg.substr(14));
      continue;
    }

    if (arg == "--no-module-search")
    {
      noModuleSearch = true;
      continue;
    }

    if (!arg.empty() && arg[0] == '-' && arg != "-")
    {
      std::fprintf(stderr, "%s: unknown option '%s' (see --help)\n", kAppName, arg.c_str());
      return 1;
    }

    if (!file.empty())
    {
      // Extra positional arguments are library modules — the same as --import
      // (SPEC §qlang-run): a bare name or a .ql/.qlc path. A `.ql` is compiled
      // to its image first, which is what the old "library modules must be .qlc
      // images" rule required the caller to do by hand.
      moduleFiles.push_back(arg);
      continue;
    }
    file = arg;
  }

  // --- --qlmeta-dump: standalone manifest extraction (portable-module-plan
  // P3). Writes this binary's own QL_OBJECT registry as a QLM1 manifest and
  // exits — no script is read, no context is created. Hosts that are not
  // qlang_run call qlang::writeTypeManifest themselves; this flag makes the
  // editor's own binary its own dumper.
  if (!qlmetaDump.empty())
  {
    if (!file.empty() || !moduleFiles.empty() || saveImage || saveListing ||
        !qlmetaFile.empty() || !packageFile.empty())
    {
      std::fprintf(stderr,
                   "%s: --qlmeta-dump does not combine with script/compile modes\n",
                   kAppName);
      return 1;
    }
    std::ofstream out(qlmetaDump, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      std::fprintf(stderr, "%s: cannot open manifest file '%s'\n", kAppName,
                   qlmetaDump.c_str());
      return 4;
    }
    try
    {
      writeTypeManifest(out);
    }
    catch (const QLangError &e)
    {
      std::fprintf(stderr, "%s: %s\n", kAppName, e.message().c_str());
      return 3;
    }
    std::cout << "wrote " << metaRegistry().size() << " type(s) to "
              << qlmetaDump << "\n";
    return 0;
  }

  // --qlmeta: read the QLM1 manifest (untrusted input — readManifest's
  // guards apply) once; the .ql compile path below hands it to the compiler.
  // An already-compiled .qlc input carries its own image and is not
  // re-checked (portable-module-plan P4).
  std::vector<ManifestType> extTypes;
  if (!qlmetaFile.empty())
  {
    std::ifstream in(qlmetaFile, std::ios::binary);
    if (!in)
    {
      std::fprintf(stderr, "%s: cannot open manifest file '%s'\n", kAppName,
                   qlmetaFile.c_str());
      return 1;
    }
    try
    {
      extTypes = readManifest(in);
    }
    catch (const QLangError &e)
    {
      std::fprintf(stderr, "%s: %s\n", kAppName, e.message().c_str());
      return 1;
    }
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

  // --- Module search paths ---
  // The context starts with the working directory (SPEC §api "module search
  // paths"); the entry file's own directory goes in front of it,
  // --no-module-search drops both, and --module-path then appends, so
  // `--no-module-search --module-path X` searches X and nothing else. A
  // directory that is not there is skipped by the resolver, so a typo is
  // warned about rather than made fatal — the run may still find everything.
  if (noModuleSearch)
    ctx.setModuleSearchPaths({});
  // The entry script's own directory goes first: `import mymod` in a script
  // that sits beside mymod.ql is what its author means, and the working
  // directory is wherever the host happened to start.
  if (!noModuleSearch)
  {
    const std::string entryDir = moduleDirectory(file);
    if (!entryDir.empty() && entryDir != ".")
    {
      std::vector<std::string> paths = ctx.moduleSearchPaths();
      paths.insert(paths.begin(), entryDir);
      ctx.setModuleSearchPaths(paths);
    }
  }
  for (const std::string &p : modulePaths)
  {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec))
      std::fprintf(stderr, "%s: warning: module path '%s' is not a directory, skipped\n",
                   kAppName, p.c_str());
    ctx.addModuleSearchPath(p);
  }

  // --- Artifacts: where -c and -a write (SPEC §qlang-run) ---
  // The names are derived from the entry file, never given: <stem>.qlc and
  // <stem>.qla. --output-path says which directory; without it the entry's own
  // directory is used ("the full qualified path of the original module"). -r
  // rotates these two names only — an image --import generates is found again
  // by name, so rotating it would hide it.
  const std::string outDir =
      outputPath.empty() ? moduleDirectory(file) : outputPath;
  if ((saveImage || saveListing) && !outputPath.empty())
  {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec && !std::filesystem::is_directory(outDir))
    {
      std::fprintf(stderr, "%s: cannot create output path '%s': %s\n", kAppName,
                   outDir.c_str(), ec.message().c_str());
      return 1;
    }
  }
  const std::string entryStem = moduleStem(file);
  const std::string imageOut =
      saveImage ? outputName(outDir, entryStem, ".qlc", rotate) : std::string();
  const std::string listingOut =
      saveListing ? outputName(outDir, entryStem, ".qla", rotate) : std::string();

  // --- Library modules ---
  // Every --import / extra positional argument is resolved into the context's
  // module registry BEFORE the entry is compiled, so `import <name>` finds it
  // there and resolves against its export table (SPEC §bytecodes "import table
  // (lookup)"). A current image is loaded as-is; otherwise the source is
  // compiled and its image saved beside it first (SPEC §qlang-run). These
  // arguments are no longer the only way a library arrives: a name the registry
  // cannot answer is looked for on the search paths while the entry compiles
  // (plan C8a), and the linker below merges whatever the registry holds by then
  // either way.
  ModuleLoader loader;
  loader.searchPaths = ctx.moduleSearchPaths();
  loader.traceLevel = traceLevel;
  loader.asmLevel = asmLevel;
  loader.listing = saveListing;
  for (const std::string &mf : moduleFiles)
  {
    LoadFailure failure;
    if (!ensureModule(ctx, mf, loader, failure))
    {
      std::fprintf(stderr, "%s: %s\n", kAppName, failure.message.c_str());
      return failure.exitCode;
    }
  }

  // --- Load (.qlc / .qpm) or parse + compile (.ql) ---
  // A .qlc file is a compiled program (QLC1 v9, docs/SERIALIZATION.md):
  // read it back and re-bind its native functions by name. A .qpm file is a
  // portable module (QPM1, docs/SERIALIZATION.md §12): its images arrive
  // through the context, which is where the load handshake runs.
  const bool isQlc = source.size() >= 4 && source.compare(0, 4, "QLC1") == 0;
  const bool isQpm = source.size() >= 4 && source.compare(0, 4, "QPM1") == 0;
  CompiledProgram compiled;
  std::map<std::string, int> nativeIds; // name -> negative opcode id
  // Copied out of the compiler (which is scoped to the parse/compile branch)
  // so the pointer stays valid for the disassembly below.
  RegisterReport reportStorage;
  const RegisterReport *report = nullptr;
  // Module provenance for the level-5 module breakdown: `import`-bound names
  // with their native slots (.ql), or the image's native-slot table (.qlc).
  std::vector<ModuleImport> moduleImports;
  size_t stmtCount = 0;
  const auto t0 = Clock::now();
  auto t1 = t0; // parse/compile boundary (== t0 for .qlc/.qpm input)
  if (isQpm)
  {
    // The package is validated and its manifest checked against this binary's
    // registered types before a single image is loaded; what arrives is the
    // entry's module image, so the link step below merges the embedded
    // libraries exactly as it merges the ones --import loaded.
    try
    {
      const std::shared_ptr<LoadedModule> mod =
          ctx.loadPortableModuleFile(file);
      compiled = mod->program;
      for (const auto &[slot, name] : mod->nativeSlots)
      {
        nativeIds[name] = -1 - slot;
        moduleImports.push_back({"?", name, slot, false});
      }
    }
    catch (const QLangError &e)
    {
      std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), e.what());
      return 3;
    }
  }
  else if (isQlc)
  {
    std::istringstream is(source, std::ios::binary);
    std::vector<std::pair<int, std::string>> nativeSlots;
    std::string err;
    if (!readQlc(is, compiled, nativeSlots, err))
    {
      std::fprintf(stderr, "%s: %s: %s\n", kAppName, file.c_str(), err.c_str());
      return 3;
    }
    // The image's native-slot table is all the provenance a .qlc carries:
    // list each slot as an import of unknown module.
    for (const auto &[slot, name] : nativeSlots)
      moduleImports.push_back({"?", name, slot, false});
    // The file stores native function NAMES only; re-resolve them against
    // this context's natives and module functions (a missing name is a
    // load-time error).
    for (const auto &[slot, name] : nativeSlots)
    {
      nativeIds[name] = -1 - slot;
      QLangContext::NativeFunc fn;
      const auto it = ctx.nativeFuncs().find(name);
      if (it != ctx.nativeFuncs().end())
        fn = it->second;
      else
        for (const auto &mod : ctx.modules())
        {
          const auto mit = mod.funcs.find(name);
          if (mit != mod.funcs.end())
          {
            fn = mit->second;
            break;
          }
        }
      if (!fn)
      {
        std::fprintf(stderr, "%s: %s: unknown native function '%s'\n",
                     kAppName, file.c_str(), name.c_str());
        return 3;
      }
      while (compiled.nativeFuncs.size() <= static_cast<size_t>(slot))
        compiled.nativeFuncs.push_back(nullptr);
      compiled.nativeFuncs[static_cast<size_t>(slot)] = fn;
    }
  }
  else
  {
    // --- Parse ---
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
    stmtCount = program.size();
    t1 = Clock::now();

    // --- Compile ---
    QLangCompiler compiler(std::move(program), &ctx);
    // The app never injects host constants (ctx.set(name, value, true)), so
    // the external-const set is empty.
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs())
      nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    // External-type contract from --qlmeta (plan P4): checks only, codegen
    // unchanged. With bindings, ctx's own natives/consts win — addExternal-
    // Types keeps existing native ids and refuses manifest/host enum drift.
    if (!extTypes.empty())
      compiler.addExternalTypes(extTypes);
    // The entry module is named after its source file unless the program
    // declares its own `export const MODULE_NAME` (SPEC §bytecodes "keyword
    // export"). A .qlc input needs no equivalent: its image carries the name.
    compiler.setModuleName(moduleStem(file));

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
    nativeIds.insert(compiler.nativeFuncIds().begin(), compiler.nativeFuncIds().end());
    reportStorage = compiler.registerReport();
    report = &reportStorage;
    for (const auto &imp : compiler.importedModuleFuncs())
    {
      int slot = -1;
      auto nit = compiler.nativeFuncIds().find(imp.func);
      if (nit != compiler.nativeFuncIds().end())
        slot = -1 - nit->second;
      moduleImports.push_back({imp.module, imp.func, slot, imp.fromClass});
    }
  }

  // --- Link ---
  // The compiler lowered every cross-module reference with a handle into the
  // DEFINING module (plan v4 Phase 3b); the linker merges those loaded images
  // with the entry program into one runnable program. A program that imports
  // nothing needs no link, so a single-file run is left exactly as it was.
  // -c writes the entry's own module image, so it is snapshotted here: the link
  // below replaces `compiled` with the merged program, which is runnable but not
  // serializable (.qlc v9 has no section for the init sequence). A program that
  // imports nothing needs no link at all, so for it the snapshot is the program.
  std::optional<CompiledProgram> entryImage;
  std::map<std::string, int> entryNativeIds;
  if (saveImage || !packageFile.empty())
  {
    entryImage = compiled;
    entryNativeIds = nativeIds;
  }

  bool linked = false;
  if (!compiled.imports.empty())
  {
    try
    {
      QLangLinker linker(ctx.loadedModules());
      CompiledProgram merged = linker.link(compiled, nativeIds);
      compiled = std::move(merged);
      linked = true;
    }
    catch (const QLangError &e)
    {
      std::fprintf(stderr, "%s: %s: link: %s\n", kAppName, file.c_str(), e.what());
      return 3;
    }
  }
  const auto t2 = Clock::now();

  // --- Bytecode output (-c) ---
  if (saveImage)
  {
    if (samePath(imageOut, file))
    {
      // The artifact would land on top of the file it was read from: running a
      // .qlc in its own directory with -c. Report it instead of rewriting the
      // image with itself.
      std::fprintf(stderr, "%s: skipping bytecode output: '%s' is the file it was read from\n",
                   kAppName, imageOut.c_str());
    }
    else
    {
      size_t written = 0;
      if (!writeBytecode(*entryImage, entryNativeIds, imageOut, written))
      {
        std::fprintf(stderr, "%s: cannot write bytecode file '%s'\n", kAppName,
                     imageOut.c_str());
        return 4;
      }
      std::fprintf(stderr, "bytecode written: %s (%zu bytes, %zu functions)\n",
                   imageOut.c_str(), written, entryImage->functions.size());
      if (linked)
        std::fprintf(stderr,
                     "%s: note: '%s' is the entry module image; the linked program is not "
                     "serializable (.qlc v9 has no init-sequence section)\n",
                     kAppName, imageOut.c_str());
    }
  }

  // --- Portable module (--package) ---
  // One shipped unit: the entry's own module image, every library image this
  // invocation loaded, and the external-type contract the compile was checked
  // against (--qlmeta; empty manifest when none was given). Module images,
  // never the linked program: `writeQlc` rejects a linked one, and the loader
  // re-links the embedded libraries through the same path as --import.
  if (!packageFile.empty())
  {
    auto slotIds = [](const std::vector<std::pair<int, std::string>> &slots) {
      std::map<std::string, int> ids;
      for (const auto &[slot, name] : slots)
        ids[name] = -1 - slot;
      return ids;
    };
    std::vector<QpmImage> images;
    bool ok = true;
    for (const auto &mod : ctx.loadedModules())
    {
      // The entry is in the registry when the input was a .qpm itself; its
      // pre-link snapshot below is the image that goes into the package.
      if (mod->program.moduleName == entryImage->moduleName)
        continue;
      QpmImage img;
      img.name = mod->program.moduleName;
      ok = qlcBytes(mod->program, slotIds(mod->nativeSlots), img.bytes) && ok;
      images.push_back(std::move(img));
    }
    QpmImage entry;
    entry.name = entryImage->moduleName;
    ok = qlcBytes(*entryImage, entryNativeIds, entry.bytes) && ok;
    images.push_back(std::move(entry));
    std::ofstream out(packageFile, std::ios::binary | std::ios::trunc);
    if (!ok)
    {
      std::fprintf(stderr,
                   "%s: cannot write portable module '%s': an image is not "
                   "serializable (a .qlc image holds durable constants only)\n",
                   kAppName, packageFile.c_str());
      return 4;
    }
    if (!out)
    {
      std::fprintf(stderr, "%s: cannot write portable module '%s'\n", kAppName,
                   packageFile.c_str());
      return 4;
    }
    try
    {
      writeQpm(out, entryImage->moduleName, extTypes, images);
    }
    catch (const QLangError &e)
    {
      std::fprintf(stderr, "%s: %s\n", kAppName, e.message().c_str());
      return 4;
    }
    std::fprintf(stderr,
                 "portable module written: %s (%zu image(s), %zu type(s))\n",
                 packageFile.c_str(), images.size(), extTypes.size());
  }

  // --- Disassembly output (-a) ---
  if (saveListing)
  {
    // .qlc/.qpm input has no source text: level 2 source interleaving is
    // simply skipped, and the register breakdown (level 5) is unavailable.
    std::vector<std::string> srcLines;
    if (!isQlc && !isQpm)
    {
      std::istringstream ls(source);
      std::string line;
      while (std::getline(ls, line))
        srcLines.push_back(line);
    }
    const std::string text =
        disassemble(compiled, srcLines, asmLevel, report, moduleImports,
                    ctx.loadedModules());
    std::string err;
    if (!writeBinaryFile(listingOut, text, err))
    {
      std::fprintf(stderr, "%s: cannot write disassembly file '%s': %s\n", kAppName,
                   listingOut.c_str(), err.c_str());
      return 5;
    }
    std::fprintf(stderr, "disassembly written: %s (%zu functions)\n",
                 listingOut.c_str(), compiled.functions.size());
  }

  // --- Execute ---
  const size_t instrCount = compiled.functions.empty()
                                ? 0
                                : compiled.functions[0].instrOffsets.size();
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
    const std::string text =
        profileText(msBetween(t0, t1), stmtCount, msBetween(t1, t2), instrCount,
                    compiled.constants.size(), compiled.functions.size(),
                    msBetween(t3, t4), msBetween(t0, t4));
    std::fputs(text.c_str(), stderr);
    if (saveListing)
    {
      // The listing was written before execution, so the profile block can
      // only be added now: execute time is not known until vm.run() returns.
      // Only the entry's listing gets it — the images --import generated each
      // keep their own listing beside them.
      std::ofstream aout(listingOut, std::ios::app);
      if (!aout)
      {
        std::fprintf(stderr, "%s: cannot append profile to disassembly file '%s'\n",
                     kAppName, listingOut.c_str());
        return 5;
      }
      aout << "\n" << text;
      aout.flush();
      if (!aout)
      {
        std::fprintf(stderr, "%s: cannot append profile to disassembly file '%s'\n",
                     kAppName, listingOut.c_str());
        return 5;
      }
    }
  }
  return 0;
}
