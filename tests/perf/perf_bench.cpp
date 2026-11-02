// perf_bench — QLang performance baseline harness (SPEC # performance tests).
//
// Benchmarks the parser, compiler and execution stages over a fixed corpus
// of .ql scripts, takes the median of N=5 runs per stage (after one
// unmeasured warm-up pass per script), writes the results to a JSON file
// and compares them against the committed baseline. A stage whose median
// deviates from the baseline by more than the tolerance (default ±20%,
// override with the QL_PERF_TOLERANCE env var, percentage) AND by more than
// the absolute epsilon (default 0.5 ms, override with QL_PERF_EPS_MS) fails
// the gate. The epsilon floor keeps sub-millisecond stages from flapping on
// timer/system jitter, where a pure relative check is meaningless.
//
//   perf_bench [options]
//
// options:
//   --update-baseline   write the measured medians to the baseline file
//                       (deliberate act — always a separate commit)
//   --baseline <file>   baseline path (default: tests/perf/baseline.json in
//                       the source tree, fixed at CMake configure time)
//   --result <file>     result JSON path (default: ./perf_result.json)
//   --no-compare        measure + write the result only, skip the gate
//   -h, --help          show this help and exit
//
// JSON schema (result and baseline, identical):
//   [
//     { "script": "hello.ql", "stage": "parse",
//       "median_ms": 0.031, "runs_ms": [0.03, 0.031, 0.032, 0.029, 0.033] },
//     ...
//   ]
//
// exit codes:
//   0  all stages within tolerance (or --no-compare / --update-baseline)
//   1  usage error
//   2  corpus script file could not be read
//   3  QLang error (parse/compile/execute)
//   4  baseline/result file I/O error (incl. missing baseline entry)
//   5  perf gate failed: a stage exceeded the tolerance
//   77 not run (build type other than Release — CTest "not run" convention)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define PERF_DUP _dup
#define PERF_DUP2 _dup2
#define PERF_CLOSE _close
#define PERF_OPEN _open
#define PERF_O_WRONLY _O_WRONLY
#define PERF_NULL_PATH "NUL"
#else
#include <fcntl.h>
#include <unistd.h>
#define PERF_DUP dup
#define PERF_DUP2 dup2
#define PERF_CLOSE close
#define PERF_OPEN open
#define PERF_O_WRONLY O_WRONLY
#define PERF_NULL_PATH "/dev/null"
#endif

#include <qlang/QLangCompiler.h>
#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>
#include <qlang/QLangVM.h>

using namespace qlang;

namespace
{

  const char *kAppName = "perf_bench";

  // The committed baseline is calibrated for Release builds (CI gxx job).
  // In any other configuration the ctest entry reports "not run" (CTest
  // exit-code convention 77) instead of failing on build-type noise.
  const int kExitNotRun = 77;

  // Fixed corpus (SPEC # performance tests). Paths are relative to the
  // tests/scripts/ directory baked in at CMake configure time.
  const char *kCorpus[] = {"hello.ql", "money.ql", "cast.ql", "perf_stress.ql"};
  const int kRuns = 5;
  const double kDefaultTolerancePct = 20.0;
  // Absolute deviation floor (ms): a stage only fails when it exceeds BOTH
  // the relative tolerance and this absolute deviation.
  const double kDefaultEpsilonMs = 0.5;
  // The corpus is fixed and known to terminate; raise the live VM step guard
  // (default 100000) so heavy deterministic loops are not misreported as
  // infinite loops. perf_stress.ql raises it from QLang as well.
  const std::size_t kBenchMaxSteps = 10000000;

  using Clock = std::chrono::steady_clock;

