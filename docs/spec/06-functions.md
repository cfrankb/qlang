# Functions
## build-in functions

|helpers|-|-|
|--------|-|-|
|toStr(var)     |  convert to string| deprecated
|toInt(var)     |  convert to int| deprecated
|type(var)      |   return a string representation of the type|
|print(var)       | print a string|
|println(var)     | print a string with carriage return
|printf(fmt, ...) | mirror C printf
|sprintf(fmt, ...) |same format as sprintf. return new string
|len(var)         | structures return storage sizes, collections return their length and other types return 0
|substr(str, start, length)| return a substring from index start. length is optional
|delete var      | delete variable. error when trying to delete external variables/structures or nil.
|str(var)        | return string representation of variable
|int(var)        | return variable cast to int; string are parsed as decimal. other types return 0
|slice(array, start, length) | return an array slice
|isset(var)| return true if variable, array index or table field exists. otherwise returns false
|join(separator, arr)| join the array element using separator
|assert(bool, str)| if bool is false thrown an exception using text provided by str
|deepcopy(var)| return an independent recursive copy of the variable|
|shallowcopy(var)| return a top-level copy of the variable; elements / field values stay shared|


```python
a = [1,2,3]
println(join(", ", a))
# output: 1, 2, 3

a = [1,2,3,4,5]
b = slice(a, 0, 2)
# b = [1,2]
```


### deepcopy

`deepcopy(x)` returns an independent copy of `x`. it is the only way to detach
a non-primitive: assignment and call arguments share, see
[01-types.md](01-types.md) `## value semantics`.

|input|result|
|-|-|
|nil, bool, numbers|a copy of the value|
|string|a copy of the bytes (strings are always copied, never shared)|
|array, set|a new container, every element deep-copied|
|table|a new table, every value deep-copied|
|class instance|a new instance of the same class, see [10-classes.md](10-classes.md) `## deepcopy`|
|closure, generator|the same handle (shared, never copied)|
|external object, bound method, type reference|the same handle (shared, never copied)|

closures, generators and external references carry live state or view a C++
object. copying them would duplicate state that cannot be duplicated, so
`deepcopy` returns the original handle.

```python
a = [1, 2, 3]
b = deepcopy(a)
a[] = 4
println(str(b))        # expected: [1, 2, 3]      (b is independent)
println(str(a))        # expected: [1, 2, 3, 4]

s = [1, 2]
t = s                  # shared: assignment does not copy
t[] = 9
println(str(s))        # expected: [1, 2, 9]      (t and s are the same array)

u = deepcopy(s)        # independent copy
u[] = 7
println(str(s))        # expected: [1, 2, 9]      (s unaffected)

tbl = {name = "x", tags = ["a"]}
v = deepcopy(tbl)
tbl.tags = ["a", "b"]
println(str(v.tags))   # expected: ["a"]          (nested value copied, not shared)
```

- the copy is **recursive**: containers nested at any depth are copied.
- **cycles are preserved**: a value that transitively contains itself is
  copied once and the copy refers to its own copy, not to the original. the
  copy terminates.
- `deepcopy` never mutates its argument.
- exactly one argument. `deepcopy()` and `deepcopy(a, b)` are compile errors.
- primitives and strings are already copied, `deepcopy` of them is legal and
  returns an equal value.


### shallowcopy

Implemented 2026-11-15 (Phase 1 of
[../plans/archive/deferred-copy-semantics-and-class-export-plan.md](../plans/archive/deferred-copy-semantics-and-class-export-plan.md),
the F4 clone round).

`shallowcopy(x)` copies the **top level only**: the result is a new array,
set, table, blob, or instance, but its elements and field values are
inserted as-is, so every nested payload stays shared by handle.

|input|result|
|-|-|
|nil, bool, numbers|the value (already independent)|
|string|the string (strings are always copied, never shared)|
|array, set|a new container, every element inserted as-is|
|table|a new table, every value inserted as-is|
|class instance|a new instance of the same class; class meta shared, field values inserted as-is|
|blob|a new buffer (bytes are not shared handles, so shallow ≡ deep here)|
|closure, generator|the same handle (shared, never copied)|
|external object, bound method, type reference|the same handle (shared, never copied)|

- `__deepcopy__` is **not consulted**: the hook is the deep-copy contract
  (see [10-classes.md](10-classes.md) `## deepcopy`).
