#pragma once

#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdio>
#include <utility>

#include "QLangBytecode.h"
#include "QLangContext.h"
#include "QLangManifest.h"
#include "QLangParser.h"

namespace qlang {

// Compile-time context for the innermost loop being compiled. The CONTINUE /
// BREAK targets are baked into those instructions as operand `a`, so the VM
// only needs to load pc = i.a — no runtime loop stack required.
struct LoopCtx {
  int contAddr; // target for `continue` (loop test / increment)
  int breakAddr; // target for `break` (just past the loop)
};

// The output of compilation: the compiled functions, the one constant pool they
// all index (SPEC §bytecodes "unified constant table"), and the
// variable-to-register map the VM needs to sync the scope.
// One module top level a LINKED program runs before its entry module
// (SPEC §bytecodes "technical specifications" 3, external-modules plan v4
// Phase 3b / Q35): `funcIndex` is the module's `<init>` in the merged function
// array, `regBase` the offset its frame aliases inside the shared global
// register area, `registerCount` the width of that slice.
struct ModuleInit {
  int funcIndex = 0;
  int regBase = 0;
  int registerCount = 0;
  std::string module;
};

struct CompiledProgram {
  std::vector<Function> functions; // functions[0] == the implicit "main"
  // The program's single constant pool. Every `mov` kind-2 operand, every
  // `makefunc`/`makegen` operand and `emit`'s operand b indexes into this
  // vector; values are unique (ConstantPool folds duplicates).
  std::vector<QLangValue> constants;
  std::unordered_map<std::string, int> vars; // script name -> register index
  std::unordered_set<std::string> constVars; // names that are const (cannot be reassigned)
  // Native (C++) functions registered via QLangContext::setFunc(), indexed
  // by the negative opcode operand (index = -1 - opcode_b).
  std::vector<QLangContext::NativeFunc> nativeFuncs;
  int registerCount = 0;
  // Module identity (SPEC §bytecodes "keyword export"): the name other modules
  // import by. A module that declares `export const MODULE_NAME = "x"` carries
  // that name; otherwise it is the name the driver supplied (the source file
  // stem), defaulting to "main".
  std::string moduleName = "main";
  // The module's export and import tables (SPEC §bytecodes "export table
  // (lookup)", "import table (lookup)"). The export table is filled by
  // `export`; the import table is filled when the compiler binds a name from a
  // loaded `.ql` module (external-modules plan Phase 3b).
  std::vector<ExportEntry> exports;
  std::vector<ImportEntry> imports;
  // Link artifact, NOT part of the `.qlc` image: the module top levels to run
  // before functions[0], in dependency order. Empty for anything a compiler
  // produced; QLangLinker fills it for a merged program (Phase 3b, Q35).
  std::vector<ModuleInit> initSequence;
};

// Per-register breakdown of the MAIN program's register-allocator state,
// snapshotted at the end of compile(). Function bodies compile in
// saved/restored state, so this reflects the top-level allocator only.
// The sets are mutually exclusive by construction (a register is named,
// live, quarantined, deferred, pooled, or unallocated — never two of them);
// consumers classify with that priority.
struct RegisterReport {
  int registerCount = 0;         // m_nextReg at end of compile (high-water mark)
  int nextNamedReg = 0;          // dense named-allocation counter
  std::unordered_map<int, std::string> named; // reg -> var name (live named regs)
  std::set<int> namedRegs;       // live named registers
  std::vector<int> freeNamedRegs; // named regs freed by dead block scopes
  std::vector<int> freeRegs;      // dead temp pool
  std::vector<int> liveTmps;      // live temps (normally empty at end of compile)
  std::set<int> protectedTmps;    // live temps that must survive statement ends
  std::set<int> quarantined;      // kept out of the pool for live call-arg slots
  std::set<int> deferred;         // freed temps below the highest live temp
  std::vector<std::pair<int, int>> slotRanges; // active call-arg slot ranges
};

// A QLang literal the v6 encoding can carry as an inline right operand
// (plan decision D3): `Int` within int32 (forms i8/i16/i32), `Float`, `Bool`
// and `nil` (the zero-byte forms). The value tag has to survive the trip
// exactly — a 64-bit literal is a Qword/Long and a container is shared, so
// those stay in the constant pool.
struct LiteralImm {
  ImmForm form = ImmForm::Reserved;
  ImmPayload payload = ImmPayload::None;
  int64_t value = 0;
  double dbl = 0;
};

// One unencoded instruction in the per-function raw stream. Jump targets
// are instruction indices (not byte offsets); finish() resolves them to
// relative byte offsets and picks the narrowest jump width.
//
// The right operand is described the way the v6 modifier byte describes it
// (docs/spec/13-bytecodes.md `### bytecode encoding`): `kind` says what it is
// (register / constant pool / inline literal) and `form` how wide it is.
// `emit` derives both from the opcode identity, so a raw stream built from the
// existing call sites keeps its historical encoding; the immediate and pool
// variants added in plan step S5 set them explicitly.
struct RawInstr {
  Opcode op;
  int32_t a = 0, b = 0, c = 0;
  RightKind kind = RightKind::Register;
  // Immediate form of a kind-0 right operand. ImmForm::Reserved means "pick
  // the narrowest signed form that fits `imm`".
  ImmForm form = ImmForm::Reserved;
  // Which payload field carries the immediate (None when there is none).
  ImmPayload payload = ImmPayload::None;
  int64_t imm = 0;
  uint64_t immUnsigned = 0;
  double immFloat = 0;
  int32_t jumpTarget = -1; // instruction index; -1 = not a jump
  uint16_t line = 0, col = 0;
};

// Per-function raw instruction emitter: accumulates unencoded instructions
// and encodes them into the flat byte stream at finish() time (defined in
// QLangCompiler.cpp).
struct FnEmitter {
  std::vector<RawInstr> raw;

