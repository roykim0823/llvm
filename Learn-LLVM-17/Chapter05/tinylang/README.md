# tinylang (Chapter 5) — deltas vs. Chapter 4

This README only documents what Chapter 5 **adds or changes** relative to
[Chapter 4](../../Chapter04/tinylang/README.md) (which in turn only
documents its deltas vs Chapter 3). Read those first for the frontend
pipeline (Lexer → Parser → Sema → AST) and the LLVM back-end wiring
(`CodeGenerator` / `CGModule` / `CGProcedure` + driver).

Chapter 5 is the **types chapter**: tinylang grows a real type system —
`ARRAY`, `RECORD`, `POINTER`, and `TYPE` aliases — and the AST/Sema/CodeGen
machinery to lower compound L-values via *designators with selectors*.

```
                   Chapter 5 additions on top of Ch04
                   ----------------------------------
   ┌──── new types ─────┐    ┌──── new expressions ───┐
   │ ARRAY [N] OF T     │    │ Designator             │
   │ RECORD … END       │    │  └── Selector chain    │
   │ POINTER TO T       │    │      ├── IndexSelector │
   │ TYPE A = B (alias) │    │      ├── FieldSelector │
   └────────────────────┘    │      └── DerefSelector │
                             └────────────────────────┘

   AST                                 CodeGen
   ===                                 =======
   Decl                                CGModule
    └── TypeDeclaration*                ├── TypeCache (compound types)
         ├── PervasiveTypeDeclaration   └── convertType dispatches on
         ├── AliasTypeDeclaration            the new hierarchy
         ├── ArrayTypeDeclaration       CGProcedure
         ├── PointerTypeDeclaration      ├── readVariable(LoadVal=false)
         └── RecordTypeDeclaration       ├── emitExpr walks selectors
                                         │     → GEP + load
                                         └── emitStmt(Assign) walks
                                               selectors → GEP + store
```

## Summary of changes

| Area | Status | What changed |
| --- | --- | --- |
| `include/tinylang/AST/ASTContext.h` | **new** | Bundles `SourceMgr` + filename; threaded into `CodeGenerator`/`CGModule`. |
| `include/tinylang/AST/AST.h` | modified | Adds `Field`/`FieldList`; turns `TypeDeclaration` abstract; adds `PervasiveTypeDeclaration`, `AliasTypeDeclaration`, `ArrayTypeDeclaration`, `PointerTypeDeclaration`, `RecordTypeDeclaration`; renames `Ident`→`Field`; adds `Selector` hierarchy (`IndexSelector`, `FieldSelector`, `DereferenceSelector`); replaces `VariableAccess` with `Designator`; narrows `AssignmentStatement::Var` from `Decl *` (Ch04) to `Designator *`. |
| `include/tinylang/Basic/TokenKinds.def` | modified | Adds punctuators `^` `[` `]` and keywords `ARRAY`, `OF`, `POINTER`, `RECORD`, `TO`, `TYPE`. |
| `include/tinylang/Basic/Diagnostic.def` | modified | Reverts Ch04's `{0}` placeholder on `err_not_yet_implemented` back to the fixed `"module imports are not yet implemented"` string. |
| `include/tinylang/Parser/Parser.h` + `lib/Parser/Parser.cpp` | modified | New productions: `parseTypeDeclaration`, `parseFieldList`, `parseField`, `parseSelectors`. `parseStatement`/`parseFactor` build a `Designator` and run selectors before consuming `:=` or descending into the expression. Skip-set updates threaded throughout. |
| `include/tinylang/Sema/Sema.h` + `lib/Sema/Sema.cpp` | modified | New actions: `actOnAliasTypeDeclaration`, `actOnArrayTypeDeclaration`, `actOnPointerTypeDeclaration`, `actOnRecordTypeDeclaration`, `actOnFieldDeclaration`, `actOnIndexSelector`, `actOnFieldSelector`, `actOnDereferenceSelector`. `actOnVariable` is renamed to `actOnDesignator`. `actOnAssignment` takes `Expr *` (a `Designator`) instead of `Decl *`. `INTEGER`/`BOOLEAN` are constructed as `PervasiveTypeDeclaration`. `actOnImport` reverts to the no-arg `err_not_yet_implemented`. |
| `include/tinylang/CodeGen/CodeGenerator.h` + `lib/CodeGen/CodeGenerator.cpp` | modified | Factory + ctor take an `ASTContext &` and forward it to `CGModule`. |
| `include/tinylang/CodeGen/CGModule.h` + `lib/CodeGen/CGModule.cpp` | modified | Holds `ASTContext &`; new `TypeCache` memoises `llvm::Type` for compound `TypeDeclaration`s; `convertType` dispatches on the new hierarchy (`Pervasive` → `i64`/`i1`; `Alias` → recurse; `Array` → `[N x T]`; `Record` → named struct). Adds a global `-g` cl::opt (hook for debug info; currently unused). |
| `include/tinylang/CodeGen/CGProcedure.h` + `lib/CodeGen/CGProcedure.cpp` | modified | `readVariable` gains `LoadVal` flag (return address vs loaded value); `mapType` drops `HonorReference` and wraps VAR formals to `ptr` via `PointerType::getUnqual`; new `createBasicBlock` helper; new `DIVariables` map (DI hook); WHILE reuses the current block as `while.cond` when empty; `emitExpr` walks `Designator` selectors and emits GEP+load chains; `emitStmt(AssignmentStatement)` does the matching GEP+store on the LHS. Plus an empty no-arg `run()` overload reserved for later. |
| `tools/driver/Driver.cpp` | modified | Back to `cl::list` of input files (per-file loop), constructs `ASTContext(SrcMgr, F)` and threads it into `CodeGenerator::create`, emits textual IR via `M->print()` directly (legacy `PassManager` is built only on the asm/object path), uses `OF_Text` for assembly. |
| `lib/Lexer/Lexer.cpp` | modified | Recognises `^`, `[`, `]`. |
| `test/CGModule_test.cpp` | modified | Threads `ASTContext` through the fixture; converts `INTEGER`/`BOOLEAN` to `PervasiveTypeDeclaration`; adds `ConvertArrayType`, `ConvertRecordType`, `ConvertAliasType`, `ConvertTypeIsCached`, plus a new mangler row for `Point::AssignX`. |
| `test/CGProcedure_test.cpp`, `test/CodeGenerator_test.cpp` | modified | Same fixture updates: `ASTContext` passed in, pervasive type construction. |
| `test/CMakeLists.txt` | modified | `CodeGenerator_test` now links `tinylangParser` + `tinylangLexer` + `tinylangBasic` in addition to the codegen/sema libs (so the test can spin up the full frontend). |
| `examples/` | replaced | `examples/Gcd.mod`, `examples/callgcd.c`, `examples/ir_example/` → removed. `examples/Point.mod` (a RECORD + procedure example) added. |
| `gen_ir.sh` | removed | The Ch04 IR-dump helper is no longer shipped. |
| `run.sh` | modified | Emits `Point.s` from `examples/Point.mod` (no C wrapper / link step). |
| `CMakeLists.txt` | modified | Includes `ChooseMSVCCRT` on non-Apple platforms. |
| `.clang-format`, `.clang-tidy` | removed | Tool configs no longer shipped in this chapter. |

