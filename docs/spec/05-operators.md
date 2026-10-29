# syntax

type coersion between numbers is always guided by the left operand.

## operators

|operator|role|
|---------|--|
|=  | assign |
|== |equal |
|+= |add or append
|!= |not equal
|<  > <= >= |comparison
|<< >> |bit shift
|^ |binary xor
|& |binary and
|\| |binary or
|&& |logical and
|\|\| |logical or
|() |parenthesis
|-- ++ |decremental/incremental
|%  |modulo
|/ |division
|* |multiplication
|** |exponential
|! |not (inverse)
|;  |silent separator (optional)
|cast|change datatype at runtime
|$| dynamic key resolution operator
|$$| indirect access operator
|::| scope resolution operator
|?|short-hand if operator


## operator ?

short-hand if (pseudo-code)

```
boolean-expr ? value-if-true : value-if-false
```

expanded short-hand (pseudo-code)

```
boolean-expr ? {
      return value-if-true
   } : {
     return value-if-false
   }
```

examples
```
local a = 3

let b = a == 3 ? "This is Three" : "This is NOT Three";
# expected: b to equal "This is Three"


let c = a == 5 ? {
   return "this is 5"
} : {
   return "this is not 5"
}
# expected: c to equal "this is not 5"


```

## operators < and >

|left hand|right hand|analysis
|-|-|-|
|number|number|numeric comparison
|number|float|coerce and numeric comparison|
|number|string|error: invalid comparison
|number|table, array, class, object|error: invalid comparison
|string|string|binary comparison
|string|table, class, array, object|binary comparison with __str__() for table, class and array
|nil|any|error: invalid value for comparison
|array|array|comparison of each value at their relative indexes
|table|table|comparison of each key value
|table|class, object|error: objects cannot be compared
|table|array|error: invalid comparison
|class, object|any|error: objects cannot be compared

same rules apply to `operator <=` and `operator >=`

## operator **

pow is only valid when left and right operands are numbers.
in this implementation, all other cases will return an error.

```
a = 2
b = 3
c = a ** b
println("{c}") # expected: 8

d = 2 ** [1,2] # expected: error
```


## operator *

|left hand|right hand|operation|example|expected|notes|
|-|-|-|-|-|-|
|number|number|multiply|2 * 2|4
|float|float|multiply|1.5 * 1.2|1.8
|int|float|multiply|1 * 1.2|1|float coerced to int
|float|int|multiply|1.2 * 1|1.2|int coerced to float
|string|number|"toto" * 2|error|
|array|number|multiply|[1,2,3]*2|[2,4,6]|
|table|any|n/a|error
|class|number||error|
|number|string|copy|2 * "abc"|"abcabc"

## operator +=

|left-hand|operator|right-hand|result
|-|-|-|-|
|"string"| +=| "_string"|"string_string"
|"string"| +=| 123|"string123"
|"string"| +=| true|"stringtrue"
|"string"| +=| 1.23|"string1.23"
|"string"| +=| any other types|string<\type>
|123| +=| 123|246
|123| +=| 1.23|124
|123| +=| any other types|error
|1.23| +=| 123|124.23
|1.23| +=| 1.23|2.46
|1.23| +=| any other types|error
|[1,2,3]|+=|[5,6]|[1,2,3,5,6]
|[1,2,3]|+=|4|[1,2,3,4]
|[1,2,3]|+=|"s"|[1,2,3,"s"]
|[1,2,3]|+=|1.23|[1,2,3,1.23]
|[1,2,3]|+=|{name="toto"}|error
|[1,2,3]|+=|any other types|error
|{name="toto"}|+=|{age=23}|{name="toto", age=23}
|{name="toto"}|+=|any other types|error

## operator cast
|examples|results|notes|
|-----|-----|-|
|cast\<int>(1.12)| 1| casting to 1. truncate decimals
|cast\<float>(1)|1.0f| make the int into a floating point
|cast\<string>(13)|"13"| turns int into string
|cast\<string>(1.3)|"1.3"| turns float into string
|cast\<word>(-1)|65535| converting int to word removes sign
|cast\<string>([])|depends on \_\_str__()| collections fallback on their string representations
|cast\<int>([])|n/a| error: collections cannot be cast to int
|cast\<array>(123)|n/a| error: numbers cannot be cast to collections
|cast\<array>(set)|n/a| sets are converted to arrays
|cast\<set>(array)|n/a| arrays are converted to sets
|cast\<any>()|n/a| error: empty source. nothing to cast
|cast\<>(123)|n/a| error: empty cast. nothing to do

