# user defined classes

Classes give qlang its object model. A class instance, a table, and an
external (C++) type all go through the **same object dispatch** — field
get, field set, method call, string conversion. A script cannot tell them
apart syntactically. (Dispatch design: `docs/EXTERNAL_TYPES.md` §9.)

```python
class Money {
   static banner = "GreatAccount";
   total = 0

   # constructor
   __init__(self, amount)[] {
      self.total = amount
   }

   # destructor
   __destroy__(self)[] {
   }

   # string representation
   __str__(self)[] {
      return f"balance: {self.total} at {banner}"
   }

   receive(self, amount)[] {
      self.total += amount
   }

   spend(self, amount)[] {
      self.total -= amount
   }

   balance(self)[] {
      return self.total
   }

   static getBanner()[] {
      return banner;
   }
   static setBanner(newBanner)[] {
      banner = newBanner;
   }
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")   # balance: 150
money.spend(15)
println(f"balance: {money.balance()}")   # balance: 135

println(money) # expected: output from _str__()
println(f"{money}") # expected: output from _str__()

Money::setBanner("OrangePumpkin")
println(Money::getBanner()) # expected: "OrangePumpkin"
println(f"{Money::banner}") # expected: "OrangePumpkin"

printf(f"banner: {money.banner}") #expected: "OrangePumpkin"
money.banner = "Turtle"
printf(f"banner: {money.banner}") #expected: "Turtle"
printf(f"banner: {Money::banner}") #expected: "Turtle"


```

## static

can only exists inside a class.

`static variables` are bound to the entire class 
they can be read and modified by functions in the class.

`static functions` can be addressed via the class name.

static can combined with const.
static cannot be combined with local or let.

`static` (and the `::` scope resolution operator) is a **new extension** beyond v1 —
v1 classes have no static members (see `## v1 restrictions`).

## class body

- a class is declared in **global scope only** (same rule as `def`).
- the body contains **field declarations** (`name = value`) and **methods**
  (`name(self, args)[] { }`). `self` is always the first method parameter.
- field declarations are **defaults**: every instance starts with a copy of
  the declared fields.
- a field declared `const` is read-only for the life of the instance;
  `x.f = 1` on a const field is a runtime (Type) error naming the field.
  This matches external types, where a field binding without a setter is
  read-only.
- method names are **unique** within a class; duplicate method or dunder
  names are compile-time errors. **No overloading in v1.**
- dunders, at most one each, all optional:
  - `__init__(self, ...)` — constructor
  - `__destroy__(self)` — finalization, runs exactly once
  - `__str__(self)` — string representation

## construction

`let x = ClassName(args)` creates an instance:

1. the instance is created holding the declared field defaults.
2. if `__init__` exists, it is called with `self` + args.
3. a class without `__init__` is constructed with **zero args only**;
   passing args to it is a compile-time error at the call site.
4. wrong arg count for an existing `__init__` is a compile-time error
   (methods have fixed arity — the compiler knows it for source classes,
   and the binding table knows it for external types).

## field access

- `obj.field` reads the instance field. An undeclared field name is a
  runtime error naming the field and the class.
- `obj.field = v` writes it. The field must be **declared**: classes have a
  **closed** field set — unlike tables, there is no open key insertion.
- fields hold qlang values of any type, including other instances and
  external objects.

## method calls

- `obj.method(args)` binds `self` to the instance implicitly.
- an unknown method is a runtime error naming the method and the class.
- methods may call other methods of the same instance via `self`.

## string conversion

- `str(obj)` and f-string interpolation of a whole object (`f"{obj}"`) call
  `__str__` when present.
- a class without `__str__` converts to its class name (`"Money"`).
  (Conformance note: an external type without a `__str__` binding is a Type
  error — it has no sensible default. See the conformance table.)
- f-string field/method interpolation (`f"{money.balance()}"`) goes through
  the same object dispatch.

## lifetime and ownership

- instances are **reference counted**. Assigning, passing, and returning
  share the instance — never a copy.
- `__destroy__` runs exactly once, when the count reaches zero.
- class instances are always **script-created and script-owned** — the
  engine never hands one in, so reference counting is the complete model.
  External (C++) objects follow the engine-authoritative rule instead: the
  engine owns, and the script holds a weak reference to handed-in objects
  (`docs/EXTERNAL_TYPES.md` §3).

## type identity

- `type(x)` returns the class name string for an instance
  (`type(money) == "Money"`). For external objects it returns the
  registered type name.

## v1 restrictions

- no inheritance (no base clause; the dispatch reserves a `base` hook for a
  later version)
- no method overloading, no operator overloading
- no static / class methods
- instances are not tables: the `$` dynamic-key pipeline does not apply to
  them; field access is by declared name only

