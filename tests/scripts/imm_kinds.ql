# imm_kinds.ql -- every v6 immediate kind and form, end to end.
#
# Each literal below is chosen to land in a specific right-hand form of the
# modifier byte (docs/plans/encoding-v6-and-indirect-plan.md 2.2/2.4). The
# assertions prove the value survives encode -> load -> execute; the byte
# layout itself is asserted in encoding_v6_test. To see the forms:
#   qlang_run --asm /tmp/imm.qla --asm-level 4 tests/scripts/imm_kinds.ql
#
#   i8     e=0x40   -128 .. 127
#   i16    e=0x50   -32768 .. 32767   (255 is i16: the compiler never emits the
#                                     unsigned forms, so 255 takes the signed one)
#   i32    e=0x60   -2^31 .. 2^31-1
#   f64    e=0x80   float literals
#   false  e=0x90   true e=0xa0   nil  e=0xb0
#   pool   e=0x08   anything wider than i32, and every unsigned/qword literal

# --- integer forms: i8, i16, i32, and the pool fallback ---------------------
local i8_lo = -128
local i8_hi = 127
local i16_lo = -129
local i16_hi = 32767
local i32_lo = -32769
local i32_hi = 2147483647
local pool_hi = 2147483648
local pool_lo = -2147483648

assert(i8_lo == -128, "i8 lower bound survives")
assert(i8_hi == 127, "i8 upper bound survives")
assert(i16_lo == -129, "i16 lower neighbour survives")
assert(i16_hi == 32767, "i16 upper bound survives")
assert(i32_lo == -32769, "i32 lower neighbour survives")
assert(i32_hi == 2147483647, "i32 upper bound survives")
assert(pool_hi == 2147483648, "past i32 falls back to the const pool")
assert(pool_hi > i32_hi, "pool literal compares greater than the i32 bound")
assert(pool_lo == -2147483648, "negative pool literal survives")

# hex spellings fold like any other int literal: 255 needs i16, not i8
local hex = 0xff
assert(hex == 255, "hex literal folds to an immediate")
assert(hex > i8_hi, "255 is one past the i8 form")

# unsigned and qword literals are never immediate-folded -- they go to the pool
local unsig = 44u
local wide = 4294967295u
assert(unsig == 44, "unsigned literal round-trips through the pool")
assert(wide == 4294967295, "qword literal round-trips through the pool")
assert(type(unsig) == "unsigned", "unsigned keeps its type")

# --- the zero-width forms: false, true, nil ---------------------------------
local t = true
local f = false
local n = nil

assert(type(t) == "bool", "true is a bool")
assert(type(f) == "bool", "false is a bool")
assert(type(n) == "nil", "nil is nil")
assert(t == true, "true round-trips")
assert(f == false, "false round-trips")
assert(n == nil, "nil equals nil")
assert(!(t && f), "bool literals keep their logic")

# --- the float form ---------------------------------------------------------
local half = 1.5
local fsuffix = 1.0f
assert(type(half) == "float", "float literal is a float")
assert(half == 1.5, "float round-trips")
assert(fsuffix == 1, "f-suffixed literal round-trips")
assert(half > 1, "float compares against an int immediate")

# --- truthiness of each kind (the forms carry no payload, so isTrue decides) --
local zero = 0
local empty = []
local filled = [1, 2]
if (zero) { assert(false, "int 0 is falsy") }
if (n) { assert(false, "nil is falsy") }
if (f) { assert(false, "false is falsy") }
if (empty) { assert(false, "empty array is falsy") }
if (!t) { assert(false, "true is truthy") }
if (!half) { assert(false, "non-zero float is truthy") }
if (!filled) { assert(false, "non-empty array is truthy") }

# --- immediates as ALU operands: in-place compound assignment ---------------
# The destination is also the left operand, so each line emits one instruction
# whose right side is the immediate (add/sub with e=0x40, 0x50, 0x60).
local acc = 0
acc += 127
acc -= 128
acc += 32767
acc -= 32768
acc += 2147483647
acc -= 2147483647
assert(acc == -2, "compound immediates across every int form")

local facc = 1.5
facc += 2.25
assert(facc == 3.75, "float compound assignment")

# --- immediates as comparison operands --------------------------------------
local z = 7
local hits = 0
if (z == 7) { hits += 1 }
if (z != 8) { hits += 1 }
if (z < 300) { hits += 1 }
if (z > -32769) { hits += 1 }
assert(hits == 4, "comparisons against immediates all branch")

local count = 0
while (count < 3) { count += 1 }
assert(count == 3, "loop condition against an immediate")

println(f"imm_kinds ok: {i8_lo} {i8_hi} {i16_lo} {i16_hi} {i32_lo} {i32_hi} {pool_hi}")
println(f"imm_kinds ok: {t} {f} {n} {half} {acc} {facc} {count}")