  size_t size() const { return raw.size(); }

  // Register-right emission (the historical shape). The right-operand kind
  // follows the opcode identity, because a base form carries no modifier byte
  // to state it in: call means an immediate, makefunc/makegen a pool index,
  // everything else a register.
  size_t emit(Opcode op, int32_t a = 0, int32_t b = 0, int32_t c = 0,
              int64_t imm = 0, uint16_t line = 0, uint16_t col = 0) {
    RawInstr r;
    r.op = op;
    r.a = a;
    r.b = b;
    r.c = c;
    r.imm = imm;
    r.line = line;
    r.col = col;
    r.kind = defaultRightKind(op);
    if (r.kind == RightKind::Immediate)
      r.payload = ImmPayload::Signed;
    raw.push_back(r);
    return raw.size() - 1;
  }

  // Emit an instruction whose right operand is described explicitly (plan
  // step S5): `right` carries the kind, the immediate form and the payload.
  // The kind may differ from the opcode's base form (a `mov` reading the
  // constant pool, an in-place ALU against a literal), which is exactly what
  // the modifier byte exists to announce.
  size_t emitRight(Opcode op, int32_t a, const RawInstr &right,
                   uint16_t line = 0, uint16_t col = 0) {
    RawInstr r;
    r.op = op;
    r.a = a;
    r.kind = right.kind;
    r.form = right.form;
    r.payload = right.payload;
    r.imm = right.imm;
    r.immUnsigned = right.immUnsigned;
    r.immFloat = right.immFloat;
    r.b = right.b; // register / constant-pool index for kinds 1 and 2
    r.line = line;
    r.col = col;
    raw.push_back(r);
    return raw.size() - 1;
  }

  size_t emitJump(Opcode op, int32_t target, int32_t a = 0,
                  uint16_t line = 0, uint16_t col = 0) {
    RawInstr r;
    r.op = op;
    r.a = a;
    r.jumpTarget = target;
    r.line = line;
    r.col = col;
    raw.push_back(r);
    return raw.size() - 1;
  }

  void patchJump(size_t idx, int32_t target) { raw[idx].jumpTarget = target; }

  // Stamp the source location of the most recently emitted instruction.
  // Used to attach a call site's line/col to a CALL that was emitted before
  // its arguments were compiled.
  void setLastLoc(uint16_t line, uint16_t col) {
    if (!raw.empty()) {
      raw.back().line = line;
      raw.back().col = col;
    }
  }

  // Encode all accumulated instructions into fn's flat byte stream, filling
  // fn.code, fn.instrOffsets and fn.loc. A function owns no constant pool:
  // pool operands were assigned indices in the program's ConstantPool while
  // the body was compiled (external-modules plan Phase 2).
  void finish(Function &fn);

  // Pick the modifier byte for a non-jump instruction from its operand widths
  // and immediate form, and report whether the instruction has to carry the
  // escape bit. Under the 2/2/4 layout a modifier of 0x00 is a *legal* escaped
  // modifier (kind immediate, form unsigned 1 byte), so the flag — not the
  // byte — is the only base-form signal. Public because encodeRawStream()
  // below — the shared encoder the linker also drives — needs it.
  static std::pair<uint8_t, bool> computeModifier(const RawInstr &r);

  // Effective immediate form of a raw instruction: the explicit one, or the
  // narrowest signed form that fits `imm`.
  static ImmForm immFormOf(const RawInstr &r);
};

// Encode a raw instruction stream into a function body: the jump-width
// fixpoint plus the byte encoding, i.e. everything FnEmitter::finish() does.
// Exposed so a stream that was not produced by an emitter — a `.qlc` image the
// linker decoded and rewrote (external-modules plan v4 Phase 3b) — can be
// encoded by the very same code, which is what keeps operand widths correct
// when a merged index turns out to need a wider operand.
void encodeRawStream(Function &fn, const std::vector<RawInstr> &raw);

// The inverse: decode a function body into raw instructions with jump targets
// restored as instruction indices. Throws QLangError on a malformed stream or
// a jump target that is not an instruction boundary.
void decodeRawStream(const Function &fn, std::vector<RawInstr> &raw);

// Walks a parsed AST and emits register-machine bytecode. Each script variable
// is assigned a stable register; control flow is lowered to jumps.
class QLangCompiler {
public:
  explicit QLangCompiler(std::vector<StmtPtr> program, QLangContext *scope = nullptr);

  // Register names that are const because they were injected from the host
  // (not declared with `const` in QLang source). The compiler treats them
  // the same as source-level const vars for guardNotConst purposes.
  void addExternalConsts(const std::unordered_set<std::string> &names);

  // Register names injected as writable host variables (not consts). Used
  // only to give `delete` a precise error (SPEC: delete of an external
  // variable is an error).
  void addExternalVars(const std::unordered_set<std::string> &names);

  // Register enums injected from the host (QLangContext::registerEnum). The
  // runtime table already lives in the hidden const var `__enum_<Name>`; this
  // only makes the name resolve at compile time: member access folds to the
  // stored value and the enum name resolves to the hidden var (the hidden
  // name itself is passed in addExternalConsts). Injected enums are global.
  void addExternalEnums(const std::unordered_map<std::string,
                                                    std::vector<std::pair<std::string, QLangValue>>> &enums);

