# value_share.ql — Phase 2 value-semantics regression (SPEC §types
# "value semantics", plan docs/plans/deepcopy-plan.md Phase 2).
#
# Every non-primitive except `string` shares its storage on assignment and on
# call; a literal evaluates to fresh storage every time. `deepcopy` is the
# only way to fork the storage.

def mutate(x)
  x[0] = 42
end

def addNine(s)
  s[] = 9
end

# --- share on assignment -------------------------------------------------
local a = [1, 2]
local b = a
b[0] = 99
assert(a[0] == 99, "array must share on assignment")

local s1 = set(1, 2)
local s2 = s1
addNine(s2)
assert(len(s1) == 3, "set must share on assignment")

# --- share on call -------------------------------------------------------
local c = [1, 2]
mutate(c)
assert(c[0] == 42, "array must share on call")

# --- a literal is fresh on every evaluation ------------------------------
local l1 = [1, 2, 3]
l1[0] = 99
local l2 = [1, 2, 3]
assert(l2[0] == 1, "array literal must not alias the constant pool")

local n1 = [[1, 2]]
let inner = n1[0]
inner[0] = 99
local n2 = [[1, 2]]
assert(n2[0][0] == 1, "nested literal must be fresh too")

local q1 = set(1, 2)
q1[] = 3
local q2 = set(1, 2)
assert(len(q2) == 2, "set literal must not alias the constant pool")

# One table literal per iteration must be a distinct table (the constant-pool
# table aliasing bug collapsed all rows into the last one).
local cards = []
for i = 1 to 5
  local card = {n = 0, v = 0}
  card.n = i
  card.v = i * 10
  cards[] = card
next i
local acc = 0
for i = 0 to 4
  acc = acc + cards[i].v
next i
assert(acc == 150, "table literal must be fresh per evaluation")

# --- deepcopy forks the storage -----------------------------------------
local d1 = [1, [2, 3]]
local d2 = deepcopy(d1)
let dn = d2[1]
dn[0] = 99
assert(d1[1][0] == 2, "deepcopy must not share the nested array")
assert(d2[1][0] == 99, "deepcopy result must be mutable")

# --- a cycle through an array survives deepcopy -------------------------
# `==` cannot be used here: comparing two cyclic values by content does not
# terminate (SPEC has no identity operator yet). Mutating through the cycle
# proves the copy points at itself.
local cyc = [1]
cyc[] = cyc
local cc = deepcopy(cyc)
assert(len(cc) == 2, "deepcopy must preserve a self-referencing array")
let tail = cc[1]
tail[0] = 77
assert(cc[0] == 77, "the copied cycle must point at the copy")

# --- string stays the value-type exception ------------------------------
local st = "ab"
local st2 = deepcopy(st)
assert(st == "ab", "string must be immutable")
assert(st2 == "ab", "deepcopy of a string yields an equal string")

# --- conversions and operators do not alias -----------------------------
local ca = [1]
local cb = ca + [2]
assert(len(ca) == 1, "concat must not reach into its left operand")
assert(len(cb) == 2, "concat result carries both sides")

local cs = set(1, 2)
local car = cast<array>(cs)
car[] = 3
assert(len(cs) == 2, "cast<array>(set) must not alias the source set")

local x = [1, 2]
x += x
assert(len(x) == 4, "self-append must double the array")

println("ok")
