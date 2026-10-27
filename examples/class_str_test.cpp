// `__str__` dispatch in all display paths: str/sprintf/printf/f-string/join
// must call `__str__` (and fall back to the class name), not "<table>".
// Value-level assertions avoid stdout scraping; one capture case each for
// print/println themselves.
#include <unistd.h>
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectStr(const std::string &name, const std::string &src,
                      const std::string &var, const std::string &expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::string got = ctx.get(var).asString();
    if (got == expect) { std::cout << "[PASS] str:  " << name << " (" << var << "=\"" << got << "\")\n"; passes++; }
    else { std::cout << "[FAIL] str:  " << name << " (" << var << "=\"" << got << "\" exp \"" << expect << "\")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] str:  " << name << " -> " << e.what() << "\n"; fails++; }
}

// Run src with stdout redirected to a pipe; return what it printed.
static std::string captureStdout(const std::string &src) {
  std::cout.flush(); // drain pending test reports into the real stdout first
  int pipefd[2];
  if (pipe(pipefd) != 0) return "<pipe-failed>";
  int saved = dup(1);
  dup2(pipefd[1], 1);
  close(pipefd[1]);
  QLangContext ctx;
  try { ctx.run(src); } catch (const QLangError &) {}
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof buf)) > 0)
    out.append(buf, static_cast<size_t>(n));
  close(pipefd[0]);
  return out;
}

static void expectPrint(const std::string &name, const std::string &src,
                        const std::string &expect) {
  std::string got = captureStdout(src);
  // Reports go to stderr: stdout is the unit under test.
  if (got == expect) { std::cerr << "[PASS] out:  " << name << " (\"" << got << "\")\n"; passes++; }
  else { std::cerr << "[FAIL] out:  " << name << " (\"" << got << "\" exp \"" << expect << "\")\n"; fails++; }
}

static const std::string MONEY =
    "class Money {\n"
    "total = 0\n"
    "__init__ (self, v) [ ] { self.total = v }\n"
    "__str__ (self) [ ] { return f\"balance: {self.total}\" }\n"
    "}\n"
    "local m = Money(125)\n";

int main() {
  // 1. str() regression guard: __str__ already worked here.
  expectStr("str: __str__", MONEY + "local s = str(m);", "s", "balance: 125");

  // 2. printf path: sprintf("%s", m).
  expectStr("sprintf %s: __str__", MONEY + "local s = sprintf(\"%s\", m);", "s", "balance: 125");

  // 3. f-string path.
  expectStr("f-string: __str__", MONEY + "local s = f\"obj={m}\";", "s", "obj=balance: 125");

  // 4. join path: both elements stringify via __str__.
  expectStr("join: __str__",
            MONEY + "local m2 = Money(300)\nlocal a = []\na[] = m\na[] = m2\nlocal s = join(\", \", a);",
            "s", "balance: 125, balance: 300");

  // 5/6. println/print themselves (stdout capture).
  expectPrint("println: __str__", MONEY + "println(m)", "balance: 125\n");
  expectPrint("print: no trailing newline", MONEY + "print(m)", "balance: 125");

  // 7. Class without __str__: class name via str, println, f-string.
  expectStr("str: no __str__ -> class name",
            "class Plain { x = 1 }\nlocal p = Plain()\nlocal s = str(p);", "s", "Plain");
  expectPrint("println: no __str__ -> class name",
              "class Plain { x = 1 }\nlocal p = Plain()\nprintln(p)", "Plain\n");
  expectStr("f-string: no __str__ -> class name",
            "class Plain { x = 1 }\nlocal p = Plain()\nlocal s = f\"t={p}\";", "s", "t=Plain");

  // 8. Plain tables render their entries (SPEC # implicit __str__ rules).
  expectStr("str: plain table", "local t = {a=1}\nlocal s = str(t);", "s", "{a=1}");
  expectStr("sprintf %s: plain table", "local t = {a=1}\nlocal s = sprintf(\"%s\", t);", "s", "{a=1}");

  // 9. __str__ returning a non-string renders via the generic form.
  expectStr("str: __str__ returns number",
            "class N { __str__ (self) [ ] { return 42 } }\n"
            "local n = N()\nlocal s = str(n);", "s", "42");

  // 10. Recursive __str__ terminates: the nested display skips __str__ and
  // falls back to the class name.
  expectStr("str: recursive __str__ terminates",
            "class R { __str__ (self) [ ] { return str(self) } }\n"
            "local r = R()\nlocal s = str(r);", "s", "R");

  std::cout << "\n" << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
