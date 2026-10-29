# error reporting

Clear and concise error reporting is important to track down bugs.

## requirements

- must report line and column where the error occured (when available)
- must report the module
- must report the class/function/table if applicable

## execution trace

provide the stack frame and execution chain when available.
each node to provide line number, module/class/function/table if applicable.

## format

the top-level catch formats one report (innermost frame first):

```
module 'main' line 12, col 5 in function 'foo': <kind>: <message>
at line 12 in foo
at line 3 in main
```

- line/col come from the pc -> line/col table stamped by the compiler into
  every emitted instruction; missing pieces are omitted.
- the trace has one `at line N in <function>` per frame, innermost first.
- parse/lex errors carry the token position; runtime errors carry the