  // Register the external types of a QLM1 manifest (portable-module-plan P4,
  // qlc `--qlmeta`). Validation input only — codegen is unchanged: type names
  // resolve through the host-injected-external-var path (the VM seeds the
  // TypeRef from ctx when the module runs on a host with the bindings), enum
  // members fold like injected enums, and member/method/ctor/signal uses are
  // checked against the manifest shape at compile time. Nothing is reserved for
  // a type the source never constructs: `T(...)` takes its slot at the call
  // (plan native-slot-portability P2).
  void addExternalTypes(const std::vector<ManifestType> &types);

  // Register names of C++ native functions. The compiler assigns them
  // negative indices so the VM can dispatch them at runtime.
  void addExternalFuncs(const std::unordered_set<std::string> &names);

  // Register module functions. Modules are NOT pre-bound: `import m` brings the
  // MODULE into scope and its entities are reached as `m.name` (SPEC §modules
  // "import statement"). This only records the names a registered module
  // exports, so a `.ql` export that collides with one is reported at import.
  void addModules(const std::vector<QLangContext::Module> &modules);
  // Name -> negative id for every name this compile was told about, whether or not
  // the source uses it (docs/SERIALIZATION.md §4.3). Ids are assigned on injection
  // because the map is also what makes a call resolvable: an unprefixed call with no
  // `def` falls back to it (`QLangCompiler.cpp`, "unknown function" otherwise), which
  // is how a host native becomes callable at all. A manifest type's constructor
  // is the exception: `addExternalTypes` declares the type, the call binds the
  // name (plan native-slot-portability P2). A driver that wires pointers for
  // the loaded program iterates this; a driver that writes an image uses
  // nativeSlotIds().
  const std::unordered_map<std::string, int> &nativeFuncIds() const { return m_nativeFuncIds; }
  // The map to write the `.qlc` native-slot table from (docs/SERIALIZATION.md §4.3):
  // only the ids the compile emitted a `CALL` for, ordered by name as the codec
  // wants. `loadProgram` resolves every entry of that table and refuses the image for
  // any name the context cannot serve, so an injected name nothing calls must stay
  // out of the file — otherwise an image inherits its build host's whole native table
  // as a load requirement, where §5.2 promises "a native the program calls"
  // (docs/plans/archive/native-slot-portability-plan.md P1). A C++ module function reached
  // through its qualifier is keyed `module.func`, so the image names the module it
  // wants; QLangContext::loadProgram re-resolves both spellings.
  std::map<std::string, int> nativeSlotIds() const;
  const std::vector<QLangContext::NativeFunc> &nativeFuncsResolved() const { return m_nativeFuncsResolved; }

  // One entry per name bound by an `import` statement (module provenance for
  // diagnostics/disassembly; `module` is the alias when `import x as y` was
  // used). `fromClass` marks a class method or prototype function: it is
  // compiled through the same path but is not an importable module function.
  // Populated by compile(); empty before it is called.
  struct ImportedModuleFunc {
    std::string module;
    std::string func;
    bool fromClass = false;
  };
  const std::vector<ImportedModuleFunc> &importedModuleFuncs() const {
    return m_importedModuleFuncs;
  }

  // Module identity of the compiled program (SPEC §bytecodes "keyword export").
  // The driver names the entry module after its source file; a program that
  // declares `export const MODULE_NAME = "x"` overrides it, and "main" is the
  // fallback when neither is available. Ignored in library mode.
  void setModuleName(const std::string &name) { m_moduleName = name; }

  // Library mode compiles a `.ql` module rather than a runnable script: the
  // top level becomes the module initializer `<init>` instead of `main`, and the
  // module must be namable — `export const MODULE_NAME = "<name>"`, or the
  // driver name above (a module neither names cannot be imported).
  // SPEC §bytecodes "keyword export".
  void setLibraryMode(bool on = true) { m_libraryMode = on; }

  CompiledProgram compile();

  // Register-allocator breakdown at the end of compile() (see
  // RegisterReport). Populated by compile(); empty before it is called.
  const RegisterReport &registerReport() const { return m_report; }

  // Gated diagnostic emit (mirrors QLangVM::trace). Prints printf-style
  // output to stderr only when the owning context's verbose level is >=
  // minLevel (see the TRACE_* levels in QLangContext). A compiler built
  // without a context emits nothing. Program output (print/println) is
  // unaffected.
  template <typename... Args>
  void trace(int minLevel, const char *fmt, Args &&...args) const {
    if (!m_scope || !m_scope->isAt(minLevel)) return;
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    if (n > 0) m_scope->output(OUT_TRACE, buf);
  }

private:
 // Engine warning diagnostic (SPEC §strict typing): routed through the
 // context's OUT_WARNING sink, never throws. The warning infrastructure is
 // this plus the sink — the strict typing level is the sole control (Q3/Q27).
 void warn(int line, const std::string &msg);
 // True when `name` is already bound in the innermost active scope: the
 // block map's back() inside block scopes, else this scope's named
 // registers (m_regs). Parameters and outer-scope names are deliberately
 // not matches (Q5) — nested shadowing stays silent.
 bool declaredInCurrentScope(const std::string &name) const;
 // SPEC §strict typing dup-decl diagnostic: warning at STRICT_DECLARED,
 // error at STRICT_ALL, silent (the default) at STRICT_OFF.
 void checkDupDecl(const StmtPtr &s);

 // Owning context, used to gate trace() output on its verbose level.
 // Null when the compiler is constructed standalone (trace() is then a no-op).
 QLangContext *m_scope = nullptr;

