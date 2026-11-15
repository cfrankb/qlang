// qla_roundtrip — the .qla v1 assembler round-trip guard (docs/plans/
// encoding-codegen-followups-plan.md Phase 5).
//
// For every tests/scripts/*.ql: `qlcc -c -a` writes the entry image and its
// v1 listing, `qlcc --assemble` rebuilds an image from that listing, and the
// rebuilt image must carry everything the listing claims to preserve
// verbatim — the constant pool (type tag and payload), the function metadata
// (name, params, registers, generator flag, capture registers) and the raw
// instruction streams with their instruction offsets. What v1 does not
// print (the per-instruction loc table, the vars/constVars maps, the
// parameter signature block, the module name, native-slot names) is not
// compared: those are the recorded deviations of the format, not of this
// test. A listing --assemble refuses (an import-table module, deferred with
// the module story) is reported as DEFERRED, never waved through silently.
//
// argv[1]: the built qlcc binary.

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_cli.h"

#include "qlang/QLangBytecodeIO.h"

#ifndef QLANG_TEST_SCRIPTS_DIR
#error "QLANG_TEST_SCRIPTS_DIR must be defined by the build (tests/scripts)"
#endif

namespace {

using qlang::CompiledProgram;
using qlang::QLangValue;

int gFailures = 0;

// Tag-exact fingerprint of a pool value: the .qlc writer serializes the type
// tag plus the payload, so an Int reassembled as a Qword must show up here
// as the byte difference it is in the image.
std::string fingerprint(const QLangValue &v, int depth = 0)
{
  std::ostringstream ss;
  ss << static_cast<int>(v.type()) << ":";
  using T = QLangValue::Type;
  switch (v.type())
  {
  case T::Nil:
  case T::Eof:
    break;
  case T::Bool:
    ss << v.asInt();
    break;
  case T::Char:
    ss << static_cast<int>(v.asChar());
    break;
  case T::Byte:
    ss << static_cast<unsigned>(v.asByte());
    break;
  case T::Word:
    ss << v.asWord();
    break;
  case T::Short:
    ss << v.asShort();
    break;
  case T::Int:
    ss << v.asInt();
    break;
  case T::Unsigned:
    ss << v.asUnsigned();
    break;
  case T::Qword:
    ss << v.asQword();
    break;
  case T::Long:
    ss << v.asLong();
    break;
  case T::Float:
  {
    uint64_t bits = 0;
    const double d = v.asFloat();
    std::memcpy(&bits, &d, sizeof bits);
    ss << std::hex << bits;
    break;
  }
  case T::String:
    ss << '"' << v.asString() << '"';
    break;
  case T::Blob:
    ss << "<" << (v.asBlob() ? v.asBlob()->size() : 0) << " bytes>";
    break;
  case T::Array:
  case T::Set:
  {
    const auto &a = *v.asArray();
    for (size_t i = 0; i < a.size(); i++)
      ss << (i ? "," : "") << fingerprint(a[i], depth + 1);
    break;
  }
  case T::Table:
  {
    for (const auto &[k, val] : *v.asTable())
      ss << k << "=" << fingerprint(val, depth + 1) << ";";
    break;
  }
  case T::Function:
  {
    ss << v.asFunction().funcIndex;
    for (const auto &cap : v.asFunction().captures)
      ss << " r" << cap.reg;
    break;
  }
  default:
    ss << v.typeName();
    break;
  }
  return ss.str();
}

bool loadImage(const std::string &path, CompiledProgram &prog, std::string &err)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    err = "cannot open '" + path + "'";
    return false;
  }
  std::vector<std::pair<int, std::string>> slots;
  return qlang::readQlc(in, prog, slots, err);
}

void check(bool ok, const std::string &what, const std::string &stem)
{
  if (!ok)
  {
    std::printf("FAIL %s: %s\n", stem.c_str(), what.c_str());
    gFailures++;
  }
}