  double msBetween(Clock::time_point a, Clock::time_point b)
  {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

  double median(std::vector<double> v)
  {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  }

  void printHelp(const char *prog)
  {
    std::cout
        << "QLang performance baseline harness.\n"
        << "\n"
        << "usage: " << prog << " [options]\n"
        << "\n"
        << "options:\n"
        << "  --update-baseline   write the measured medians to the baseline file\n"
        << "  --baseline <file>   baseline path (default: "
        << QLANG_PERF_BASELINE << ")\n"
        << "  --result <file>     result JSON path (default: ./perf_result.json)\n"
        << "  --no-compare        measure + write the result only, skip the gate\n"
        << "  -h, --help          show this help and exit\n"
        << "\n"
        << "env:\n"
        << "  QL_PERF_TOLERANCE   relative tolerance as a percentage (default 20, i.e. ±20%)\n"
        << "  QL_PERF_EPS_MS      absolute deviation floor in ms (default 0.5); a stage only\n"
        << "                      fails when it exceeds BOTH the tolerance and this floor\n"
        << "\n"
        << "exit codes:\n"
        << "  0  all stages within tolerance (or --no-compare / --update-baseline)\n"
        << "  1  usage error\n"
        << "  2  corpus script file could not be read\n"
        << "  3  QLang error (parse/compile/execute)\n"
        << "  4  baseline/result file I/O error (incl. missing baseline entry)\n"
        << "  5  perf gate failed: a stage exceeded the tolerance\n"
        << "  77 not run (build type other than Release — CTest \"not run\" convention)\n";
  }

  [[noreturn]] void die(int code, const std::string &msg)
  {
    std::fprintf(stderr, "%s: %s\n", kAppName, msg.c_str());
    std::exit(code);
  }

  std::string readFile(const std::string &path)
  {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      die(2, "cannot open file '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  bool writeFile(const std::string &path, const std::string &content)
  {
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out)
      return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    return static_cast<bool>(out);
  }

  // RAII guard: point stdout at the OS null device for the guarded scope so
  // the benchmarked script's program output stays out of the report table.
  class StdoutGuard
  {
  public:
    StdoutGuard()
    {
      m_savedFd = PERF_DUP(1);
      if (m_savedFd < 0)
        return;
      int nullFd = PERF_OPEN(PERF_NULL_PATH, PERF_O_WRONLY);
      if (nullFd < 0)
        return;
      std::fflush(stdout);
      PERF_DUP2(nullFd, 1);
      PERF_CLOSE(nullFd);
    }

    ~StdoutGuard()
    {
      if (m_savedFd < 0)
        return;
      std::fflush(stdout);
      PERF_DUP2(m_savedFd, 1);
      PERF_CLOSE(m_savedFd);
    }

    StdoutGuard(const StdoutGuard &) = delete;
    StdoutGuard &operator=(const StdoutGuard &) = delete;

  private:
    int m_savedFd = -1;
  };

  // One pipeline pass of a script: parse -> compile -> execute, each stage
  // timed. A fresh context per pass keeps the runs independent.
  struct StageTimes
  {
    double parse = 0.0;
    double compile = 0.0;
    double execute = 0.0;
  };

  StageTimes runOnce(const std::string &script, const std::string &source)
  {
    {
      QLangContext ctx;
      ctx.setMaxSteps(kBenchMaxSteps);
      StageTimes t;

      // --- Parse ---
      const auto t0 = Clock::now();
      std::vector<StmtPtr> program;
      try
      {
        program = ctx.parse(source);
      }
      catch (const QLangError &e)
      {
        die(3, script + ": parse: " + std::string(e.what()));
      }
      t.parse = msBetween(t0, Clock::now());

      // --- Compile (wired exactly as qlang_run does) ---
      QLangCompiler compiler(std::move(program), &ctx);
      compiler.addExternalConsts({});
      std::unordered_set<std::string> nativeNames;
      for (const auto &[name, _] : ctx.nativeFuncs())
        nativeNames.insert(name);
      compiler.addExternalFuncs(nativeNames);
      compiler.addModules(ctx.modules());

      CompiledProgram compiled;
      const auto t1 = Clock::now();
      try
      {
        compiled = compiler.compile();
      }
      catch (const QLangError &e)
      {
        die(3, script + ": compile: " + std::string(e.what()));
      }
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
      for (size_t j = 0; j < resolved.size(); j++)
      {
        while (compiled.nativeFuncs.size() <= j)
          compiled.nativeFuncs.push_back(nullptr);
        if (resolved[j])
          compiled.nativeFuncs[j] = resolved[j];
      }
      t.compile = msBetween(t1, Clock::now());

      // --- Execute (script output suppressed) ---
      const auto t2 = Clock::now();
      {
        StdoutGuard guard;
        QLangVM vm(compiled, ctx);
        try
        {
          vm.run();
        }
        catch (const QLangError &e)
        {
          die(3, script + ": execute: " + std::string(e.what()));
        }
        catch (const std::exception &e)
        {
          die(3, script + ": execute: " + std::string(e.what()));
        }
      }
      t.execute = msBetween(t2, Clock::now());
      return t;
    }
  }

  std::vector<StageTimes> benchScript(const std::string &script, const std::string &source)
  {
    // Unmeasured warm-up pass: stabilizes allocator/caches/code paths before
    // the measured runs.
    runOnce(script, source);
    std::vector<StageTimes> times;
    for (int i = 0; i < kRuns; i++)
      times.push_back(runOnce(script, source));
    return times;
  }

  // --- Result model + JSON ---

  struct Entry
  {
    std::string script;
    std::string stage;
    double medianMs = 0.0;
    std::vector<double> runsMs;
  };

  std::vector<Entry> collectEntries(const std::string &scriptsDir)
  {
    std::vector<Entry> entries;
    for (const char *script : kCorpus)
    {
      const std::string path = scriptsDir + "/" + script;
      const std::string source = readFile(path);
      std::vector<StageTimes> times = benchScript(script, source);

      double parseRuns[kRuns], compileRuns[kRuns], executeRuns[kRuns];
      for (int i = 0; i < kRuns; i++)
      {
        parseRuns[i] = times[i].parse;
        compileRuns[i] = times[i].compile;
        executeRuns[i] = times[i].execute;
      }
      entries.push_back({script, "parse", median({parseRuns, parseRuns + kRuns}),
                         {parseRuns, parseRuns + kRuns}});
      entries.push_back({script, "compile", median({compileRuns, compileRuns + kRuns}),
                         {compileRuns, compileRuns + kRuns}});
      entries.push_back({script, "execute", median({executeRuns, executeRuns + kRuns}),
                         {executeRuns, executeRuns + kRuns}});
    }
    return entries;
  }

  std::string jsonEscape(const std::string &s)
  {
    std::string out = "\"";
    for (char c : s)
    {
      switch (c)
      {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20)
          {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          }
          else
            out += c;
      }
    }
    out += "\"";
    return out;
  }

  std::string formatMs(double v)
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
  }

