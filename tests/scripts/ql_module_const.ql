# F1 guard (module-export/import plan P0): a module-level const read by a
# `def` of the same module has ONE identity. Before the fix the def's def-time
# capture bound a frame-local cell, and releasing the module-init frame moved
# the value out of the table — the importers then read an emptied cell. The
# third assert is that signature: read the const AFTER calling the def.
from ql_module_const_dep import TAG
from ql_module_const_dep import tag

assert(TAG == "tagged", "imported const reads empty")
assert(tag() == "fn:tagged", "the module def does not read its own const")
assert(TAG == "tagged", "calling the def emptied the shared cell")
println("ok")
