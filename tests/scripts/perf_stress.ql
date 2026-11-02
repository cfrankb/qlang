# perf_stress.ql — fixed deterministic workload for the perf baseline
# (SPEC # performance tests: parser / compiler / execution time).
#
# Mixed workload, no rand / I/O / wall-clock:
#   1. nested loops (integer arithmetic)
#   2. array churn (append, index, chained index)
#   3. table churn (built field-by-field in a loop, field read/update)
#   4. string ops (concat, sprintf, f-string, substr, join)
#   5. closure calls
#   6. while-loop scan
#
# The single println at the end proves execution completed; every value it
# prints is deterministic. Do not add rand(), I/O, or time-dependent
# constructs here — the perf gate compares against tests/perf/baseline.json.

# The workload deliberately exceeds the default 100k VM step guard; raise it
# so the script runs to completion (the guard is a live VM setting).
set_max_steps(10000000)

# --- 1. nested loops ---
local total = 0
for i = 1 to 200
  for j = 1 to 200
    total = total + i * j
  next j
next i

# --- 2. array churn ---
local rows = []
for i = 1 to 500
  local r = []
  r[] = i
  r[] = i * 2
  r[] = i * i
  rows[] = r
next i
local picked = []
for i = 0 to 499
  picked[] = rows[i][2]
next i

# --- 3. table churn ---
local cards = []
for i = 1 to 500
  local c = {n = 0, v = 0}
  c.n = i
  c.v = i * 10
  cards[] = c
next i
local acc = 0
for i = 0 to 499
  acc = acc + cards[i].v
next i
local scratch = {a = 0, b = 0, c = 0}
for i = 1 to 500
  scratch.a = scratch.a + i
  scratch.b = scratch.b - i
  scratch.c = scratch.c + 1
next i

# --- 4. string ops ---
local s = "seed"
for i = 1 to 2000
  s = s + "-" + sprintf("%04d", i)
next i
local parts = []
for i = 1 to 100
  parts[] = f"part-{i}"
next i
local joined = join(",", parts)
local chunk = substr(s, 10, 60)

# --- 5. closure calls ---
local square = (x)[] { return x * x }
local fs = 0
for i = 1 to 1000
  fs = fs + square(i)
next i

# --- 6. while scan over picked ---
local idx = 0
local wsum = 0
while (idx < 1000)
  wsum = wsum + picked[idx % 500]
  idx = idx + 1
end

local s_len = len(s)
local j_len = len(joined)
println(f"perf_stress total={total} acc={acc} fs={fs} wsum={wsum} s_len={s_len} j_len={j_len} chunk={chunk}")