  // Serialize as the shared result/baseline schema: an array of
  // {script, stage, median_ms, runs_ms[]} objects.
  std::string toJson(const std::vector<Entry> &entries)
  {
    std::string out = "[\n";
    for (size_t i = 0; i < entries.size(); i++)
    {
      const Entry &e = entries[i];
      out += "  {\n";
      out += "    \"script\": " + jsonEscape(e.script) + ",\n";
      out += "    \"stage\": " + jsonEscape(e.stage) + ",\n";
      out += "    \"median_ms\": " + formatMs(e.medianMs) + ",\n";
      out += "    \"runs_ms\": [";
      for (size_t j = 0; j < e.runsMs.size(); j++)
        out += (j ? ", " : "") + formatMs(e.runsMs[j]);
      out += "]\n";
      out += (i + 1 < entries.size()) ? "  },\n" : "  }\n";
    }
    out += "]\n";
    return out;
  }

  // Minimal reader for the baseline schema above (array of flat objects).
  // Tolerates extra/unknown keys so the baseline format can grow.
  class JsonReader
  {
  public:
    explicit JsonReader(const std::string &text) : m_text(text) {}

  private:
    const std::string m_text;
    size_t m_pos = 0;

  public:

    bool parse(std::vector<Entry> *out, std::string *err)
    {
      out->clear();
      skipWs();
      if (!consume('['))
        return fail(err, "expected '[' at document start");
      skipWs();
      if (consume(']'))
        return true; // empty array is valid syntax, caller checks completeness
      while (true)
      {
        Entry e;
        if (!parseObject(&e, err))
          return false;
        out->push_back(std::move(e));
        skipWs();
        if (consume(','))
        {
          skipWs();
          continue;
        }
        if (consume(']'))
          return true;
        return fail(err, "expected ',' or ']' in array");
      }
    }

