# Plan C8b end to end for the driver: `ql_module_dep` is nowhere on disk as a
# `.qlc`, and the run's working directory is the build dir — what answers is the
# `ql_module_dep.ql` sitting beside this file, compiled by the engine on the
# spot. Printing the value is the assertion: a module that failed to resolve is
# a compile error, and one that resolved empty prints nothing.
from ql_module_dep import shout

println(shout("c8b"))
