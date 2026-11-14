// Universal output callback (SPEC §api "output callback (universal)"; plan
// docs/plans/output-callback-plan.md P1): every message the engine emits —
// program output (print/println/printf) and verbose trace — flows through
// QLangContext::output(). No callback: the bytes are identical to the old
// direct stream writes (stdout for program output, stderr for trace). With a
// callback: the redirect is EXCLUSIVE (nothing reaches the streams), program
// output arrives as OUT_INFO carrying the module name and the statement's
// line, trace as OUT_TRACE carrying the ""/0 sentinels, verbose gating is
// unchanged, a throwing callback is swallowed, assigning nullptr restores the
// defaults, and the callback survives clear().
#if defined(_WIN32)
#include <fcntl.h> // _O_RDONLY/_O_WRONLY
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

// MSVC has no pipe(); build the fd pair from an anonymous Win32 pipe, and
// alias the POSIX fd functions to their _-prefixed CRT equivalents.
#if defined(_WIN32)
#define dup _dup
#define dup2 _dup2
#define close _close
#define read _read
typedef long long ssize_t; // MSVC has no ssize_t
#endif

namespace {

bool makePipe(int pipefd[2]) {
#if defined(_WIN32)
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0))
    return false;
  pipefd[0] = _open_osfhandle((intptr_t)rd, _O_RDONLY);
  pipefd[1] = _open_osfhandle((intptr_t)wr, _O_WRONLY);
  if (pipefd[0] < 0 || pipefd[1] < 0) {
    if (pipefd[0] >= 0) _close(pipefd[0]);
    if (pipefd[1] >= 0) _close(pipefd[1]);
    return false;
  }
  return true;
#else
  return pipe(pipefd) == 0;
#endif
}

struct Msg {
  std::string text;
  int level;
  std::string file;
  int line;
};

using Sink = std::vector<Msg>;

void collect(Sink &sink, const std::string &message, int level,
             const std::string &file, int line) {
  sink.push_back({message, level, file, line});
}

// Reports are buffered and printed only at the end of main(): the suite
// captures fds 1 and 2, so nothing may write to the streams mid-run.
std::vector<std::string> g_reports;
int g_failures = 0;

void check(bool ok, const std::string &name) {
  g_reports.push_back(std::string(ok ? "ok   " : "FAIL ") + name);
  if (!ok) g_failures++;
}

// Run `fn` with fd (1 = stdout, 2 = stderr) redirected to a pipe; return
// everything that arrived on it. Flushes both streams first so no earlier
// bytes land in this capture.
template <typename Fn>
std::string captureFd(int fd, Fn &&fn) {
  std::cout.flush();
  std::cerr.flush();
  fflush(stdout);
  fflush(stderr);
  int pipefd[2];
  if (!makePipe(pipefd)) return "<pipe-failed>";
  int saved = dup(fd);
  dup2(pipefd[1], fd);
  close(pipefd[1]);
  fn();
  std::cout.flush();
  std::cerr.flush();
  fflush(stdout);
  fflush(stderr);
  dup2(saved, fd);
  close(saved);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof buf)) > 0)
    out.append(buf, static_cast<size_t>(n));
  close(pipefd[0]);
  return out;
}

// The program every case runs: three distinct print sites on three distinct
// lines (the line attributes are asserted below).
const char *kProgram =
    "local a = 1\n"
    "println(\"hi\")\n"
    "printf(\"%d\", 42)\n"
    "print(\"x\")\n";

std::string joinTexts(const Sink &sink) {
  std::string all;
  for (const auto &m : sink) all += m.text;
  return all;
}

bool allLevels(const Sink &sink, int level) {
  for (const auto &m : sink)
    if (m.level != level) return false;
  return true;
}

} // namespace