## Designators: how a compound L-value flows through the pipeline

`p.X := a` is the canonical Ch05 example (see `examples/Point.mod`).

```
  Parser                 Sema                          CodeGen (CGProcedure)
  ------                 ----                          ---------------------
  parseStatement:        actOnDesignator(p)         →  emitStmt(AssignmentStatement)
    parse "p"          ─► makes Designator(p)           ├── selectors not empty →
    parseSelectors:                                     │   readVariable(p, LoadVal=false)
      ".X"   ──► actOnFieldSelector(D, "X")             │     // get the address of `p`
                  → appends FieldSelector(index=0,      ├── for each FieldSelector/IndexSelector:
                                          name="X")    │       push index into GEP idx list
    consume ":="                                        ├── CreateInBoundsGEP(base, idxList)
    parseExpression(E)                                  └── CreateStore(Val, gepResult)
    actOnAssignment(stmts, loc, D, E)
```

For an R-value (e.g. `a[i].x` on the right of `:=`), `emitExpr` does the
mirror image: GEP through the selectors and emit a load.

## CGModule::convertType — the new compound dispatch

```cpp
if (auto *P = dyn_cast<PervasiveTypeDeclaration>(Ty)) { … i64 / i1 …  }
if (auto *A = dyn_cast<AliasTypeDeclaration>(Ty))     return convertType(A->getType());
if (auto *A = dyn_cast<ArrayTypeDeclaration>(Ty))     return [Nums × elem];
if (auto *R = dyn_cast<RecordTypeDeclaration>(Ty))    return struct { fields… };
```

`TypeCache` is keyed on the tinylang `TypeDeclaration *` and stores the
result for `Alias`/`Array`/`Record` so a record/array shared across decls
maps to one `llvm::Type` (important for IR readability and for the
upcoming debug-info pass).

## ASTContext — what's it for?

```cpp
class ASTContext {
  llvm::SourceMgr &SrcMgr;
  StringRef Filename;
public:
  StringRef getFilename();
  llvm::SourceMgr &getSourceMgr();
};
```