  private:
    bool fail(std::string *err, const std::string &msg)
    {
      *err = msg + " (offset " + std::to_string(m_pos) + ")";
      return false;
    }

    void skipWs()
    {
      while (m_pos < m_text.size() &&
             (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' ||
              m_text[m_pos] == '\r'))
        m_pos++;
    }

    bool consume(char c)
    {
      if (m_pos < m_text.size() && m_text[m_pos] == c)
      {
        m_pos++;
        return true;
      }
      return false;
    }

    bool parseString(std::string *out)
    {
      if (!consume('"'))
        return false;
      std::string s;
      while (m_pos < m_text.size() && m_text[m_pos] != '"')
      {
        char c = m_text[m_pos++];
        if (c == '\\' && m_pos < m_text.size())
        {
          char e = m_text[m_pos++];
          switch (e)
          {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            default: s += e; break;
          }
        }
        else
          s += c;
      }
      if (!consume('"'))
        return false;
      if (out)
        *out = s;
      return true;
    }

    bool parseNumber(double *out)
    {
      size_t start = m_pos;
      while (m_pos < m_text.size() &&
             (std::isdigit(static_cast<unsigned char>(m_text[m_pos])) ||
              m_text[m_pos] == '-' || m_text[m_pos] == '+' || m_text[m_pos] == '.' ||
              m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        m_pos++;
      if (m_pos == start)
        return false;
      *out = std::stod(m_text.substr(start, m_pos - start));
      return true;
    }

    // Skip any value (used for unknown keys).
    bool skipValue()
    {
      skipWs();
      if (m_pos >= m_text.size())
        return false;
      char c = m_text[m_pos];
      if (c == '"')
        return parseString(nullptr);
      if (c == '[')
      {
        if (!consume('['))
          return false;
        skipWs();
        if (consume(']'))
          return true;
        while (true)
        {
          if (!skipValue())
            return false;
          skipWs();
          if (consume(','))
          {
            skipWs();
            continue;
          }
          return consume(']');
        }
      }
      if (c == '{')
      {
        if (!consume('{'))
          return false;
        skipWs();
        if (consume('}'))
          return true;
        while (true)
        {
          std::string key;
          if (!parseString(&key))
            return false;
          skipWs();
          if (!consume(':'))
            return false;
          if (!skipValue())
            return false;
          skipWs();
          if (consume(','))
          {
            skipWs();
            continue;
          }
          return consume('}');
        }
      }
      // number / true / false / null
      while (m_pos < m_text.size() && m_text[m_pos] != ',' && m_text[m_pos] != ']' &&
             m_text[m_pos] != '}' && m_text[m_pos] != ' ' && m_text[m_pos] != '\n' &&
             m_text[m_pos] != '\t' && m_text[m_pos] != '\r')
        m_pos++;
      return true;
    }

    bool parseObject(Entry *e, std::string *err)
    {
      if (!consume('{'))
        return fail(err, "expected '{'");
      skipWs();
      if (consume('}'))
        return true;
      while (true)
      {
        std::string key;
        if (!parseString(&key))
          return fail(err, "expected object key string");
        skipWs();
        if (!consume(':'))
          return fail(err, "expected ':' after key");
        skipWs();
        if (key == "script")
        {
          if (!parseString(&e->script))
            return fail(err, "script: expected string");
        }
        else if (key == "stage")
        {
          if (!parseString(&e->stage))
            return fail(err, "stage: expected string");
        }
        else if (key == "median_ms")
        {
          if (!parseNumber(&e->medianMs))
            return fail(err, "median_ms: expected number");
        }
        else if (key == "runs_ms")
        {
          if (!consume('['))
            return fail(err, "runs_ms: expected '['");
          skipWs();
          if (!consume(']'))
          {
            while (true)
            {
              double v = 0.0;
              if (!parseNumber(&v))
                return fail(err, "runs_ms: expected number");
              e->runsMs.push_back(v);
              skipWs();
              if (consume(','))
              {
                skipWs();
                continue;
              }
              break;
            }
            if (!consume(']'))
              return fail(err, "runs_ms: expected ']'");
          }
        }
        else
        {
          if (!skipValue())
            return fail(err, "unknown key '" + key + "': bad value");
        }
        skipWs();
        if (consume(','))
        {
          skipWs();
          continue;
        }
        return consume('}');
      }
    }
  };

  struct CompareRow
  {
    const Entry *entry;
    double baselineMs;
    double deltaPct; // signed, e.g. +12.3 means 12.3% slower
    bool outside; // exceeds BOTH the relative tolerance and the epsilon
  };

  // Look up each result entry in the baseline and compute the signed delta.
  // A row is "outside" when its deviation exceeds BOTH the relative
  // tolerance (percent) and the absolute epsilon (ms). Returns false (with a
  // message) when a (script, stage) pair is missing.
  bool compare(const std::vector<Entry> &result, const std::vector<Entry> &baseline,
               double tolerancePct, double epsilonMs, std::vector<CompareRow> *rows,
               std::string *err)
  {
    std::unordered_map<std::string, const Entry *> byKey;
    for (const auto &e : baseline)
      byKey[e.script + "/" + e.stage] = &e;

    rows->clear();
    for (const auto &e : result)
    {
      auto it = byKey.find(e.script + "/" + e.stage);
      if (it == byKey.end())
      {
        *err = "baseline has no entry for " + e.script + "/" + e.stage +
               " (run with --update-baseline after changing the corpus)";
        return false;
      }
      const Entry *b = it->second;
      if (b->medianMs <= 0.0)
      {
        *err = "baseline median for " + b->script + "/" + b->stage +
               " is not positive";
        return false;
      }
      CompareRow row;
      row.entry = &e;
      row.baselineMs = b->medianMs;
      row.deltaPct = (e.medianMs - b->medianMs) / b->medianMs * 100.0;
      const double absDevMs = std::fabs(e.medianMs - b->medianMs);
      row.outside = std::fabs(row.deltaPct) > tolerancePct && absDevMs > epsilonMs;
      rows->push_back(row);
    }
    return true;
  }

} // namespace

int main(int argc, char **argv)
{
#if defined(QLANG_PERF_BUILD_TYPE)
  if (std::string(QLANG_PERF_BUILD_TYPE) != "Release")
  {
    std::printf("%s: build type '%s' — perf gate is calibrated for Release, "
                "skipping (CTest not-run)\n",
                kAppName, QLANG_PERF_BUILD_TYPE);
    return kExitNotRun;
  }
#endif

  std::string baselinePath = QLANG_PERF_BASELINE;
  std::string resultPath = "perf_result.json";
  bool updateBaseline = false;
  bool noCompare = false;

  for (int i = 1; i < argc; i++)
  {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help")
    {
      printHelp(argv[0]);
      return 0;
    }
    if (arg == "--update-baseline")
    {
      updateBaseline = true;
      continue;
    }
    if (arg == "--no-compare")
    {
      noCompare = true;
      continue;
    }
    if (arg == "--baseline")
    {
      if (i + 1 >= argc)
        die(1, "--baseline requires a file name");
      baselinePath = argv[++i];
      continue;
    }
    if (arg.rfind("--baseline=", 0) == 0)
    {
      baselinePath = arg.substr(11);
      continue;
    }
    if (arg == "--result")
    {
      if (i + 1 >= argc)
        die(1, "--result requires a file name");
      resultPath = argv[++i];
      continue;
    }
    if (arg.rfind("--result=", 0) == 0)
    {
      resultPath = arg.substr(9);
      continue;
    }
    die(1, "unknown option '" + arg + "' (see --help)");
  }

  double tolerancePct = kDefaultTolerancePct;
  if (const char *env = std::getenv("QL_PERF_TOLERANCE"))
  {
    char *end = nullptr;
    tolerancePct = std::strtod(env, &end);
    if (end == env || *end != '\0' || tolerancePct < 0.0)
      die(1, std::string("QL_PERF_TOLERANCE must be a non-negative number of percent, got '") +
                 env + "'");
  }

  double epsilonMs = kDefaultEpsilonMs;
  if (const char *env = std::getenv("QL_PERF_EPS_MS"))
  {
    char *end = nullptr;
    epsilonMs = std::strtod(env, &end);
    if (end == env || *end != '\0' || epsilonMs < 0.0)
      die(1, std::string("QL_PERF_EPS_MS must be a non-negative number of milliseconds, got '") +
                 env + "'");
  }

  // Script directory and baseline path are fixed at CMake configure time.
  const char *scriptsDir = QLANG_TEST_SCRIPTS_DIR;

  std::vector<Entry> result = collectEntries(scriptsDir);
  const std::string resultJson = toJson(result);
  if (!writeFile(resultPath, resultJson))
    die(4, "cannot write result file '" + resultPath + "'");
  std::printf("result written: %s (%zu entries)\n", resultPath.c_str(), result.size());

  if (updateBaseline)
  {
    if (!writeFile(baselinePath, resultJson))
      die(4, "cannot write baseline file '" + baselinePath + "'");
    std::printf("baseline updated: %s (deliberate act — commit it separately)\n",
                baselinePath.c_str());
    return 0;
  }

  if (noCompare)
  {
    for (const auto &e : result)
      std::printf("%-16s %-8s median %10.3f ms\n", e.script.c_str(), e.stage.c_str(),
                  e.medianMs);
    std::printf("result: OK (no comparison)\n");
    return 0;
  }

  std::string baselineText;
  {
    std::ifstream in(baselinePath, std::ios::binary);
    if (!in)
      die(4, "cannot open baseline file '" + baselinePath +
                 "' (generate it with: " + std::string(kAppName) + " --update-baseline)");
    std::ostringstream ss;
    ss << in.rdbuf();
    baselineText = ss.str();
  }

  std::vector<Entry> baseline;
  std::string jsonErr;
  JsonReader reader(baselineText);
  if (!reader.parse(&baseline, &jsonErr))
    die(4, "baseline file '" + baselinePath + "' is not valid: " + jsonErr);

  std::vector<CompareRow> rows;
  std::string cmpErr;
  if (!compare(result, baseline, tolerancePct, epsilonMs, &rows, &cmpErr))
    die(4, cmpErr);

  std::printf("\ntolerance ±%.1f%% and > %.2f ms absolute (per stage)\n\n", tolerancePct,
              epsilonMs);
  std::printf("%-16s %-8s %12s %12s %10s\n", "script", "stage", "median(ms)",
              "baseline(ms)", "delta(%)");
  int outside = 0;
  for (const auto &row : rows)
  {
    const Entry *e = row.entry;
    if (row.outside)
      outside++;
    std::printf("%-16s %-8s %12.3f %12.3f %+10.1f%s\n", e->script.c_str(),
                e->stage.c_str(), e->medianMs, row.baselineMs, row.deltaPct,
                row.outside ? "  <-- OUT" : "");
  }

  if (outside > 0)
  {
    std::printf("\nresult: FAIL — %d/%zu stages outside ±%.1f%% and > %.2f ms of baseline\n",
                outside, rows.size(), tolerancePct, epsilonMs);
    return 5;
  }
  std::printf("\nresult: PASS — %zu/%zu stages within ±%.1f%% and %.2f ms of baseline\n",
              rows.size(), rows.size(), tolerancePct, epsilonMs);
  return 0;
}
