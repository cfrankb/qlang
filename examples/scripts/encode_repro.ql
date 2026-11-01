# Reproduction script for bytecode-encoding guideline examples.
local n = 5
while n > 0 {
    n = n - 1
}

local s = "hello"

local sum = 0
local i = 0
while i < 10 {
    sum = sum + i
    i = i + 1
}

local a = 3
a = a + 1
a = a - 1
a = a + 2
a = a * 2
local b = -a
local c = 7 + 8
local d = 1 + 2 * 3
local e = a - b + c
println(sum)
