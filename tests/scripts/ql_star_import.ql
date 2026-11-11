# F2 guard (module-export/import plan P1): MODULE_NAME is identity, not an
# entity. Before the fix both modules exported it as an ordinary name, so the
# second `from … import *` always died on 'MODULE_NAME' is already imported.
from ql_star_a import *
from ql_star_b import *

assert(A == "from-ql_star_a", "star-import A lost")
assert(B == "from-ql_star_b", "star-import B lost")

# Still reachable when explicitly asked for, and through the qualifier.
from ql_star_a import MODULE_NAME
import ql_star_b
assert(MODULE_NAME == "ql_star_a", "explicit MODULE_NAME import broke")
assert(ql_star_b.MODULE_NAME == "ql_star_b", "qualified MODULE_NAME broke")
println("ok")
