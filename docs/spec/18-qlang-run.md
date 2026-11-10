# qlang_run 

this is the ql/qlc loader

## proposed changes


``
REMOVE
--output
--asm
``

```
ADD
-c                  save compile qlc                --> flag
-a                  save disassembly listing (qla)  --> flag
-r                  alias for --rotate              --> flag
--output-path       folder to save qlc and qla
```

```
UNCHANGED
--rotate            still rotates the filename (same as before) 
```

if no `output-path` is specified then then the full qualified path of the original module is being used,

```
MODIFIED
--import <lib>      loads a lib module, if the qlc file exists, is the same version and is more recent than the match ql then load the qlc. otherwise, compile the ql to qlc and save it to disk before loading.  qlc generated through import are not rotated.
```

if `-a` is defined then a disassembly listing is generated for all generated `qlc` files.