- there is no cycle map — the copy never recurses, so a self-referencing
  array clones to `[<original>]`.
- exactly one argument. `shallowcopy()` and `shallowcopy(a, b)` are compile
  errors.
- `shallowcopy` never mutates its argument.


## functions (qlang native)

```python
def function_name(param1, param2)
end

def hello(name, year)
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
end

hello("Frank", 2026)

def hello2(name, year) {
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
}

# def is only valid in global scope. no closure.

local func1 = (name, year)[] { # no capture
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
}
func1("Frank", 2026)

local world = "world"
# this is the lambda implementation
local func2 = (name, year)[world] {  # explicit capture passed by reference
   println("hello " + world)
   printf("hi %s it is %d\n", name, year)
   return true
}

func2("Nana", "2003")

```

only explicit capture being passed from caller context;
closure list is mandatory []. closure only available in function scope.
reference still maintained even after reassignment 
return value is optional. defaults to nil
local always apply to the current scope.

functions, lamdas and generators without a return statement will return nil (nil type)

### copy parameters

Implemented 2026-11-15 (Phase 2 of
[../plans/archive/deferred-copy-semantics-and-class-export-plan.md](../plans/archive/deferred-copy-semantics-and-class-export-plan.md)).

`def f(copy x)` deep-copies the argument into the parameter at call entry:
the callee works on an independent copy, so caller-visible state never
changes, in place or by rebind. The copy follows the `deepcopy` rules (see
`### deepcopy`); for primitives and strings it is a no-op.

```python
def push_it(copy bag)
   bag[] = 99
end
local a = [1]
push_it(a)
println(str(a))   # expected: [1]   (the caller's array is untouched)
```

- `copy` is a **contextual** keyword: it is the specifier only where an
  identifier `copy` is followed by an identifier inside a parameter list;
  anywhere else it is a plain identifier (precedent: the import alias `as`;
  [02-lexical.md](02-lexical.md) reserves nothing new).
- typed parameters read `copy x:type`.
- not valid on `self`, on captures (`[x]` cells), or on arrow-closure
  parameters: those uses are parse errors (`copy is not allowed here`).
- it is call-entry sugar over `deepcopy`: call sites and signatures are
  unchanged, so `export def f(copy x)` passes by value across a module
  boundary.

# C++ api

Injected host variables are available to QLang code via the
`QLangContext` API. Two injection modes exist:

```cpp
ctx.set("name", value, false)   // writable from QLang 
ctx.set("name", value, true)    // read-only from QLang; assignment is a compile error
```

Host code constructs values via the `QValue` type (alias for `QLangValue`) and
free helpers:

```cpp
#include "qlang/QValue.h"
using namespace qlang;

ctx.set("greeting", qval_str("hello"));
ctx.set("pi",       qval_float(3.14159), true);
ctx.set("MAX",      qval_int(99), true);

// QValue helpers cover all scalar types plus aggregate types:
qval_nil()
qval_bool(bool)
qval_byte(uint8_t)
qval_char(int8_t)
qval_word(uint16_t)
qval_short(int16_t)
qval_int(int32_t)
qval_unsigned(uint32_t)
qval_qword(uint64_t)
qval_float(double)
qval_str(const char*) / qval_str(const std::string&)
qval_blob(const std::vector<uint8_t>&)
qval_array(ArrayValue)
qval_table(TableValue)
```

Const-injected variables seed the VM register file at start (readable from
QLang) but are excluded from the end-of-run scope sync, so QLang code cannot
mutate them. The compiler guards against assignment with a parse-time error.

Native (C++) functions are injected with `setFunc()`. The callback receives the
context as its first argument (Lua convention) followed by the QLang arguments:

```cpp
ctx.setFunc("double", [](QLangContext& ctx, const std::vector<QLangValue>& args) -> QLangValue {
    return qval_int(static_cast<int32_t>(args[0].asInt()) * 2);
});

ctx.setFunc("add", [](QLangContext& ctx, const std::vector<QLangValue>& args) -> QLangValue {
    return qval_int(static_cast<int32_t>(args[0].asInt()) + static_cast<int32_t>(args[1].asInt()));
});
```

Called from QLang like any builtin: `r = double(21)`. The return value is
assigned to the destination register; returning `qval_nil()` means no result.
Native funcs are resolved at compile time (negative opcode indices) and stored
in `CompiledProgram::nativeFuncs` — no name lookup at runtime.