 int newRegister();
  int tmpRegister();
  // A temp register is dead: return it to the pool so a later tmpRegister()
  // reuses it (keeps registerCount flat as the program grows). The old value
  // is released when the register is next written; if never rewritten, frame
  // teardown releases it.
  void freeRegister(int r);
  // Clear a dead statement-level temp (releases its reference) and return
  // it to the pool. Sub-expression temps use freeRegister() (never cleared,
  // as before).
  void releaseTemp(int r);
  // Drop freed registers in [lo, hi) — those slots are reserved for call
  // args/template, so tmpRegister() must not hand one back out.
  void dropFreeRegs(int lo, int hi);
  // Reserve dest+1 .. dest+argCount as live call-arg slots (frame covers
  // them; freed registers can't be handed out into the range).
  void reserveArgSlots(int dest, int argCount);
  // Call-arg slot quarantine: while a call's slots (dest+1 .. dest+nSlots)
  // are live, registers in that range are kept out of the free pool so
  // tmpRegister() can't alias a slot with a live temp. Ranges nest (f(g(x)))
  // and are all restored at the statement boundary (endOfStatement).
  void beginArgSlots(int dest, int nSlots);
  void endArgSlots();
  // Highest live temp, or -1. The free pool must stay strictly above this:
  // a call's arg slots (dest+1..dest+n, dest = temp) must never alias a
  // live temp (loop-header temps survive across statements).
  int maxLiveTmp() const;
  void promoteDeferred();
  void returnToPool(int r);
  int declareVar(const std::string &name, bool blockScoped = false);
  int regOf(const std::string &name) const;
  int emitConst(const QLangValue &value);