int main() {
  // 1. Default routing (no callback): the streams still get exactly the
  //    bytes the engine wrote before the sink existed.
  {
    QLangContext ctx;
    std::string out = captureFd(1, [&] { ctx.run(kProgram); });
    check(out == "hi\n42x", "default: print/println/printf bytes on stdout");
    QLangContext tr;
    tr.setVerboseLevel(1);
    std::string err = captureFd(2, [&] { tr.run(kProgram); });
    check(!err.empty(), "default: verbose trace still lands on stderr");
  }

  // 2. Redirect is exclusive: with a callback the streams get nothing and
  //    the messages arrive exactly once, in call order.
  {
    QLangContext ctx;
    Sink sink;
    ctx.setOutputCallback(
        [&](const std::string &m, int l, const std::string &f, int n) {
          collect(sink, m, l, f, n);
        });
    std::string out = captureFd(1, [&] { ctx.run(kProgram); });
    check(out.empty(), "redirect: stdout gets nothing");
    check(sink.size() == 3 && joinTexts(sink) == "hi\n42x",
          "redirect: exactly the three program-output messages");
    check(allLevels(sink, OUT_INFO), "redirect: every message is OUT_INFO");
  }

  // 3. Attributes: file = the module name, line = the statement's line.
  {
    QLangContext ctx;
    Sink sink;
    ctx.setOutputCallback(
        [&](const std::string &m, int l, const std::string &f, int n) {
          collect(sink, m, l, f, n);
        });
    ctx.run(kProgram);
    bool ok = sink.size() == 3 && sink[0].text == "hi\n" &&
              sink[1].text == "42" && sink[2].text == "x";
    for (const auto &m : sink)
      ok = ok && m.file == "main" && m.level == OUT_INFO;
    check(ok && sink.size() == 3 && sink[0].line == 2 && sink[1].line == 3 &&
              sink[2].line == 4,
          "attributes: file \"main\", lines 2/3/4, println text carries \\n");
  }

  // 4. Trace routing: verbose trace reaches the callback at OUT_TRACE with
  //    the ""/0 sentinels, and stderr stays empty.
  {
    QLangContext ctx;
    ctx.setVerboseLevel(TRACE_VM_LOOP);
    Sink sink;
    ctx.setOutputCallback(
        [&](const std::string &m, int l, const std::string &f, int n) {
          collect(sink, m, l, f, n);
        });
    std::string err = captureFd(2, [&] { ctx.run(kProgram); });
    check(err.empty(), "trace routing: stderr gets nothing");
    int traces = 0;
    bool sentinels = true;
    for (const auto &m : sink)
      if (m.level == OUT_TRACE) {
        traces++;
        if (!m.file.empty() || m.line != 0) sentinels = false;
      }
    check(traces > 0, "trace routing: OUT_TRACE messages reach the callback");
    check(sentinels, "trace routing: trace carries file \"\" / line 0");
    check(joinTexts(sink).find("hi") != std::string::npos,
          "trace routing: program output still delivered alongside trace");
  }

  // 5. Gating unchanged: verbose 0 delivers no trace at all.
  {
    QLangContext ctx; // verbose level defaults to off
    Sink sink;
    ctx.setOutputCallback(
        [&](const std::string &m, int l, const std::string &f, int n) {
          collect(sink, m, l, f, n);
        });
    ctx.run(kProgram);
    bool noTrace = true;
    for (const auto &m : sink)
      if (m.level == OUT_TRACE) noTrace = false;
    check(noTrace && sink.size() == 3,
          "gating: verbose 0 delivers program output only");
  }

  // 6. Contract: a throwing callback does not escape (the program keeps
  //    running); assigning nullptr restores the default routing.
  {
    QLangContext ctx;
    int calls = 0;
    ctx.setOutputCallback([&calls](const std::string &, int, const std::string &,
                                   int) {
      calls++;
      throw std::runtime_error("host callback failure");
    });
    bool survived = true;
    try {
      ctx.run(kProgram);
    } catch (...) {
      survived = false;
    }
    check(survived && calls == 3,
          "contract: throwing callback is swallowed, run() completes");
    ctx.setOutputCallback(nullptr);
    std::string out = captureFd(1, [&] { ctx.run("println(\"back\")\n"); });
    check(out == "back\n", "contract: nullptr restores default routing");
  }

  // 7. Lifecycle: the callback is host configuration — clear() keeps it.
  {
    QLangContext ctx;
    Sink sink;
    ctx.setOutputCallback(
        [&](const std::string &m, int l, const std::string &f, int n) {
          collect(sink, m, l, f, n);
        });
    ctx.clear();
    ctx.run("println(\"again\")\n");
    bool fired = sink.size() == 1 && sink[0].text == "again\n" &&
                 sink[0].level == OUT_INFO && sink[0].file == "main" &&
                 sink[0].line == 1;
    check(fired, "lifecycle: the callback still fires after clear()");
  }

  for (const auto &r : g_reports) std::cerr << r << "\n";
  if (g_failures) {
    std::cerr << g_failures << " FAILURES (output_callback)\n";
    return 1;
  }
  std::cerr << "output_callback: all checks passed\n";
  return 0;
}
