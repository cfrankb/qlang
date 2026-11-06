# Polymorphic subscript regression. SPEC declarations: `t["age"] = 33` is
# valid and equivalent to `t.age`; `t[] = 44` is an error. SPEC bytecodes:
# "OpCode indexget" / "OpCode indexput". The base kind is resolved at runtime:
# arrays index, tables and class instances go through ObjectOps.

local t:table = {name="Frank", age=99}
assert(t["age"] == 99, "table subscript read")
t["age"] = 33
assert(t.age == 33, "table subscript write equals member write")
t["tokens"] = 14455
assert(t.tokens == 14455, "table subscript adds a new field")
local k = "age"
assert(t[k] == 33, "subscript key may be a variable")
t["n"] = 1
t["n"] += 5
assert(t["n"] == 6, "table subscript compound assign")

local ab = [0,4,5]
assert(ab[1] == 4, "array subscript read unchanged")
ab[0] = 55
assert(ab[0] == 55, "array subscript write unchanged")
ab[1] += 10
assert(ab[1] == 14, "array subscript compound assign unchanged")
ab[] = 4
assert(len(ab) == 4, "empty subscript still appends to arrays")

class D {
  x = 7
}
local a = D()
assert(a["x"] == 7, "class instance subscript read")
a["x"] = 9
assert(a.x == 9, "class instance subscript write equals member write")

println("ok")