  void emit(Opcode op, int a, int b = 0, int c = 0);
  // Right-operand forms of the unified v6 opcodes (plan step S5). The `i`/`c`
  // suffixed identities are no longer emitted: the modifier byte's kind field
  // says what the right operand is, so `mov`/`add`/`sub`/`cmp` cover the
  // immediate and the constant-pool spelling too (the in-place arity rule
  // drops the trailing `c` for a non-register ALU/compare right operand).
  // Separate names rather than overloads: a trailing `int` parameter would be
  // ambiguous with emit()'s defaulted b/c.
  void emitImmI(Opcode op, int a, int32_t imm);   // narrowest signed form
  void emitImmF(Opcode op, int a, double value);  // form 8
  void emitImmB(Opcode op, int a, bool value);    // forms 10 / 9
  void emitImmNil(Opcode op, int a);              // form 11, no pool entry
  void emitPool(Opcode op, int a, int poolIdx);   // kind 2 (constant pool)
  // Emit an already-selected inline immediate (see LiteralImm).
  void emitLiteralImm(Opcode op, int a, const LiteralImm &imm);
  // Emit `op a, <literal>`: an inline immediate when the literal's type
  // round-trips exactly through one (see LiteralImm), otherwise a pool read.
  void emitLiteral(Opcode op, int a, const QLangValue &value);
  // Branch-site fusion (SPEC §bytecodes: alternate conditional jump): emit
  // `cmp ra, rb` + the "jump if `rel` is false" conditional jump (JE/JNE/JG/
  // JGE/JL/JLE), leaving the jump target as 0 for the caller to patch.
  // Returns the index of the j* instruction. `rel` must be a comparison
  // opcode (EQ/NE/LT/GT/LE/GE).
  int emitCmpJumpFalse(Opcode rel, int ra, int rb);
  // Expression form for if/while conditions: a binary comparison
  // (==,!=,<,>,<=,>=) lowers to a compare that sets the frame flags plus the
  // "jump if the relation is FALSE" flag jump, and returns that jump's index
  // (target 0, patched by the caller). Both operands compile-time Int -> CMP;
  // anything else -> the EQF/LTF/GTF/LTEF/GTEF family, picking the *dual*
  // comparator (flagCompareIfFalse) so the same jump map fires exactly on the
  // false branch. Neither path builds a boolean temporary
  // (plan docs/plans/m3-flag-flow-plan.md D2). Returns -1 when `cond` is not a
  // comparison (truthiness, && / ||, calls, ...); the caller then lowers the
  // condition with `tst` + a flag jump (emitJumpIfFalse below).
  int emitCmpJumpFalseExpr(const ExprPtr &cond);
  // Bare-truthiness lowering (SPEC §bytecodes "OpCode tst"; plan
  // docs/plans/phase3-opcode-retirement-plan.md D1): `tst r[a]` puts
  // truth(r[a]) into the flags (eq set, the other two clear), so `je` takes
  // the true side and `jne` — which fires when no eq bit is set — the false
  // one. Both emit the compare and the flag jump with target 0 and return the
  // index of the *jump* (the `tst` sits at index - 1, which is where a loop
  // back-edge must point to re-evaluate the condition).
  int emitJumpIfTrue(int condReg);
  int emitJumpIfFalse(int condReg);
  void compileStmt(const StmtPtr &s,
                   std::vector<std::string> *captures = nullptr);
  // Per-statement compilers (one per StmtKind); compileStmt dispatches to
  // them after stamping the source position and snapshotting live temps.
  void compileDeclaration(const StmtPtr &s);
  void compileConst(const StmtPtr &s);
  void compileAssignment(const StmtPtr &s);
  void compileDelete(const StmtPtr &s);
  void compileFor(const StmtPtr &s);
  void compileForIn(const StmtPtr &s);
  void compileWhile(const StmtPtr &s);
  void compileIf(const StmtPtr &s);
  void compileGotoIf(const StmtPtr &s);
  void compileExprStmt(const StmtPtr &s);
  void compileDef(const StmtPtr &s);
  void compileArrow(const StmtPtr &s);
  void compileReturn(const StmtPtr &s);
  void compileYield(const StmtPtr &s);
  void compileEmit(const StmtPtr &s);
  void compileContinue(const StmtPtr &s);
  void compileBreak(const StmtPtr &s);
  void compileLabel(const StmtPtr &s);
  void compileGoto(const StmtPtr &s);
  void resolveImport(const StmtPtr &s);
  void compileClass(const StmtPtr &s);
  void compileEnum(const StmtPtr &s);
  void compileGlobal(const StmtPtr &s, std::vector<std::string> *captures);
  void compileSwitch(const StmtPtr &s);
  // Module exports (SPEC §bytecodes "keyword export"). collectExports() walks
  // the top-level statements BEFORE compilation, so a standalone `export NAME;`
  // can name a declaration that appears later in the file, and a second export
  // of the same name is rejected. finishExports() then resolves every recorded
  // name to its function index or register, once the program exists.
  void collectExports();
  void finishExports(CompiledProgram &out);
  // Rejects writing through a name exported as a function: the export table
  // holds its function index, so rebinding the name would silently detach the
  // export (SPEC §bytecodes "keyword export": functions "not modified").
  void guardNotExportedFunc(const std::string &name, const char *op) const;
  // Rejects writing through (or rebinding) a name imported from a loaded `.ql`
  // module: the defining module owns the cell, so a write here would either be
  // lost or silently detach the import (SPEC §bytecodes "technical
  // specifications" 4, plan Phase 3b "write through an import").
  void guardNotImported(const std::string &name, const char *op) const;
  // Rejects writing through a module qualifier (`m.x = 1`): the module owns
  // what it exports and the qualifier is not a value (SPEC §modules "import
  // statement").
  void guardNotModuleMember(const ExprPtr &target, const char *op) const;
  // Imported `.ql` names (plan Phase 3b). `qlModule` resolves an import source
  // name (honouring `import m as a` aliases) to a loaded image, or nullptr.
  // `bindQlImport` records one ImportEntry and returns its index, which is the
  // operand `getimport`/`callimport` name (plan Phase 3c).
  const LoadedModule *qlModule(const std::string &name) const;
  // True when `name` is an imported name whose definition yields, i.e. a call of
  // it produces a generator value. The import entry names the DEFINING module's
  // function record, which is where `isGenerator` lives, so this is the only way
  // the importing module can know what the call returns (plan Phase 3c).
  bool importedIsGenerator(const std::string &name) const;
  // The five import forms resolved against a loaded image's export table.
  void compileQlImport(const StmtPtr &s, const LoadedModule &mod);
  int bindQlImport(const LoadedModule &mod, const ExportEntry &exp,
                   const std::string &localName, const std::string &moduleAs,
                   const StmtPtr &s);
  // --- Module namespaces (SPEC §modules "import statement") ---
  // `import m` / `import m as a` binds the MODULE under the name the source
  // spells (the alias when there is one) -> the module's own name. Nothing a
  // module exports becomes visible by its bare name: only `from m import a` /
  // `from m import *` / `import m.a` bind flat names.
  std::unordered_map<std::string, std::string> m_moduleNs;
  // The registered C++ module named `name`, or nullptr.
  const QLangContext::Module *cppModule(const std::string &name) const;
  // True when `base` is a module qualifier rather than a value: a bare name an
  // `import` bound as a namespace, unread as a variable and not a class/enum.
  bool moduleQualified(const ExprPtr &base) const;
  // Resolves `q.name` on a module qualifier: a loaded `.ql` entity is the
  // import-table entry bound under that key, a C++ module function gets its
  // native slot. Returns the import index (>= 0) or the native slot id (< 0,
  // the `CALL` operand `b`); a name the module does not offer is a compile
  // error.
  int moduleMember(const std::string &q, const std::string &name, int line,
                   int col);
  // The name a call or a name reference resolves through: `q.name` when the
  // callee's base is a module qualifier, the bare name otherwise.
  std::string callKey(const ExprPtr &e) const;
  // Binds one native slot under `spelling` (the name the image stores and the
  // host re-resolves) and returns its negative id.
  int bindNative(const std::string &spelling,
                 const QLangContext::NativeFunc &fn);
  // `callimport` for one import-table entry, the args already staged at
  // `argBase`. Shared by a flat imported name and its qualified spelling
  // (plan Phase 3c).
  void emitImportCall(int importIdx, const std::string &name, int argBase,
                      int dest, int argCount, int line, int col);
  // Rejects an import graph that reaches the entry module again (SPEC
  // §bytecodes "technical specifications": import cycles are an error). Walks
  // the loaded images' import tables, so it stays correct once a library can
  // import a library (Phase 3c).
  void checkImportCycles() const;
  void compileExpr(const ExprPtr &e, int dest);
  // Lower a ternary block branch ({ ... return v ... }) to a closure call:
  // the block's `return` yields the ternary's value (SPEC §operators
  // "operator ?"). Free variables are captured by buildFunction.
  void compileTernaryBlock(const std::vector<StmtPtr> &block, int dest);
  Opcode binOp(const std::string &text) const;
  Opcode unOp(const std::string &text) const;

  // Compile a function body (a `def` body or an arrow-closure body) into its
  // own instruction stream and constant pool, append it to the program, and
  // return its function index. `captures` is the explicit capture list.
  int buildFunction(const std::string &name,
                    const std::vector<std::string> &params,
                    std::vector<std::string> captures,
                    const std::vector<StmtPtr> &body,
                    const std::vector<TypeAnnotation> &paramTypes = {},
                    const TypeAnnotation &returnType = {}, int line = 0);