## operator == and !=
|var type|operator|var type|result
|-|-|-|-|
|nil| ==| nil | true
|nil| ==| number (int, float etc) | false
|nil| ==| collection | false
|nil| ==| table | false
|nil| !=| nil | false
|nil| !=| number | true
|nil| !=| collection | true
|nil| !=| table | true

*important*: `nil` is ONLY equal to itself. ``collection`` implies (table, array, class, set)

|expr|operator|expr|result|notes
|-|-|-|-|-|
|1|==|1|true||
|1|==|1.0|see notes|only if type coercion is possible and results in equivalency|
|3.0|==|3.0|true||
|"3"|==|3|false|strings are not numbers. there is no coersion|
|' '|==|32|true|byte ' ' is 32
|-1|==|(byte)0xFF|false|comparison between signed and unsigned must match intended value 
|1|==|(unsigned)1|true|signed and unsigned int are equal when sign bit is not set
|1|==|object|false|objects are not primitives
|string|==|object|see notes|true only if the __str__ value of the object matches the string
|"toto"|==|"toto"|true|string is equal to itself
|table|==|table|depends|only true if all kv pairs must match. no stray keys
|array|==|array|depends|only true if array is an exact match
|set|==|set|depends|only true if set is an exact match

equivalency works both ways. `operator !=` is the `boolean` inversion of `operator ==`.

## The \$ Operator (Evaluation / Dynamic Key Resolution)

The `$` operator evaluates an identifier at runtime and forces its value into a string representation. It is primarily used for dynamic property key resolution during table literal construction and dynamic expression formatting.

### Constraints and Type Restrictions
Unlike the explicit `cast<string>()` operator which falls back on default string representations for all types, the `$` operator is strictly a **scalar-to-string evaluator**. 

* **Supported Types:** `string/str`, `char`, `byte`, `word`, `short`, `int`, `unsigned`, `qword`, `float`, `bool`, and `nil`.
* **Unsupported Types:** `array`, `table`, and `class`. Invoking the `$` operator on an aggregate collection type will halt execution and depeding on when evaluation is taking place, it will throw either an exception or a compilation error. 

### Behavior Matrix

| Operand Type | Value | Evaluation Result | Status |
|---|---|---|---|
| `int` | `33` | `"33"` | Valid |
| `float` | `19.99` | `"19.99"` | Valid |
| `string` | `"gender"` | `"gender"` | Valid |
| `bool` | `true` | `"true"` | Valid |
| `nil` | `nil` | `"nil"` | Valid |
| `array` | `[1, 2, 3]` | `error: collections not supported` | **Compile/Runtime Error** |
| `table` | `{age=33}` | `error: collections not supported` | **Compile/Runtime Error** |

### Code Examples

#### 1. Valid Scalar Resolution
```python
local age = 33
local str_age = $age  # str_age becomes "33"

local property = "gender"
local t:table = {name="Bob", age=99, $property="male"}
# Resulting table: {name="Bob", age=99, gender="male"}
```

#### 2. Invalid Collection Evaluation
```python
local a = [1, 2, 3]
local err1 = \$a       # Error: collections not supported

local t = {x=1, y=2}
local err2 = \$t       # Error: collections not supported
```

### Compiler and VM Implementation Notes
1. **Scope Boundaries:** The `$` operator respects QLang scope lookup invariants. If an identifier is located in the global scope, it must be explicitly exposed using the `global` keyword before a local `$` evaluation can resolve it inside a function body.
2. **Bytecode Generation:** The compiler must emit a specialized `OP_EVAL_KEY` opcode. This opcode is responsible for checking the target variable type tag before performing the stringification step. If the type tag matches an aggregate or compound collection signature, the VM must raise an execution panic.

## operator $$

The `$$` operator is an **indirect access operator**: it resolves a variable name
at runtime from a string and reads or assigns the referenced variable. Unlike the
`$` operator (scalar-to-string evaluation), `$$` does **not** coerce its operand —
the operand must already be a string.

`$$name` is a first-class expression: it may appear on the right-hand side of
assignments, as a function argument, in conditions, and as a table value.

