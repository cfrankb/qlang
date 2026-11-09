// phase0_api.cpp — Phase 0 red tests: host API lifetime / durability defects.
//
//   P1  A QLangError thrown mid-run aborts the VM before the main-frame
//       sync-out, so the context's variable map keeps the PRE-run value.
//       set(v=1); run("v = 99; assert(false)") → get(v) must be 99.
//       Today: get(v) == 1 (stale).
//
//   P3  call() enters the VM with NO caller frame, so `global` captures in
//       the called function are bound to frame -1 (out-of-bounds pointer);
//       even when that does not crash, the value read is a call ARGUMENT
//       slot, not the host variable. The host variable set via set() after
//       run() must be what the function sees.
//       set(x=7); run("x = 0; def f() { global x; return x }");
//       set(x=5); call("f") must return 5.
//       Today: segfault (capture bound to m_frames[-1]).
//
//   F1  writeQlc silently remaps non-durable constant types (Generator,
//       Object, External, BoundMethod, TypeRef) to Nil, so a program that
//       depends on them is corrupted without any signal. The writer must
//       reject such programs (return false) instead of silently dropping
//       the values.  Today: writeQlc returns true and the constants come
//       back as Nil.
//
// P3 runs in a child process (test_child.h) so the current segfault cannot
// kill the other checks.

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "test_child.h"

#include "qlang/QLangBytecode.h"
#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

using namespace qlang;

// P3 check body. Runs in the child process (see test_child.h); returns 42
// on success, 1 on failure.
int p3Check() {
  try {
    QLangContext ctx;
    ctx.set("x", QLangValue::Int(7));
    ctx.run("x = 0\ndef f() { global x; return x }\n");
    ctx.set("x", QLangValue::Int(5));
    QLangValue r = ctx.call("f");
    if (r.type() == QLangValue::Type::Int && toInt64(r) == 5)
      return 42;
  } catch (...) {
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc > 2 && std::strcmp(argv[1], "--child") == 0 &&
      std::strcmp(argv[2], "p3") == 0)
    return p3Check();

  int failures = 0;

  // ---------------------------------------------------------------- P1
  {
    QLangContext ctx;
    ctx.set("v", QLangValue::Int(1));
    bool threw = false;
    try {
      ctx.run("v = 99\nassert(false, \"boom\")\n");
    } catch (const QLangError &) {
      threw = true;
    }
    if (!threw) {
      fprintf(stderr, "FAIL P1: assert(false) did not throw\n");
      failures++;
    } else {
      QLangValue v = ctx.get("v");
      if (v.type() == QLangValue::Type::Int && toInt64(v) == 99) {
        fprintf(stderr, "ok   P1: scope synced before throw (v = 99)\n");
      } else {
        fprintf(stderr, "FAIL P1: v = type %d value %lld after throw, expected 99 (stale scope)\n",
                (int)v.type(),
                (long long)(v.type() == QLangValue::Type::Int ? toInt64(v) : -1));
        failures++;
      }
    }
  }

  // ---------------------------------------------------------------- P3
  {
    int rc = testchild::runSelfChild(argv[0], "p3");
    if (rc == 42) {
      fprintf(stderr, "ok   P3: set() after run() visible to call() (x = 5)\n");
    } else {
      const char *why = rc < 0
                            ? "crashed (capture bound to frame -1 / stale snapshot)"
                            : "wrong value (stale snapshot)";
      fprintf(stderr, "FAIL P3: set() after run() visible to call() — %s\n", why);
      failures++;
    }
  }

  // ---------------------------------------------------------------- F1
  {
    // A program whose constant pool holds non-durable values.
    CompiledProgram prog;
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = 1;
    prog.constants.push_back(
        QLangValue::Generator(std::make_shared<GeneratorState>()));
    prog.constants.push_back(
        QLangValue::External(nullptr, std::shared_ptr<void>()));
    prog.constants.push_back(QLangValue::TypeRef(nullptr));
    prog.constants.push_back(
        QLangValue::Object(std::make_shared<ObjectDescriptor>()));
    prog.functions.push_back(std::move(fn));
    prog.registerCount = 1;

    std::ostringstream os(std::ios::binary);
    bool ok = writeQlc(os, prog, {});
    if (!ok) {
      fprintf(stderr, "ok   F1: writeQlc rejected non-durable constants\n");
    } else {
      // Silent success: verify the corruption (constants dropped to Nil).
      CompiledProgram back;
      std::vector<std::pair<int, std::string>> slots;
      std::string err;
      std::istringstream is(os.str(), std::ios::binary);
      bool rd = readQlc(is, back, slots, err);
      bool allNil =
          rd && back.functions.size() == 1 && back.constants.size() == 4;
      if (allNil)
        for (const auto &c : back.constants)
          if (c.type() != QLangValue::Type::Nil)
            allNil = false;
      fprintf(stderr,
              "FAIL F1: writeQlc silently dropped non-durable constants "
              "(round-trip all Nil: %s); expected rejection\n",
              allNil ? "yes" : "no");
      failures++;
    }
  }

  if (failures == 0) {
    fprintf(stderr, "ALL PASS (phase0_api)\n");
    return 0;
  }
  fprintf(stderr, "%d FAILURE(S) (phase0_api)\n", failures);
  return 1;
}