  // Compile a statement inside a function body (used by buildFunction for the
  // body and by compileStmt for the main program). The current function's
  // register space, capture table, and instruction stream are the active ones.
  void compileBodyStmt(const StmtPtr &s);

  // Throw if `name` was declared with `const` (guards against reassignment).
  void guardNotConst(const std::string &name, const char *op) const;

  // Builtin name -> id resolution lives in the library table
  // (`findBuiltin`/`builtinId`, qlang/QLangBytecode.h): one table for the
  // compiler, the VM and the disassembler.
  // Fold a constant array/table element expression into a QLangValue.
  QLangValue evalConstExpr(const ExprPtr &e);
  // Infer the runtime type of an expression (Array/Table/... or Unknown).
  QLangValue::Type inferType(const ExprPtr &e) const;
  // Recompute m_knownReceivers from the module AST (plan P4 check 2): a name
  // is a known external receiver when its only write anywhere in the module
  // is `let p = T(...)` / `const p = T(...)` / `p = T(...)` with T a manifest
  // type that has a constructor.
  void collectKnownReceivers();
  // P4 argument-tag check shared by the method and constructor sites: an
  // mismatch is reported only when inferType answers (literals); dynamic
  // arguments stay silent for the runtime binding. `what` names the entry.
  void checkExtArgTags(const std::vector<QTypeTag> &tags,
                       const std::vector<ExprPtr> &args,
                       const std::string &what) const;
  // Update the tracked type of a variable from an assigned RHS.
  void updateVarType(const std::string &name, const ExprPtr &e);
  // Compile an index/member assignment target (a[i]=v, t.name=v).
  void compileAssignTarget(const ExprPtr &target, const Token &op,
                           const ExprPtr &value);
  // Register that already holds e's value and can be used directly as an
  // operand of a following instruction without emitting code (-1 when e
  // must be compiled into a register first). Only plain local/parameter
  // variables qualify (SPEC §bytecodes: encoding guidelines — avoid
  // temporary registers and register aliasing).
  int inPlaceReg(const ExprPtr &e) const;
  // Fold `x = x + k` / `x = x - k` into an in-place INC/DEC/ADD/SUB on
  // register r (SPEC §bytecodes: encoding guidelines — favor int
  // immediates over temporaries). Returns true when a fold was emitted.
  bool foldSelfArith(const ExprPtr &e, const std::string &name, int r);
  // Compile each call argument directly into its VM slot (slotBase + i):
  // simple operands (plain variables, literals) land in the slot without a
  // temp; everything else is routed through ONE shared scratch register so
  // an N-arg call needs at most one temp (SPEC §bytecodes: encoding
  // guidelines — universal scratch pad register).
  void compileArgsToSlots(const std::vector<ExprPtr> &args, int slotBase);
  // Recursively check if a statement (or any of its children) contains a yield.
  static bool containsYield(const StmtPtr &s);