```python
local a = "b"
local b # b = nil (implicit)
$$a = "toto"
println(a) # expected; "b"
println(b) # expected: "toto"

local c = "d"
$$c = "etc"  # expected: indirect variable error, `d` not declared

local e = 123
$$e = "x" # expected: indirect variable error, invalid name `123`

f = $$a
println(b) # expected: "toto"
g = $$c # expected: indirect variable error, `d` not declared

# assume global module_owner = "cfrankb"
global module_owner
h = "module_owner"
println($$h) # expected: "cfrankb"

local aa = "bb"
$$aa = 1   # expected: indirect variable error, `bb` is not declared
local bb

def foo() { return 1 }
local a = "foo"
x = $$a   # expected: indirect variable error, `foo` is a function
$$a = 5   # expected: indirect variable error, `foo` is a function

const year = 2026
local y = "year"
z = $$y   # z = 2026 (local const read is valid)
$$y = 1   # expected: indirect variable error, `year` is a constant

global module_owner
$$h = "x" # expected: indirect variable error, global write denied
```

### restrictions

- the string must hold a valid variable name.
- assignment requires the variable reference to exist in the local scope. read requires the variable to be visible (local, global declared)
- no creation of new variables
- cannot be applied to any function or lambda
- cannot reference an operator or a reserved word
- assignment is denied global scope access
- can read from global variables and const (if exposed via global keyword)
- can read local const
- cannot read global functions
- declarations are sequential

### resolution and check pipeline

Every `$$` expression runs the same staged pipeline; the first failing stage halts
execution with its error. All messages use the prefix `indirect variable error:`.

| stage | check | failure message |
|---|---|---|
| 1 | operand is a string | `invalid name <value>` |
| 2 | string lexes as a valid identifier | `invalid name <value>` |
| 3 | name is not a reserved word | `reserved word <name>` |
| 4 | name is declared and visible (resolution order below) | `<name> not declared` |
| 5 | binding is not a function/lambda (reads and writes), not a const (writes) | `<name> is a function` / `<name> is a constant` |
| 6 | (writes only) binding is local, not global | `global write denied` |

**resolution order**

- **writes:** current frame locals only. A local that shadows a global resolves to
  the local.
- **reads:** current frame locals, then globals and const exposed via `global` in
  the current scope. Global const is visible everywhere (per the global scope
  rules) and does not require `global`.

A read that misses every level is an error — `$$` never returns nil for an
unresolved name.

### declaration state (sequential declarations)

A name is targetable only after its **declaration event** has executed. The first
declaration of a name is either an explicit `local`/`let` or its first plain
assignment; a later explicit `local`/`let` for the same name is a compile warning
declares into the enclosing function frame, so a `$$` targeting a name declared
in a branch that has not run is a `not declared` error:

```python
local a = "x"
if cond {
   local x = 5
}
$$a = 1   # legal only if cond executed true
```

`$$` never creates variables, so a name whose declaration event has not executed
cannot be introduced by indirection.

**asymmetry note:** direct references resolve frame-wide at compile time, so a
direct use of a name before its declaration reads its default value; `$$`
indirection is deliberately stricter and raises `not declared` until the
declaration event has executed.

### const and function bindings

- **const** (local and global) is readable via `$$`; assignment through `$$` to a
  const binding is an error, consistent with direct assignment.
- **function and lambda** bindings (user `def`, lambda literals, global and
  native functions) are never valid `$$` targets, for reads or writes. Functions
  hosted in tables or classes are table entries, not scope names — they remain
  mutable through ordinary dynamic table access (`t[key]`).

### compiler and VM implementation notes

1. **frame name map:** at compile time the compiler builds, per frame, a static
   map of name → (register, binding kind, declaration bit). Binding kind is one of
   `var`, `const`, `function`, `lambda`. The map covers every name directly
   declared in the frame; a `$$` target absent from the map (and not an exposed
   global) can only be reached through a dynamic name string and fails at stage 4.
2. **declaration bit:** a flag on the existing store/init opcode, set once at the
   name's unique declaration site (explicit `local`/`let` or first plain
   assignment). No new opcode is required for declaration state.
3. **new opcodes:** two — `OP_INDIRECT_READ nameReg → dstReg` and
   `OP_INDIRECT_WRITE nameReg, srcReg`. Both run the pipeline above: string check,
   identifier lexeme check, reserved-word check, map lookup (frame, then exposed
   globals for reads), declaration-bit test, binding-kind test, and (writes) the
   local-only test.
4. **compile-time fast path (optional):** if a literal form `$$"name"` is ever
   supported, stages 2–6 for a constant name resolve at compile time, turning
   unknown names and function/const/global-write violations into compile errors;
   dynamic names keep the runtime path. Semantics are identical.

