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

```
--assemble <file>   rebuild a qlc image from a qla v1 listing   --> flag
-o <file>           output image for --assemble                 --> option
```

`--assemble` is a standalone mode: no script is compiled and no other mode flag
combines with it. The listing (the `-a` print format) is the whole input — the
constant pool values, the printed `e=0x..` modifier bytes, the operands and the
jump targets (`-> target`, authoritative: the offset is recomputed from the
target and the encoded size) are re-encoded verbatim through the instruction
codec, and the printed addresses are checked against the encoded sizes. The
mnemonics `ja`/`jae`/`jb`/`jbe` are accepted as aliases of `jg`/`jge`/`jl`/`jle`
(see `docs/spec/13-bytecodes.md`, D6); disassembly stays canonical. The v1
listing carries every image section — besides the constants block it prints
`module: <name>`, `globals:` entries (`<name> r<reg> [const]`), `natives:`
entries (`<slot> <name>`), `exports:` entries (`<kind> <name> <ref>
[init-arity]`, kind one of `var` `const` `fn` `class`), `imports:` entries
(`<module> <name> <kind> <ref>`, the §4.6 table order; `ref` is the defining
module's own handle, never range-checked against the importing image) and, per
function, a
`locs:` list (parallel `<line>:<col>` decimals) and a `sigs:` line
(`name:annotation` per parameter, ` -> annotation` return; an annotation is
`-` for undeclared, a builtin type name, `@Class`, or `t<ordinal>`). Pool
values carry their tags in the print: only an `int` integer prints as a bare
decimal (a bare decimal out of the `int` range reads back as `qword`), every
other integer type prints name-tagged (`byte=200`, `unsigned=4000000000`,
`long=-4000000000`, `qword=`, `char=`, `word=`, `short=`), an integral float
carries a forced decimal point (`3.0`), a set prints `set[...]` and a blob
prints `<N: hexbytes>`. Assembling a compiler-written listing therefore
rebuilds a byte-identical `.qlc` image — for every program, module ones
included: `-a` lists the **entry image**, the same artifact `-c` writes (a
linked program's merged form is runnable but not serializable and is never a
listing subject); the `qla_roundtrip` guard compares whole files. The
sections are additive: a hand-written listing may
omit them and assembles with the documented defaults (module `main`, `(0,0)`
locations, undeclared signatures, empty maps). Parse
diagnostics are line-numbered; exit 1 usage, 2 unreadable listing, 3 parse
error, 4 image could not be written. Without `-o` the image is written as
`<stem>.qlc` beside the listing.

