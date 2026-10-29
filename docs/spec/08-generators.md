# generators

Generators are created by defining a function that uses `yield`. Each call to
`yield` pauses the generator and returns the yielded value. A subsequent call
advances to the next `yield`. When the generator is exhausted, `yield` returns
`nil` (the eof sentinel).

## PROTOTYPE 1


```python
def gen_range(mi, mx) {
   for i = mi to mx {
      yield i
   }
}

local gen = gen_range(1, 3)
local v = gen()        # yields 1
while v != nil {
   println(v)
   v = gen()          # yields 2, then 3, then nil
}
# output:
# 1
# 2
# 3
```


The generator value is a function-like callable that captures its local state.
Each invocation runs the generator body from its last yield point until the next
`yield` or until the body returns/exhausts.

## Syntax

```
yield expr      ; yield a value
yield           ; yield nil (empty yield, equivalent to `return` from generator)
```

`yield` is a reserved word; it cannot be used as an identifier.

## EOF sentinel

When a generator is exhausted (body returns or falls off the end), the next call
returns `nil`. This is the EOF sentinel — distinguishable from a yielded `nil`
because a yielded `nil` would have been produced by an explicit `yield` statement
inside the generator body, while the EOF `nil` is produced by the VM's exhaustion
check.

