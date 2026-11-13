# qlcc (formerly qlang_run)


this is the ql/qlc bytecode compiler

## interface


```
-c                  save compile qlc                --> flag
-a                  save disassembly listing (qla)  --> flag
-r                  alias for --rotate              --> flag
--rotate            still rotates the filename (same as before) 
--output-path       folder to save qlc and qla
--run               run the compiled bytecode
--strict <level>    enforce strict typing level (see `docs/spec/16-strict_typing.md`)
```

flags can be combined
```
-ca                 -c and -a 
-car                -c, -a  and -r
-ar                 -a and -r
```

if no `output-path` is specified then then the full qualified path of the original module is being used,

```
MODIFIED
--import <lib>      loads a lib module, if the qlc file exists, is the same version and is more recent than the match ql then load the qlc. otherwise, compile the ql to qlc and save it to disk before loading.  qlc generated through import are not rotated.
```

if `-a` flag is specified then a disassembly listing is generated for all generated `qlc` files.

only run the bytecodes if `--run` is added to the command line (original qlang_run behavior).