  // Active per-function raw instruction emitter (defined above; finish()
  // and computeModifier() are implemented in QLangCompiler.cpp). The emitter
  // carries instructions only — the constant pool below outlives it, so a
  // nested `def` compiles against the same pool as its enclosing body.
  FnEmitter m_emitter;
  // The program's single constant pool (SPEC §bytecodes "unified constant
  // table"). Dedup spans every `def`, so a literal used in two functions
  // occupies one slot; compile() moves it into CompiledProgram::constants.
  ConstantPool m_pool;
  std::vector<StmtPtr> m_program;
  std::unordered_map<std::string, int> m_regs;
  // Block-scope name maps, innermost last. Only explicit `local`/`let`/`const`
  // land here (SPEC # block scope); bare assignments and loop vars stay in
  // m_regs so they cross block boundaries.
  std::vector<std::unordered_map<std::string, int>> m_blockScopes;
  // Current function's parameter names (saved/restored by buildFunction):
  // re-declaring a parameter with `local` is not a duplicate declaration
  // (Q5), so declaredInCurrentScope excludes them at function top level.
  std::unordered_set<std::string> m_paramNames;
  std::unordered_set<std::string> m_constVars; // names declared with `const`
  std::unordered_set<std::string> m_externalVars; // injected writable host vars
  // External types from a QLM1 manifest (addExternalTypes, plan P4). The
  // store owns the entries; m_extTypes maps name -> entry (deque: push_back
  // must not invalidate the stored pointers). m_knownReceivers is recomputed
  // per compile() by collectKnownReceivers(); the receiver checks apply at
  // top level only (m_inFnBody guards function/closure bodies, where a
  // same-named local or parameter could shadow), keeping the lenient
  // semantics everywhere else.
  std::deque<ManifestType> m_extTypeStore;
  std::unordered_map<std::string, const ManifestType *> m_extTypes;
  std::unordered_map<std::string, const ManifestType *> m_knownReceivers;
  bool m_inFnBody = false;
  // External C++ native functions: name -> negative opcode index.
  // Negative indices (-1, -2, ...) signal the VM to dispatch via the
  // context's native function registry.
  std::unordered_map<std::string, int> m_nativeFuncIds;
  // Ids emitted as a negative `CALL` operand, i.e. the natives the program calls.
  // What nativeSlotIds() filters by; the reservation map above stays the resolve-time
  // truth (plan native-slot-portability P1).
  std::unordered_set<int> m_usedNativeIds;
  // Resolved native function indices: index -> function (filled at compile time).
  std::vector<QLangContext::NativeFunc> m_nativeFuncsResolved;
  // Set of names that are module functions (imported from registered modules).
  std::unordered_set<std::string> m_moduleFuncNames;
  // Registered modules (populated by addModules, used by import compilation).
  std::vector<QLangContext::Module> m_moduleFuncs;
  // Module provenance of every `import`-bound name (see ImportedModuleFunc).
  std::vector<ImportedModuleFunc> m_importedModuleFuncs;
  // Compiled user functions (index 0 is reserved for the implicit "main"
  // function and is filled in at the end of compile()). `def` bodies and
  // arrow-closure bodies are appended here as they are compiled.
  std::vector<Function> m_functions;
  // User-defined functions: name -> function index (for `def` calls).
  std::unordered_map<std::string, int> m_userFuncs;
  // User-defined classes: name -> prototype-creation function index + method info.
  struct ClassRecord {
    int prototypeFunc = -1; // function index that creates and returns the prototype table
    std::vector<std::string> methodNames;
    std::unordered_map<std::string, int> methodFuncIndices;
    // Fixed arity (Phase 4): method name -> declared parameter count
    // (INCLUDING `self`). Call sites are checked against this at compile
    // time: a call must pass exactly arity-1 arguments.
    std::unordered_map<std::string, int> methodArities;
    // Closed field set (Phase 1): declared fields in declaration order, plus
    // name -> index. Emitted into the prototype's `__fields__` table and
    // copied onto each instance so GETFIELD/SETFIELD can enforce the set.
    std::vector<std::string> fieldNames;
    std::unordered_map<std::string, int> fieldIndices;
    // Const fields (Phase 2): the subset of declared fields that are `const`
    // (read-only). Emitted into the prototype's `__const_fields__` table and
    // copied onto each instance so SETFIELD can reject assignment to them.
    std::unordered_set<std::string> constFields;
    // Field type inference (Phase 5): field name -> type inferred from the
    // field's default value. Emitted into the prototype's `__field_types__`
    // table (name -> type-name string) and copied onto each instance so
    // SETFIELD can reject values that violate the inferred type. A field with
    // a non-constant (or `nil`) default has no entry -> no constraint.
    std::unordered_map<std::string, QLangValue::Type> fieldTypes;
    // Static members (new extension beyond v1): live in the class-level
    // `__statics__` table (shared by the prototype and every instance), NOT in
    // the instance field schema. Static methods are compiled with a hidden
    // first parameter (the `__statics__` table) so they need no self.
    std::unordered_set<std::string> staticFields;
    std::unordered_set<std::string> staticConstFields;
    std::unordered_map<std::string, int> staticMethodFuncIndices;
    // Static method arity = declared user param count (NO hidden anchor).
    std::unordered_map<std::string, int> staticMethodArities;
  };
  std::unordered_map<std::string, ClassRecord> m_classes;
  // User-defined enums: name -> member values in declaration order. The enum
  // itself is stored at runtime in a hidden variable `__enum_<Name>` holding
  // an immutable table (name -> value); member access folds to the constant.
  std::unordered_map<std::string, std::vector<std::pair<std::string, QLangValue>>> m_enums;
  // Names of enums declared at top level (visible everywhere, like global
  // consts). Used to keep global enums visible when entering a function body.
  std::unordered_set<std::string> m_globalEnums;
  // Names of host-injected enums (addExternalEnums). Unlike a declared enum
  // they have no hidden-var register inside a function body until captured;
  // member folding accepts them on the strength of this set alone.
  std::unordered_set<std::string> m_injectedEnumNames;
  // Variable names that currently hold an arrow-closure value.
  std::unordered_set<std::string> m_closureVars;
  // Active closure capture table: captured variable name -> refId (index into
  // the current function's explicit capture list).
  std::unordered_map<std::string, int> m_captureRegs;
  // Variable names declared at the top level (visible everywhere as globals).
  // Functions only see them when explicitly imported via `global`.
  std::unordered_set<std::string> m_globalVars;
  // Variables explicitly imported into function scope via `global`. These are
  // NOT in m_regs (so they compile as REFLOAD/REFSTORE) but ARE assignable.
  std::unordered_set<std::string> m_capturedVars;
  // Variable names known to hold a generator (arrow-closure containing yield).
  // Assignments/calls to these variables inside nested functions use CALLCLOS.
  std::unordered_set<std::string> m_generatorVars;
  // Function indices known to be generator functions (contain `yield`).
  // Persists across buildFunction calls so nested def calls can check without
  // accessing m_functions (which is restored after each buildFunction).
  std::unordered_set<int> m_generatorFuncIndices;
  // Maps generator funcIndex → constant pool index of the Closure constant
  // used by MAKEGEN to create a GeneratorValue.
  std::unordered_map<int, int> m_generatorConstIndices;
  // Caller's register mapping saved at function-entry for global keyword support.
  std::unordered_map<std::string, int> m_savedRegs;
  // Mapping from global var name to caller's register number (for capture spec).
  std::unordered_map<std::string, int> m_globalRegMap;
  // Runtime type tracking: lets the compiler choose the right opcode for
  // `+=` (array append vs. string/number add) and array/table access.
  std::unordered_map<std::string, QLangValue::Type> m_varTypes;
  std::vector<LoopCtx> m_loops; // enclosing-loop targets for continue/break
  std::vector<int> m_switches;  // enclosing-switch end targets for break
  std::vector<int> m_breakJmps; // instruction indices of unpatched break JMPs
  std::unordered_map<std::string, int> m_labels; // label name -> instruction index (per-scope)
  int m_nextReg = 0;
  // Dense allocation for named registers (params, locals, globals): they
  // occupy the low end of the frame, temporaries stay above them so a
  // call's arg slots (dest+1..dest+argCount, dest = temp) can't clobber a
  // live named variable. Sharing m_nextReg with temps made every `local` in
  // a recompiled loop body land above all prior temps, purging the pool.
  int m_nextNamedReg = 0;
  // Allocated named registers (params, locals, globals): the dense
  // allocator's skip loop must avoid these too, not just live temps —
  // without it, declarations after a live-temp run all land on the same
  // register.
  std::set<int> m_namedRegs;
  // Named registers freed by dead block scopes (loop/if bodies): reused by
  // later dense allocations so registerCount doesn't grow with the number of
  // loop bodies in the program.
  std::vector<int> m_freeNamedRegs;
  // Pop the innermost block scope, returning its named registers to
  // m_freeNamedRegs. Replaces raw m_blockScopes.pop_back() sites.
  void popBlockScope();
  // Pool of dead temp registers, reused by tmpRegister() before allocating a
  // new one. Scoped per function body (saved/cleared/restored in
  // buildFunction and class compilation) because register indices are
  // per-frame.
  std::vector<int> m_freeRegs;
  // All currently-live temp registers. Temps allocated during a statement are
  // freed at the statement boundary (endOfStatement) so registerCount is the
  // high-water mark of simultaneously-live temps. Temps that must survive
  // across statement boundaries (loop-header temps, re-read on the back
  // edge) are protected: at each statement start every live temp is marked
  // protected, so only temps allocated *within* the statement get freed.
  std::vector<int> m_liveTmps;
  std::set<int> m_protectedTmps;
  // Per-statement entry snapshot of m_protectedTmps. endOfStatement must
  // free against the set as of ITS OWN entry, not the current one — nested
  // statements restore their own (larger) sets, and using the current set
  // would keep loop-header temps alive forever.
  std::vector<std::set<int>> m_protectStack;
  // Active call-arg slot ranges (nested) and the registers quarantined from
  // the free pool on their behalf. Restored to the pool at the statement
  // boundary, skipping registers still inside an enclosing active range.
  std::vector<std::pair<int, int>> m_slotRanges;
  std::set<int> m_quarantined;
  // True if r falls inside any active call-arg slot range. Slot registers
  // hold live values, so no allocator path (pool, deferred) may hand them
  // out as temps while the range is active.
  bool inSlotRange(int r) const;
  // Freed temps BELOW the highest live temp (e.g. loop-header temps): kept
  // out of the pool so tmpRegister() stays above every live temp — a call's
  // arg slots (dest+1..) must never alias a live temp. Promoted to the pool
  // once the high temp dies (tmpRegister).
  std::set<int> m_deferred;
  void endOfStatement();
  // Snapshot the allocator state into m_report (called at the end of
  // compile(), before m_regs is moved out).
  void buildRegisterReport();
  // Source position of the statement currently being compiled; stamped into
  // every instruction by emit() so runtime errors report line/col (SPEC #
  // error reporting).
  int m_curLine = 0;
  int m_curCol = 0;
  // True when compiling inside a function body (global keyword is only valid there).
  bool m_inFunction = false;
  // Static-member compile context: static field + method names of the class
  // currently being compiled, so method bodies can resolve a bare static name
  // through the class-level `__statics__` table. Empty outside class bodies.
  std::unordered_set<std::string> m_classStaticNames;
  // True while compiling a static method body: the hidden anchor parameter
  // (register 0) IS the `__statics__` table, so static names TABGET directly
  // from reg 0. In an instance method body reg 0 is `self`; statics are
  // reached via `self.__statics__`.
  bool m_inStaticMethod = false;
  // Register holding the current method's `__statics__` table (emits the
  // `self.__statics__` TABGET for instance methods; reg 0 for static methods).
  int staticsTableReg();
  // End-of-compile allocator snapshot for registerReport().
  RegisterReport m_report;
  // --- Module identity and exports (SPEC §bytecodes "keyword export") ---
  // Library mode: the top level compiles as `<init>` and MODULE_NAME is
  // mandatory (see setLibraryMode).
  bool m_libraryMode = false;
  // Module name supplied by the driver (normally the source file stem); empty
  // falls back to "main".
  std::string m_moduleName;
  // Value of `export const MODULE_NAME` when the module declares one; it wins
  // over the driver-supplied name.
  std::string m_moduleNameFromSource;
  // The export table in source order, built by collectExports() with `ref`
  // still -1; finishExports() resolves the refs.
  std::vector<ExportEntry> m_exports;
  // Exported name -> kind: duplicate-export detection plus the guard that keeps
  // an exported function's name from being rebound.
  std::unordered_map<std::string, ExportKind> m_exportedKinds;
  // Top-level declarations seen by collectExports(): name -> the statement
  // that binds it, so a standalone `export NAME;` resolves forward references
  // and can report the kind it exports the name as.
  std::unordered_map<std::string, StmtPtr> m_globalDecls;
  // --- Imports from loaded `.ql` modules (external-modules plan v4 Phase 3b) ---
  // The import table in binding order (SPEC §bytecodes "import table
  // (lookup)"): `ref` is the DEFINING module's handle — its function index or
  // its cell — because the entry program cannot name either until the linker
  // rewrites it into a merged one.
  std::vector<ImportEntry> m_imports;
  // Local name -> index into m_imports, i.e. -> the `getimport`/`callimport`
  // operand. Kept across function bodies (like m_userFuncs): an imported name
  // is reachable from any depth, which is exactly what the two opcodes buy over
  // a register operand.
  std::unordered_map<std::string, int> m_qlImports;
  // `import m as a`: alias -> the loaded module's own name.
  std::unordered_map<std::string, std::string> m_qlAliases;
};

} // namespace qlang
