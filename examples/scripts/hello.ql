local a = 32                 # int
local b:byte = 32            # byte
local s = "hello"            # string
local sum = 0                # int

a += 12                      # compound assignment -> 44
s += " world"                # string append

for i = 1 to 5               # for auto-increments i, inclusive
  sum = sum + i
next i

if a == 44 then
  s += " ok"
end

local n = 3
while (n > 0)                 # while with manual decrement
  n = n - 1
end

printf("Louis is TOTO\n")

local hello = (name)[] {
  printf("hello %s\n", name)
  return true
}

hello("Sonia")

for i in [1,2,3,4] {
  printf("coundown: %d\n",i)
}

for c in "string" {
   printf("char: %c\n", c)
}

aa = 5
if (aa==4) {

} else if aa == 5{
  println("ELSE")
}

local events:table = {
  onLoad = (name)[] {
    printf("onLOaD -->%s\n", name);
  },
  onDeath = ()[] {
  }
}

events.onLoad("Tam Tam")


local human = {
  name = "Denise",
  age = 65
}

printf("human: %s age=%d\n", human.name, human.age)

printf("space %d\n", 0x20)

println([1,2,3])
ar = [1,2,3]
ar += [4,5]
for i in ar {
  printf("%d", i)
}
println("")

printf("%s --> len %d\n", join(",", ar), len(ar))

println(sprintf("Today is %s %d", "monday", 2026))

println(f"Hello {human.name}. Formatted")

local x = 1024 / 10
println(f"1024 / 10 = {x}")  # 102
local x = 1024 % 10
println(f"1024 %% 10 = {x}") # 4

local x = 0x678 & 0x70
println(f"0x678 & 0x70 = {x:#04x}") #  expected: 0x678 & 0x70 = 0x0070
println(f"{255:#06x}") # expected: "0x00ff"

#assert(1==0, "this is TRUE")

println("this is the END")

import math

local mx = max(3, 7, -1)
local mn = min(3, 7, -9)
local sm = sum(1, 2, 3)
println(sprintf("max= %d, min=%d, sum=%d", mx, mn, sm))
