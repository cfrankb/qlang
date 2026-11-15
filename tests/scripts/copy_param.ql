# copy_param.ql — regression for the `copy` parameter specifier (deferred
# copy-semantics plan Phase 2, SPEC 06-functions "copy parameters"): the
# argument is deep-copied into the parameter at call entry; `copy` stays a
# plain identifier anywhere else.
def push_it(copy bag)
  bag[] = 99
end
local a = [1]
push_it(a)
assert(len(a) == 1, "caller array untouched")
local alias = a
alias[] = 5
assert(len(a) == 2, "no-copy path still shares")

def rebind_it(copy v)
  v = [9]
end
local b = [1]
rebind_it(b)
assert(b[0] == 1, "rebind inside is invisible")

def deep_touch(copy t)
  local w = t.inner
  w.n = 2
end
local tbl = {inner = {n = 1}}
deep_touch(tbl)
assert(tbl.inner.n == 1, "copy is deep, not shallow")

def str_touch(copy s)
  s = s + "!"
end
local s = "hi"
str_touch(s)
assert(s == "hi", "string param identical with/without")

def g(copy)
  return copy + 1
end
assert(g(41) == 42, "copy is a plain identifier in param position alone")

class P {
  x = 0
  mutate(self, copy arr)[] {
    arr[] = 7
  }
}
local m = [1]
local p = P()
p.mutate(m)
assert(len(m) == 1, "method copy param")

local c = [1]
def mixed(n, copy bag:array, tag)
  bag[] = n
  tag = 0
end
mixed(5, c, "t")
assert(len(c) == 1, "mixed param list copies only the marked param")

println("copy_param OK")
