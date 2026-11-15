# F4 P3b end to end (docs/plans/archive/class-export-plan.md): constructing classes
# exported by a `.ql` module resolved beside this file. Both spellings — the
# flat `from` name and the module-qualified name — build instances from the
# prototype cell; method dispatch, `type()`, the captured-const `__init__`
# and the shared field defaults all ride the clone.
from class_export_leaf import Point, Plain
import class_export_leaf

let p = Point(3, 4)
assert(p.x == 3, "field written by the imported __init__")
assert(p.sum() == 7, "method call through a receiver variable")
assert(p.tag == "leaf", "__init__ reads its captured module const")
assert(type(p) == "Point", "type() names the imported class")

let q = class_export_leaf.Point(1, 2)
assert(q.sum() == 3, "the qualified spelling constructs")
assert(type(q) == "Point", "type() on the qualified construction")

let z = Plain()
assert(z.flag == true, "a zero-__init__ class constructs")
let z2 = class_export_leaf.Plain()
assert(z2.flag == true, "the qualified zero-__init__ construction")

let sa = p.shared
sa[] = 9
assert(len(q.shared) == 1, "field defaults are shared by handle")
println("ok")
