// The universal output callback in practice (SPEC §api "output callback
// (universal)"; plan docs/plans/output-callback-plan.md).
//
// Every message the engine emits — program output (print/println/printf) and
// verbose trace — flows through ONE QLangContext callback as
// (message, level, file, line), so a host can route it all into one message
// manager (a Qt widget, a log panel) instead of juggling stdout and stderr.
// With no callback assigned the engine writes to the streams exactly as
// always; assigning nullptr back restores that routing.
//
// Build: cmake --build build --target output_callback
// Run:   ./build/output_callback

#include <iostream>
#include <string>
#include <vector>

#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>

using namespace qlang;

namespace {

struct Message {
  std::string text;
  int level;
  std::string file;
  int line;
};

const char *levelName(int level) {
  switch (level) {
    case OUT_TRACE: return "trace";
    case OUT_DEBUG: return "debug";
    case OUT_INFO: return "info";
    case OUT_WARNING: return "warning";
    case OUT_ERROR: return "error";
    case OUT_FATAL: return "fatal";
    default: return "?";
  }
}

// The same program runs under both routings: two print statements on known
// lines, so the attributes in the table are recognisable.
const char *kSource =
    "local name = \"world\"\n"
    "println(\"hello, \" + name)\n"
    "printf(\"2 + 3 = %d\\n\", 5)\n";

} // namespace

int main() {
  try {
    // 1. Default routing (no callback): program output lands on stdout,
    //    the verbose trace on stderr — the streams the engine has always
    //    used.
    {
      QLangContext ctx;
      ctx.setVerboseLevel(TRACE_FRAME);
      ctx.run(kSource);
      std::cout << "[1] default routing above: program output on stdout,"
                << " trace on stderr (levels here: TRACE_FRAME)\n\n";
    }

    // 2. The callback: one sink, exclusive — nothing from the run reached
    //    the streams; everything arrived here with its level and, for
    //    program output, the module name and statement line (verbose trace
    //    carries the ""/0 sentinels).
    {
      QLangContext ctx;
      ctx.setVerboseLevel(TRACE_FRAME);
      std::vector<Message> captured;
      ctx.setOutputCallback(
          [&captured](const std::string &message, int level,
                      const std::string &file, int line) {
            captured.push_back({message, level, file, line});
          });
      ctx.run(kSource);
      std::cout << "[2] the callback captured " << captured.size()
                << " message(s); stdout and stderr got nothing:\n";
      for (const auto &m : captured) {
        std::string one = m.text;
        while (!one.empty() && (one.back() == '\n' || one.back() == '\r'))
          one.pop_back();
        std::cout << "    " << levelName(m.level);
        if (!m.file.empty()) std::cout << "  " << m.file << ":" << m.line;
        std::cout << "  | " << one << "\n";
      }
      std::cout << "\n    print/println/printf arrive as OUT_INFO with "
                << "file/line; every verbose trace line arrives as OUT_TRACE "
                << "with file \"\" / line 0; assigning nullptr to the "
                << "callback restores the default routing; the callback "
                << "survives clear().\n";
    }
    return 0;
  } catch (const QLangError &e) {
    std::cerr << "output_callback: " << e.what() << "\n";
    return 1;
  }
}
