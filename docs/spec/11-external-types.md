## conformance with external (C++) types

A registered C++ type (`docs/EXTERNAL_TYPES.md`) implements this exact
model. The mapping:

| class concept        | external type binding              |
|----------------------|------------------------------------|
| class name           | `TypeDescriptor.name`              |
| `ClassName(args)`    | `TypeDescriptor.construct`         |
| `__init__`           | `TypeDescriptor.construct`         |
| field declaration    | `FieldBinding` (get + set)         |
| `const` field        | `FieldBinding` with null setter    |
| method `m(self, ...)`| `MethodBinding`                    |
| `__str__`            | `TypeDescriptor.str` (optional)    |
| `__destroy__`        | `TypeDescriptor.destroy` (optional)|
| reference count      | engine-authoritative: `weak_ptr` for handoffs, `shared_ptr` for script-constructed |
| `type(x)`            | `type_info` name                   |

Both kinds of object go through the same four dispatch operations
(GETFIELD / SETFIELD / CALLMETHOD / TOSTRING), so code written against the
class model works unchanged on external types.

# implicit \_\_str\_\_ rules

|type|example|behavior|
|--|--|--|
|numbers|| convert to string
|strings|| remain unchanged
|boolean| |"true" or "false"
|nil|| "nil"
|array|[0,1,2,3,4]| "[0, 1, 2, 3, 4]"
|table|{name="Bob", age=9}| "{name="Bob", age=9}"
|class||use provided \_\_str\_\_() method

table key order is implementation defined (depends on the underlying container).

# warning 

compiler and interpreter warning can be togged off.

all warnings defered until further notice.