A minimal container at this point: SourceMgr + filename. It exists so
`CGModule` can produce filename-aware debug info and so future
AST-allocation work (string interning, type uniquing) has a single
authoritative context.

## Driver delta

```diff
- // Ch04 driver: cl::opt<std::string> InputFile; — a single file.
- if (CodeGenerator *CG = CodeGenerator::create(Ctx, TM)) { … }
+ // Ch05 driver: cl::list<std::string> InputFiles; — loop.
+ for (const auto &F : InputFiles) {
+   …
+   auto ASTCtx = ASTContext(SrcMgr, F);
+   if (CodeGenerator *CG = CodeGenerator::create(Ctx, ASTCtx, TM)) { … }
+ }
```

And textual IR is now written directly:

```diff
- PM.add(createPrintModulePass(Out->os()));
+ M->print(Out->os(), nullptr);
```

so the legacy `PassManager` is only constructed when targeting asm or
object output.

## Tests added

The Ch04 test suites still apply (with the fixture updated to pass
`ASTContext` in). New Ch05-specific cases live in
`test/CGModule_test.cpp`:

| Test | What it checks |
| --- | --- |
| `ConvertArrayType` | `ARRAY [N] OF INTEGER` → `[N x i64]` with N elements / `i64` element type. |
| `ConvertRecordType` | `RECORD X, Y: INTEGER END` → named struct `{ i64, i64 }`. |
| `ConvertAliasType` | An alias of `INTEGER` resolves transparently to `i64`. |
| `ConvertTypeIsCached` | Calling `convertType` twice on the same record returns the same `llvm::Type *`. |
| `MangleNameParam{AssignXInPoint}` | `_t5Point7AssignX` — matches the symbol emitted from `examples/Point.mod`. |

`CGProcedure_test.cpp` and `CodeGenerator_test.cpp` get fixture updates
only (no new coverage in Ch05).

## Build & run

```bash
# from Chapter05/tinylang
./build.sh                # configures and builds via CMake/Ninja
./run.sh                  # build Point.s from examples/Point.mod
./test.sh                 # ctest in ./build/test
```

The driver flags are unchanged from Ch04:

```
  -o <file>            output filename
  -mtriple=<triple>    override the target triple
  --filetype=asm|obj   output format
  --emit-llvm          with --filetype=asm, write textual LLVM IR (.ll)
  -g                   (Ch05) parse a debug-info request — currently a no-op
                       hook; debug info lands in a later chapter
```

## Doxygen

Reuses the Ch04 setup; only `PROJECT_NAME` / `PROJECT_BRIEF` differ in
`docs/Doxyfile`, and `docs/.gitignore` excludes the generated `html/` /
`latex/`.

```bash
# from Chapter05/tinylang
doxygen docs/Doxyfile
open docs/html/index.html       # macOS
```

Only the **new or changed** code carries Doxygen comments in this
chapter — comments inherited verbatim from Ch04 are *not* re-annotated.
Specifically:

- `include/tinylang/AST/ASTContext.h` — `\file` + brief on the class.
- `include/tinylang/AST/AST.h` — Doxygen `\note` blocks on
  `Field`, the abstract `TypeDeclaration`, every concrete type subclass
  (`AliasTypeDeclaration`, `ArrayTypeDeclaration`,
  `PervasiveTypeDeclaration`, `PointerTypeDeclaration`,
  `RecordTypeDeclaration`), the `Selector` family, `Designator`, and
  `AssignmentStatement` (LHS narrowing to `Designator *`).
- `include/tinylang/Basic/Diagnostic.def` — comment on the
  `err_not_yet_implemented` revert.
- `include/tinylang/CodeGen/CodeGenerator.h`,
  `CGModule.h`, `CGProcedure.h` — `\file` summaries listing the Ch05
  deltas; per-member briefs on the new fields/methods only.
- `lib/CodeGen/CGModule.cpp`, `CGProcedure.cpp` — `\file` headers; brief
  on `convertType` describing the new RTTI dispatch.
- `lib/Sema/Sema.cpp` — `\file` header listing the new actions.
- `tools/driver/Driver.cpp` — `\file` brief describing the Ch05
  deltas (per-file loop, `ASTContext` plumbing, textual-IR path).
- `test/CGModule_test.cpp`, `test/CGProcedure_test.cpp`,
  `test/CodeGenerator_test.cpp` — `\file` summaries highlighting the
  fixture changes and (for CGModule) the new Ch05 test cases.

Files inherited unchanged from Ch04 (or changed only in whitespace) are
*not* re-annotated; their docs live in the Ch04 tree.