// One script: compile+disassemble, assemble, and compare what v1 promises.
void runScript(const std::string &qlcc, const std::string &dir,
               const std::string &qlPath)
{
  const std::filesystem::path p(qlPath);
  const std::string stem = p.stem().string();
  auto res = testcli::run(qlcc + " -c -a --output-path " + dir + " " + qlPath);
  if (res.code != 0)
  {
    std::printf("FAIL %s: qlcc -c -a exited %d\n%s\n", stem.c_str(), res.code,
                res.out.c_str());
    gFailures++;
    return;
  }
  const std::string image = dir + "/" + stem + ".qlc";
  const std::string listing = dir + "/" + stem + ".qla";
  const std::string rebuilt = dir + "/" + stem + ".asm.qlc";
  res = testcli::run(qlcc + " --assemble " + listing + " -o " + rebuilt);
  if (res.code != 0)
  {
    // Only the documented v1 limit defers; anything else is a failure.
    if (res.out.find("import table") != std::string::npos)
    {
      std::printf("DEFERRED %s: %s", stem.c_str(), res.out.c_str());
      return;
    }
    std::printf("FAIL %s: --assemble exited %d\n%s\n", stem.c_str(), res.code,
                res.out.c_str());
    gFailures++;
    return;
  }

  CompiledProgram want, got;
  std::string err;
  if (!loadImage(image, want, err))
  {
    std::printf("FAIL %s: reading %s: %s\n", stem.c_str(), image.c_str(),
                err.c_str());
    gFailures++;
    return;
  }
  if (!loadImage(rebuilt, got, err))
  {
    std::printf("FAIL %s: the assembled image does not load back: %s\n",
                stem.c_str(), err.c_str());
    gFailures++;
    return;
  }
  if (!want.imports.empty())
  {
    // A module program: -a listed the LINKED program (which -c does not
    // write — it is not serializable) while -c wrote the entry image, so the
    // two artifacts describe different programs. Nothing to compare here;
    // assembling linked images is deferred with the module story.
    std::printf("DEFERRED %s: module program (the -a listing is the linked "
                "program, the -c image the entry module)\n",
                stem.c_str());
    return;
  }

  // The constant pool: index-aligned, tag-exact.
  check(want.constants.size() == got.constants.size(),
        "constant pool size " + std::to_string(want.constants.size()) +
            " != " + std::to_string(got.constants.size()),
        stem);
  const size_t n =
      std::min(want.constants.size(), got.constants.size());
  for (size_t i = 0; i < n; i++)
    if (fingerprint(want.constants[i]) != fingerprint(got.constants[i]))
    {
      check(false, "constant [" + std::to_string(i) + "] '" +
                       fingerprint(want.constants[i]) + "' != '" +
                       fingerprint(got.constants[i]) + "'",
            stem);
      break; // one report per script is enough to localize
    }

  // The functions: metadata plus the raw instruction streams.
  check(want.functions.size() == got.functions.size(),
        "function count " + std::to_string(want.functions.size()) + " != " +
            std::to_string(got.functions.size()),
        stem);
  const size_t nf = std::min(want.functions.size(), got.functions.size());
  for (size_t f = 0; f < nf; f++)
  {
    const auto &a = want.functions[f];
    const auto &b = got.functions[f];
    const std::string tag = "function #" + std::to_string(f);
    check(a.name == b.name, tag + " name '" + a.name + "' != '" + b.name + "'",
          stem);
    check(a.paramCount == b.paramCount && a.registerCount == b.registerCount,
          tag + " params/registers mismatch", stem);
    check(a.isGenerator == b.isGenerator, tag + " generator flag mismatch",
          stem);
    check(a.captures.size() == b.captures.size(), tag + " capture count", stem);
    for (size_t c = 0;
         c < std::min(a.captures.size(), b.captures.size()); c++)
      check(a.captures[c].reg == b.captures[c].reg,
            tag + " capture #" + std::to_string(c) + " register", stem);
    check(a.code == b.code,
          tag + " instruction stream " + std::to_string(a.code.size()) +
              " vs " + std::to_string(b.code.size()) + " bytes",
          stem);
    check(a.instrOffsets == b.instrOffsets, tag + " instruction offsets", stem);
  }
  std::printf("ok %s (%zu functions, %zu constants)\n", stem.c_str(),
              want.functions.size(), want.constants.size());
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    std::printf("usage: qla_roundtrip <path-to-qlcc>\n");
    return 1;
  }
  const std::string scratch = testcli::scratch("qla_roundtrip");
  if (scratch.empty())
  {
    std::printf("cannot create the scratch directory\n");
    return 1;
  }
  std::vector<std::string> scripts;
  for (const auto &e : std::filesystem::directory_iterator(
           QLANG_TEST_SCRIPTS_DIR))
    if (e.is_regular_file() && e.path().extension() == ".ql")
      scripts.push_back(e.path().string());
  std::sort(scripts.begin(), scripts.end());
  if (scripts.empty())
  {
    std::printf("no scripts found in %s\n", QLANG_TEST_SCRIPTS_DIR);
    return 1;
  }
  for (const auto &s : scripts)
    runScript(argv[1], scratch, s);
  if (gFailures != 0)
  {
    std::printf("%d round-trip failure(s)\n", gFailures);
    return 1;
  }
  std::printf("all .qla listings round-trip (%zu scripts)\n", scripts.size());
  return 0;
}
