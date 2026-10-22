
#include <iostream>
#include <string>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;
int main() {
  QLangContext ctx;
  ctx.set("greeting", QValue::String("hello"));
  ctx.set("pi", QValue::Float(3.14159), true);
  ctx.set("MAX", QValue::Int(99), true);
  if (ctx.get("greeting").asString() != "hello") { std::cerr << "FAIL1\n"; return 1; }
  if (ctx.get("pi").asFloat() < 3.14 || ctx.get("pi").asFloat() > 3.15) { std::cerr << "FAIL2\n"; return 1; }
  QValue vi = qval_int(42);
  QValue vf = qval_float(2.5);
  QValue vs = qval_str("world");
  QValue vb = qval_bool(true);
  QValue vn = qval_nil();
  QValue vy = qval_byte(0xFF);
  std::cerr << "DEBUG: vi=" << vi.asInt() << " vf=" << vf.asFloat() << " vs=" << vs.asString()
            << " vb=" << vb.asInt() << " vn_isNil=" << vn.isNil() << " vy=" << (int)vy.asByte() << "\n";
  if (vi.asInt() != 42) { std::cerr << "FAIL_vi\n"; return 1; }
  if (vf.asFloat() != 2.5) { std::cerr << "FAIL_vf\n"; return 1; }
  if (vs.asString() != "world") { std::cerr << "FAIL_vs\n"; return 1; }
  if (vb.asInt() != 1) { std::cerr << "FAIL_vb\n"; return 1; }
  if (!vn.isNil()) { std::cerr << "FAIL_vn\n"; return 1; }
  if (vy.asByte() != 0xFF) { std::cerr << "FAIL_vy\n"; return 1; }
  std::string src = "greeting = \"changed\"\nprintln(greeting)\n";
  ctx.run(src);
  std::cerr << "DEBUG: greeting after run=" << ctx.get("greeting").asString() << "\n";
  if (ctx.get("greeting").asString() != "changed") { std::cerr << "FAIL3\n"; return 1; }
  std::string src2 = "pi = 2.0\n";
  bool caught = false;
  try { ctx.run(src2); } catch (const QLangError &e) { caught = true; std::cout << "PASS const rejected\n"; }
  if (!caught) { std::cerr << "FAIL4\n"; return 1; }
  if (ctx.get("pi").asFloat() < 3.14 || ctx.get("pi").asFloat() > 3.15) { std::cerr << "FAIL5\n"; return 1; }
  std::cout << "ALL PASS\n";
  return 0;
}
