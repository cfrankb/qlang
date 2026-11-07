# flags.ql — the flag-flow regression (plan docs/plans/m3-flag-flow-plan.md S6,
# SPEC §bytecodes "extended OpCode eq, lt, gt, lte, gte" / "OpCode tst").
#
# Every comparison below sits in *control* position (`if` / `while`), because
# that is the only place the compiler lowers a condition to a flag compare +
# flag jump; in value position the same source still emits the value-producing
# `eq`/`lt`/... (see cmp.ql). The dual-comparator rule (plan §3 D1) pairs each
# condition with the compare whose set bits are exactly the states where the
# condition is false, and keeps the negative jump of `cmp`:
#
#   a == b   eqf  + jne      a != b   eqf  + je
#   a <  b   gtef + jge      a >  b   ltef + jle
#   a <= b   gtf  + jg       a >= b   ltf  + jl
#
# Truthiness (`if x`) and the short-circuits (`&&`, `||`, `!`) deliberately keep
# the deprecated `jmpfalse` spelling (plan §3 D4) — the last section asserts
# they still behave, so their removal stays a Phase-3 change.
#
# To see the shapes:
#   qlang_run --asm /tmp/flags.qla --asm-level 4 tests/scripts/flags.ql

class Point {
  x = 0

  __init__(self, v)[] {
    self.x = v
  }

  __str__(self)[] {
    return f"P{self.x}"
  }
}

# A call in a comparison operand: both sides are staged into registers *before*
# the compare, so the flags are never clobbered between compare and jump.
def tag(k)
  return f"<{k}>"
end

# --- string relations in if position ---------------------------------------
local apple = "apple"
local banana = "banana"
local hits = 0

if (apple < banana) { hits += 1 }
if (apple <= banana) { hits += 1 }
if (apple <= apple) { hits += 1 }
if (banana > apple) { hits += 1 }
if (banana >= apple) { hits += 1 }
if (apple >= apple) { hits += 1 }
if (apple == apple) { hits += 1 }
if (apple != banana) { hits += 1 }
assert(hits == 8, "every string relation branches the way it reads")

if (banana < apple) { assert(false, "banana is not below apple") }
if (banana <= apple) { assert(false, "banana is not at or below apple") }
if (apple > banana) { assert(false, "apple is not above banana") }
if (apple >= banana) { assert(false, "apple is not at or above banana") }
if (apple == banana) { assert(false, "apple is not banana") }
if (apple != apple) { assert(false, "apple equals itself") }

# the negative branch of each pair is really taken (a wrong jump map would
# silently never branch, which is the bug plan §6 records)
local misses = 0
if (apple > banana) { misses += 1 } else { misses += 2 }
if (apple == banana) { misses += 1 } else { misses += 2 }
if (banana <= apple) { misses += 1 } else { misses += 2 }
assert(misses == 6, "the else side of a flag compare is reachable")

# --- a loop whose guard is a string compare ---------------------------------
# Proves the fused pair iterates instead of leaving on the first test.
local names = ["alpha", "beta", "gamma"]
local i = 0
local scanned = 0
local stop = "gamma"
while (i < len(names) && names[i] < stop)
  scanned += 1
  i += 1
end
assert(scanned == 2, "string-guarded loop stops at the sentinel")

local prefix = ["a", "a"]
local countdown = "ccc"
local target = "aaccc"
local ticks = 0
while (countdown != target)
  ticks += 1
  countdown = prefix[ticks - 1] + countdown
end
assert(ticks == 2, "string != loop guard iterates to its sentinel")

# --- float against int ------------------------------------------------------
local half = 2.5
local whole = 2
if (half < 3) { hits += 1 }
if (half > whole) { hits += 1 }
if (half >= whole) { hits += 1 }
if (half <= 2.5) { hits += 1 }
assert(hits == 12, "float/int relations branch correctly")
if (half < whole) { assert(false, "2.5 is not below 2") }
if (half > 3) { assert(false, "2.5 is not above 3") }

# `==` compares numbers through int64, so a float truncates: 2.5 == 2 is true
# while 2.5 > 2 is true as well (ordering switches to double). Shipped
# behaviour, recorded here so a future change to either path shows up.
if (half == whole) { hits += 1 } else { assert(false, "== truncates the float") }

# --- arrays and tables in control position ----------------------------------
local low = [1, 2, 3]
local high = [1, 3]
if (low < high) { hits += 1 }
if (low != high) { hits += 1 }
if (low == low) { hits += 1 }
assert(hits == 16, "array ordering and equality branch correctly")
if (high < low) { assert(false, "[1,3] is not below [1,2,3]") }
if (low == high) { assert(false, "the arrays differ") }

local one = {n = 1}
local same = {n = 1}
local other2 = {n = 2}
if (one == same) { hits += 1 }
if (one <= same) { hits += 1 }
if (one < other2) { hits += 1 }
if (one != other2) { hits += 1 }
assert(hits == 20, "table ordering and equality branch correctly")
if (one > same) { assert(false, "equal tables do not order above each other") }
if (one == other2) { assert(false, "{n:1} is not {n:2}") }

# --- an object compared through __str__ -------------------------------------
let p1 = Point(1)
local pstr = "P1"
local above = "P2"
if (p1 == pstr) { hits += 1 }
if (p1 <= pstr) { hits += 1 }
if (p1 < above) { hits += 1 }
if (p1 != above) { hits += 1 }
assert(hits == 24, "object __str__ comparison branches correctly")
if (p1 == above) { assert(false, "P1 is not P2") }
if (p1 > above) { assert(false, "P1 is not above P2") }

# --- mixed string / int equality --------------------------------------------
local three = "3"
if (three == 3) { assert(false, "a string never equals an int") } else { hits += 1 }
if (three != 3) { hits += 1 }
assert(hits == 26, "mixed string/int equality branches correctly")

# --- calls on both sides of the compare -------------------------------------
if (tag(1) == tag(1)) { hits += 1 }
if (tag(1) != tag(2)) { hits += 1 }
if (tag(1) < tag(2)) { hits += 1 }
assert(hits == 29, "calls stage their operands before the flags are set")

# --- a pool right operand ---------------------------------------------------
# A string literal too long for an immediate is a const-pool entry, and the
# compare shares the entry the `mov` above already created (plan §11 item 10).
if (apple == "apple") { hits += 1 }
if (apple < "banana") { hits += 1 }
if (apple != "applesauce") { hits += 1 }
assert(hits == 32, "pool-right flag compares branch correctly")

# --- truthiness and short-circuits keep the jmpfalse path (plan D4) ----------
local text = "text"
local list = [1]
local map = {k = 1}
if (text) { hits += 1 }
if (list) { hits += 1 }
if (map) { hits += 1 }
if (!apple) { assert(false, "a non-empty string is truthy") }
if (apple && banana) { hits += 1 }
if (apple == banana || apple == apple) { hits += 1 }
if (!(apple == banana)) { hits += 1 }
if (apple == banana && apple == banana) { assert(false, "&& of two false tests") }
assert(hits == 38, "truthiness and short-circuits still branch")

local flag = "on"
while (flag)
  hits += 1
  flag = ""
end
assert(hits == 39, "a truthiness while guard still iterates")

println(f"flags ok: hits={hits} scanned={scanned} ticks={ticks} misses={misses}")
