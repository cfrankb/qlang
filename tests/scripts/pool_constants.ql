# Pool tag fidelity guard (docs/plans/qla-assembler-followups/README.md M3).
# Each construct below lands an ambiguously-printable value in the constant
# pool: folded enum members of narrow/wide base types (byte, unsigned, long,
# qword tags) and an integral float inside a table literal. The qla_roundtrip
# whole-byte comparison is the actual proof; this script is its producer.
enum Ebyte:byte { BA, BB }
enum Euns:unsigned { UA }
enum Elong:long { LA }
enum Eqw:qword { QA }
local t = {x = 3.0, y = 7}
println(Ebyte.BA)
println(Ebyte.BB)
println(Euns.UA)
println(Elong.LA)
println(Eqw.QA)
println(t)
