# strict typing

the enforcement is at the QLangContext level. C++ api need to configure this setting.


## enforcement levels

|enforcement level||
|-|-|
|0|no type checking (default)|
|1|check type if declared|
|2|enforce type checking and throw an error if type is not declared|

nil type is implicit and doesn't need to be declared

when strict typing is on, type declarations are strictly enforced.

type declarations are optional. no enforcement. (at level 2 the absence of a
declaration is itself an error — see `## level 2 — undeclared types`.)

## enforcement level constants

to prevent magic numbers, the levels are named constants of `QLangContext`:

|constant|value|
|-|-|
|`QLangContext::STRICT_OFF`|0|
|`QLangContext::STRICT_DECLARED`|1|
|`QLangContext::STRICT_ALL`|2|

C++ api:

```
void setStrictTypingLevel(int level);  // outside [0, 2] clamps to the nearest
                                       // constant and emits a warning
int strictTypingLevel() const;
```

the level defaults to `STRICT_OFF`, survives `QLangContext::clear()`, and
applies to everything compiled and executed on that context. the `qlcc` driver
flag `--strict <level>` (see `18-qlang-run.md`) sets it for a driver session.

## example of regular functions
```
# assuming strict typing is level 1

def hello_world(name:str, age:int) {
}

# exception: age is not an int
hello_world("Tommy", "24")

# exception: name is not a string
hello_world(112, 24)

# exception: storage for age is too large for int
let age:long = 24
hello_world("Tommy", age)

# exception: age is a byte not an int
age:byte = 24
hello_world("Tommy", age)

def sum_int2int(a:int, b:int) -> int {
    a += b
    # exception: function must return an int
}
```
## lambda examples

```
let hello_word = (name:str, age:int)[]->str{
    return f"hello {name}. you are {age} years old"
}
```

## type annotations

a parameter may carry a type annotation (`name:type`) and a callable may carry
a return annotation (`-> type`), on `def`, lambda/closure literals, generators
and class methods alike. annotations are optional at levels 0 and 1.

## rules

|types|rules|
|--|--|
|string| only allow a string
|bool|only allow a bool; a bool never satisfies an integer parameter (`cast<int>(…)` is the conversion)
|long, int, short, char|coerce only allow signed integer types (see left column) of smaller storage size. implicit conversion
|qword, dword, word, byte|coerce only allow unsigned integer types (see left column) of smaller storage size. implicit conversion
|float|only allow floats|
|array|only allow arrays|
|set|only allow set|
|table|only allow table|
|class|objects of that give class

the two integer rows are read against these groups:

- signed group: `char, short, int, long`
- unsigned group: `byte, word, dword, qword` (`dword` is the `unsigned` type)

an argument satisfies a declared parameter type exactly when one holds:

1. its type is the declared type (exact match);
2. it is in the same group as the declared type and its storage size is not
   larger than the declared type's (implicit widening: `short` satisfies an
   `int` parameter, `byte` satisfies a `word` parameter).

Everything else is rejected. Cross-sign never coerces (`byte` is not an `int`;
no signed type satisfies an unsigned parameter or the reverse). Storage is
never narrowed implicitly (`long` does not satisfy an `int` parameter — the
`long` storage is too large). `cast<type>` is the only other conversion. The
non-integer rows are exact matches; a class annotation (`p:Person`) is
satisfied by instances of that class — script classes by class name, external
(C++) types by their registered type name.

nil passes a declared parameter type. nil never satisfies a declared return
type: a callable whose body ends without a return statement returns nil, so
`def sum_int2int(a:int, b:int) -> int { }` raises at the return — the
"function must return an int" case above.

Where the types are statically known a mismatch is a compile error; otherwise
it is checked at runtime at the call boundary and at the return.

## level 2 — undeclared types

at `STRICT_ALL` a compile error (`undeclared type for <site>`) is raised where
the type of a value cannot be determined:

- a callable parameter or return without an annotation;
- a declaration whose initializer type is unknown (e.g. a call to a callable
  without a return annotation);
- a `return` expression whose type the compiler cannot determine.

Exempt — the nil-implicit invariant and existing constructs keep working:
declarations without an initializer (`local z`, nil by default), captures,
loop variables, `global` declarations, and class fields. Return annotations are
enforced only where written; there is no `-> nil` annotation.

## scope of enforcement

Checked: call sites of annotated callables, return values against `-> type`
annotations, and the duplicated-variable diagnostic below.

Not checked at any level: built-in functions (their existing behavior is
unchanged); the typed declaration/assignment coercion (`local x:int = <any>`,
the `CONV` coercion — it remains lenient at every level, see
`03-declarations.md`); and `cast<>` arguments.

## redeclared variables within a scope

```
local a
local a
```

|enforcement level||
|-|-|
|0|no warning, no error|
|1|warning: duplicated variable "a" declaration within scope|
|2|error: duplicated variable "a" declaration within scope|

the diagnostic text is exactly `duplicated variable "a" declaration within
scope` (the declared name in quotes) with the per-level prefix; the level-1
warning goes to the context's warning output channel, the level-2 error is a
compile error.

## error messages

|site|message|
|-|-|
|argument mismatch (compile-time or at the call)|`parameter 'age' of 'hello_world' expects int, got string`|
|return mismatch|`function 'sum_int2int' must return int, returned nil`|
|duplicate declaration (level 1 / level 2)|`warning: duplicated variable "a" declaration within scope` / `error: duplicated variable "a" declaration within scope`|
|undeclared type (level 2)|`undeclared type for 'x'`|
