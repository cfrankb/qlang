# modules (internal)

## import statement

```python
import math                   # import module into the namespace
                              # use math.min and matn.max

import math as m              # m.min, m.max

from math import *            # import all public names from the math 
from math import min, max     # import `min` and `max` 

```

Modules are registered from C++ via `ctx.registerModule(name, module)`. The
`import` statement is resolved at compile time: each function in the module
is registered as an external (native) function in the current scope. Qualified
access (`math.min(...)`) is a future extension; v1 uses flat import only.

Module can be written in C++ or Qlang. System modules (C++) are part of the
qlang library.

qlang context (ctx) parses the source. it creates the `main` module.
when a module is registered, it will provide it's own name.
all errors messages will reference the module name.

## math module

```cpp
ctx.registerModule("math", {
  {"min", [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue { ... }},
  {"max", ...},
  {"sum", ...}
});
```

|functions|-|-|
|--------|-|-|
|min(number...\|array)| returns the smallest value in a group number (int, float, byte etc). |
|max(number...\|array)| returns the largest value in the group of numbers (int, float, byte etc). |
|sum(number...\|array)| returns a sum of the group of numbers (int, float, byte etc). |
|rand()|return a random int value|

```
import math
c = min(4, 1)  # c = 1
c = min(2, 0, 4, 3)  # c = 0
c = min([2.5,4,1]) # c = 1
c = min([3,5,6,1], [2,5,1,2]) # errror: only 1 array allowed
#import min and max accept: numbers (int, char, float) or an array ONLY
#min and max refuse data if types are mixed (int and float)
#min and max refuse data if types have mixed signed and unsigned types (char and byte)
#max used the same rules as min

d = sum(1,10,20) # d = 31
d = sum([1,4,20]) # d = 25
d = sum([1,4,20], [50,2]) # d = 77
# sum accepts: numbers (int, char, float etc) or arrays ONLY
# sum return float if a type float is present. coerce non-float to float
# sum return int   if data is mixed signed and unsigned types (char and byte) and no float are present
```

