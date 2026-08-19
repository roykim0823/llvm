# Chapter 5 — Control Flow: if/then/else and for

Until now Kaleidoscope could only compute straight-line expressions — every
function was one `entry` block running top to bottom. This chapter adds
`if/then/else` and `for` loops, which means functions finally have real
**control flow**. Two general concepts first:

**The control-flow graph (CFG).** A function's body is a graph of **basic
blocks** — straight-line instruction runs that end in exactly one
*terminator* (`br`, `ret`). Branching constructs don't emit "jump
instructions into the middle of code"; they create new blocks and wire them
with edges. An `if/then/else` becomes the classic four-block **diamond**; a
loop becomes a block that branches back to itself:

```
        if x then A else B                 for i = ... in body

              entry                            entry (preheader)
           br i1 %cond                             │
            /       \                              ▼
          then:    else:                   ┌──▶ loop:  i = phi ...
           A         B                     │      body, i += step
            \       /                      └── br cond, loop, after
             ifcont:                               │
      phi [A, then], [B, else]                     ▼
                                               afterloop:
```

**SSA and the phi node.** LLVM IR is in *static single assignment* form —
each value is assigned by exactly one instruction, forever. That collides
with control flow: after the diamond merges, "the result" is the then-value
on one path and the else-value on the other, and SSA forbids assigning the
same name twice. The **`phi` instruction** is SSA's answer: placed at a merge
block, it yields a different value *depending on which predecessor block
control arrived from* — `phi [A, then], [B, else]`. The same trick gives a
loop its induction variable: `i = phi [start, entry], [i+step, loop]` — one
instruction, two incoming edges, no mutation. Building optimal SSA form is
normally a compiler's job (dominance frontiers etc.); this chapter sidesteps
all of it because Kaleidoscope has no mutable variables — the *only* merge
points are these two constructs, so the codegen just emits the phis directly.
(Chapter 7 shows the lazier industry-standard route: stack slots + the
`mem2reg` pass.)

Everything else — one type, JIT, optimizer — is as in
[Chapter4](../Chapter4/README.md); parser/AST basics in
[Chapter2](../Chapter2/README.md), codegen basics in
[Chapter3](../Chapter3/README.md).
Reference: [Chapter 5: Extending the Language — Control Flow](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl05.html).

The additions cut through every layer, and each one is textbook mechanics by
now — new keywords in the lexer, new AST nodes, new productions in
`parsePrimary()`, new `codegen()` overrides:

```
  lexer.h/.cpp     ast.h            parser.h/.cpp        codegen.cpp
  tok_if  ──────▶  IfExprAST  ◀──── parseIfExpr()   ────▶ IfExprAST::codegen
  tok_then                            (primary case)        4-block diamond + phi
  tok_else
  tok_for ──────▶  ForExprAST ◀──── parseForExpr()  ────▶ ForExprAST::codegen
  tok_in                              (primary case)        loop block + phi + backedge
```

Both constructs are **expressions**, not statements — an `if` yields the
value of the taken branch (like C's `?:`), a `for` always yields `0.0`.
That's what lets them slot into the existing grammar as just two more
`primary` cases, with zero changes to `parseExpression`/`parseBinOpRHS`.

(A precise file-by-file diff against Chapter4 is in
[File-by-file](#file-by-file-what-changed-from-chapter4) near the end.)

## Lexer and parser: five keywords, two productions

The lexer additions are five identifier checks (`if`, `then`, `else`, `for`,
`in` → `tok_if` ... `tok_in`); the grammar grows two productions, dispatched
from `parsePrimary()`:

```
ifexpr  ::= 'if' expression 'then' expression 'else' expression
forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
```

`parseIfExpr()` is the standard eat-check-recurse pattern (eat `if`, parse
condition, expect `then`, parse, expect `else`, parse). `parseForExpr()` has
one wrinkle — the step clause is optional, encoded as a null `Step` in the
AST and defaulted to `1.0` at codegen time:

**`Chapter5/src/parser.cpp`**
```cpp
  // The step value is optional.
  std::unique_ptr<ExprAST> Step;
  if (curTok == ',') {
    getNextToken();
    Step = parseExpression();
    if (!Step)
      return nullptr;
  }
```

The two AST nodes hold exactly their subtrees: `IfExprAST{Cond, Then, Else}`
and `ForExprAST{VarName, Start, End, Step, Body}` — note the loop *variable
name* is data here; it doesn't exist as a variable anywhere until codegen
makes a phi for it.

## if/then/else codegen: the diamond and the phi

**`Chapter5/src/codegen.cpp`**
```cpp
llvm::Value *IfExprAST::codegen(IRGenContext &ctx) {
  llvm::Value *CondV = Cond->codegen(ctx);
  if (!CondV)
    return nullptr;

  // Convert condition to a bool by comparing non-equal to 0.0.
  CondV = ctx.builder->CreateFCmpONE(
      CondV, llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(0.0)), "ifcond");

  llvm::Function *TheFunction = ctx.builder->GetInsertBlock()->getParent();

  // Create blocks for the then and else cases.  Insert the 'then' block at the
  // end of the function.
  llvm::BasicBlock *ThenBB = llvm::BasicBlock::Create(*ctx.theContext, "then", TheFunction);
  llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*ctx.theContext, "else");
  llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*ctx.theContext, "ifcont");

  ctx.builder->CreateCondBr(CondV, ThenBB, ElseBB);

  // Emit then value.
  ctx.builder->SetInsertPoint(ThenBB);
  llvm::Value *ThenV = Then->codegen(ctx);
  if (!ThenV)
    return nullptr;
  ctx.builder->CreateBr(MergeBB);
  // Codegen of 'Then' can change the current block, update ThenBB for the PHI.
  ThenBB = ctx.builder->GetInsertBlock();

  // Emit else block.
  TheFunction->insert(TheFunction->end(), ElseBB);
  ctx.builder->SetInsertPoint(ElseBB);
  llvm::Value *ElseV = Else->codegen(ctx);
  if (!ElseV)
    return nullptr;
  ctx.builder->CreateBr(MergeBB);
  ElseBB = ctx.builder->GetInsertBlock();

  // Emit merge block.
  TheFunction->insert(TheFunction->end(), MergeBB);
  ctx.builder->SetInsertPoint(MergeBB);
  llvm::PHINode *PN = ctx.builder->CreatePHI(llvm::Type::getDoubleTy(*ctx.theContext), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}
```

The mechanics worth slowing down for:

- **Truthiness**: Kaleidoscope has no bool, so the condition is `fcmp one`
  ("ordered, not-equal") against `0.0` — any non-zero value is true.
- **The builder is a cursor, and recursion moves it.** After
  `Then->codegen(ctx)` returns, the "current block" may be some deep
  `ifcont` block of a *nested* if — not the `then` block we created. Two
  consequences: the `br` to the merge block is emitted wherever the branch
  chain actually ended, and `ThenBB = ctx.builder->GetInsertBlock()`
  re-reads the block for the phi's incoming edge. Forgetting that re-read is
  the classic bug this chapter warns about — a phi's incoming block must be
  the *actual* predecessor.
- **Deferred block insertion.** `ElseBB` and `MergeBB` are created without a
  parent and appended (`TheFunction->insert`) only when their turn comes, so
  the blocks appear in the function in roughly source order even after
  nested codegen appended blocks of its own.
- **The phi is the return value.** The whole construct evaluates to `PN` —
  from the outside, an if-expression is just one more `Value*`.

One thing you will notice in the REPL: this driver prints **post-optimizer**
IR (the FPM from Chapter 4 runs on every function), so the printed IR rarely
matches the raw codegen above. Two real examples: InstCombine flips
`fcmp one x, 0.0; br then, else` into `fcmp ueq x, 0.0; br else, then`
(same meaning, canonical form), and when both arms are side-effect-free
constants, SimplifyCFG deletes the diamond entirely and replaces the phi with
a single `select` instruction.

## for codegen: the loop phi and variable shadowing

The target shape, from the comment in the source:

```
        ...preheader...                  loop:
        start = startexpr                  i = phi [start, preheader], [nextvar, loop]
        br loop                            body...
                                           nextvar = fadd i, step
                                           endcond = (endexpr) != 0.0
        afterloop:                         br endcond, loop, afterloop
        (always yields 0.0)
```

**`Chapter5/src/codegen.cpp`** (abridged — the shape-defining lines)
```cpp
  llvm::Value *StartVal = Start->codegen(ctx);        // emitted in the preheader,
                                                      // 'i' not in scope yet
  llvm::BasicBlock *PreheaderBB = ctx.builder->GetInsertBlock();
  llvm::BasicBlock *LoopBB = llvm::BasicBlock::Create(*ctx.theContext, "loop", TheFunction);
  ctx.builder->CreateBr(LoopBB);                      // explicit fall-through edge
  ctx.builder->SetInsertPoint(LoopBB);

  llvm::PHINode *Variable =
      ctx.builder->CreatePHI(llvm::Type::getDoubleTy(*ctx.theContext), 2, VarName);
  Variable->addIncoming(StartVal, PreheaderBB);       // first entry now...

  llvm::Value *OldVal = ctx.namedValues[VarName];     // shadow outer 'i', if any
  ctx.namedValues[VarName] = Variable;

  if (!Body->codegen(ctx))                            // value ignored, errors not
    return nullptr;
  ...
  llvm::Value *NextVar = ctx.builder->CreateFAdd(Variable, StepVal, "nextvar");
  EndCond = ctx.builder->CreateFCmpONE(EndCond, ..., "loopcond");

  llvm::BasicBlock *LoopEndBB = ctx.builder->GetInsertBlock();
  ctx.builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  ctx.builder->SetInsertPoint(AfterBB);

  Variable->addIncoming(NextVar, LoopEndBB);          // ...backedge entry last

  if (OldVal)                                         // un-shadow
    ctx.namedValues[VarName] = OldVal;
  else
    ctx.namedValues.erase(VarName);

  // for expr always returns 0.0.
  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*ctx.theContext));
```

The notable moves:

- **The phi is built in two steps.** Its preheader entry is added
  immediately, but the backedge entry (`NextVar` from `LoopEndBB`) can only
  be added *after* the body is generated — the value and the predecessor
  block don't exist yet. Temporarily-incomplete phis are fine as long as
  they're complete before verification.
- **The loop variable is scoped by shadow-and-restore.** `namedValues` is a
  flat map, so nesting is handled manually: save whatever `VarName` mapped to
  (an argument, or an outer loop's variable), overwrite it with the phi, and
  restore/erase on the way out. This two-line idiom is the entire "scoping
  story" until Chapter 7 brings real mutable variables.
- **Execution order is: body → step → end-condition.** The condition is
  tested *after* the body, against the **pre-increment** variable — do-while
  semantics. Two visible consequences: the body always runs at least once
  (even if the start value fails the condition), and the loop re-enters one
  more time after the last passing test — trace `for i = 1, i < n`: the body
  runs with `i = n-1`, the check `n-1 < n` passes, so the body runs again
  with `i = n` before exiting. The body executes for `i = 1 .. n`, n times;
  concretely, `printstar(10)` below prints **10** stars.
- **A for-expression always evaluates to `0.0`** (`Constant::getNullValue`) —
  it is used for its side effects.

## Real output at last: `putchard` and `extern_d.cpp`

Loops are only fun if the body can *do* something, so this chapter adds the
tutorial's mini "standard library" — ordinary C++ functions the JIT'd code
can call through the Chapter 4 dlsym mechanism:

**`Chapter5/src/extern_d.cpp`**
```cpp
/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}
```

`extern "C"` disables C++ name mangling so the JIT's process-wide symbol
search finds the names as written. The build wrinkle (see the comment in
`CMakeLists.txt`): these functions are *never referenced* by the compiler's
own code, and a static archive drops unreferenced members at link time — so
`extern_d.cpp` is listed directly in each executable's sources instead of
going into `toy_core`, keeping the symbols in the process image where dlsym
can see them.

The directory also gains a tiny helper, `view_cfg/`: `run.sh` pipes an `.ll`
file through `opt -passes=view-cfg` to render a function's CFG with Graphviz
— handy for actually *seeing* the diamond and the loop backedge.

## File-by-file: what changed from Chapter4

**New files**

| File | Purpose |
| --- | --- |
| `src/extern_d.cpp` | `putchard`/`printd` — host functions callable via `extern`. |
| `test/filecheck/controlflow.k` | CFG-shape checks for if/for (see Tests). |
| `view_cfg/run.sh`, `view_cfg/t.ll` | Graphviz CFG viewer helper. |

**Same filename, byte-identical** — safe to skip when reading:
`include/ir_gen_ctx.h` (nothing about optimizer/JIT changes this chapter),
`include/log.h`, `src/log.cpp`, `src/main.cpp`, `build.sh`,
`test/filecheck/opt.k`, `test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter5/include/lexer.h` / `src/lexer.cpp` — five new token enumerators
(`tok_if = -6` ... `tok_in = -10`) and the matching keyword checks in
`gettok()`:

```cpp
// Chapter4                       // Chapter5
if (identifierStr == "def") ...    if (identifierStr == "def") ...
if (identifierStr == "extern")...  if (identifierStr == "extern") ...
return tok_identifier;             if (identifierStr == "if")   return tok_if;
                                   if (identifierStr == "then") return tok_then;
                                   if (identifierStr == "else") return tok_else;
                                   if (identifierStr == "for")  return tok_for;
                                   if (identifierStr == "in")   return tok_in;
                                   return tok_identifier;
```

`Chapter5/include/ast.h` — adds `IfExprAST` (Cond/Then/Else) and
`ForExprAST` (VarName/Start/End/Step/Body); existing nodes untouched.

`Chapter5/include/parser.h` / `src/parser.cpp` — adds `parseIfExpr()` and
`parseForExpr()`, and `parsePrimary()` grows two cases:

```cpp
// Chapter4                       // Chapter5
switch (curTok) {                  switch (curTok) {
case tok_identifier: ...           case tok_identifier: ...
case tok_number:     ...           case tok_number:     ...
case '(':            ...           case '(':            ...
default: return logError(...);     case tok_if:  return parseIfExpr();
}                                  case tok_for: return parseForExpr();
                                   default: return logError(...);
                                   }
```

`Chapter5/src/codegen.cpp` — adds the two `codegen()` overrides explained
above; everything else identical.

`Chapter5/CMakeLists.txt` — `src/extern_d.cpp` added to the `toy`,
`parser_test`, and `codegen_test` executables (deliberately *not* to the
`toy_core` archive; see above).

`Chapter5/test/lexer_test.cpp` — five new keyword cases (`"if"` → `tok_if`,
...); the rest is a whitespace re-indent of the existing table.

`Chapter5/test/parser_test.cpp` — two new parameterized suites:
`ParseIfExprTest` (missing `then`/`else` must fail, nested if passes) and
`ParseForExprTest` (step optional, missing `=`/`in` must fail).

`Chapter5/test/codegen_test.cpp` — no new C++ checks; a comment redirects
IR-structure verification for control flow to `controlflow.k` (block labels
and canonicalized predicates pattern-match better in FileCheck than as
substring loops).

`Chapter5/cmd.txt` — new demo: an if-diamond over opaque externs, and
`printstar` printing `*` via `putchard`.

`Chapter5/test/filecheck/jit.k` — carried forward from Chapter4 (the
evaluate-loop and error-recovery contract still holds) and extended with two
control-flow checks: an if-expression evaluates to its taken arm, and a
for-expression always evaluates to `0.0`.

## Build and run

Same recipe (`./build.sh`). A session with `./build/toy < cmd.txt` — note
the driver prints post-optimizer IR:

```
ready> extern foo();
Read extern:
declare double @foo()

ready> extern bar();
Read extern:
declare double @bar()

ready> def baz(x) if x then foo() else bar();
Read function definition:
define double @baz(double %x) {
entry:
  %ifcond = fcmp ueq double %x, 0.000000e+00   ; InstCombine flipped one→ueq
  br i1 %ifcond, label %else, label %then      ; ...and swapped the targets

then:                                             ; preds = %entry
  %calltmp = call double @foo()
  br label %ifcont

else:                                             ; preds = %entry
  %calltmp1 = call double @bar()
  br label %ifcont

ifcont:                                           ; preds = %else, %then
  %iftmp = phi double [ %calltmp, %then ], [ %calltmp1, %else ]
  ret double %iftmp
}

ready> extern putchard(char);
Read extern:
declare double @putchard(double)

ready> def printstar(n)
  for i = 1, i < n, 1.0 in
    putchard(42);  # ascii 42 = '*'
Read function definition:
define double @printstar(double %n) {
entry:
  br label %loop

loop:                                             ; preds = %loop, %entry
  %i = phi double [ 1.000000e+00, %entry ], [ %nextvar, %loop ]
  %calltmp = call double @putchard(double 4.200000e+01)
  %nextvar = fadd double %i, 1.000000e+00
  %cmptmp = fcmp ult double %i, %n
  br i1 %cmptmp, label %loop, label %afterloop

afterloop:                                        ; preds = %loop
  ret double 0.000000e+00
}

ready> printstar(10)
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @printstar(double 1.000000e+01)
  ret double %calltmp
}

**********Evaluated to 0.000000
```

Ten stars: the end condition runs after the body with the pre-increment `i`,
so the body executes for `i = 1..10`.

## Tests

Same two-scheme setup — rationale and lit mechanics in the [top-level
README](../README.md#testing-the-two-schemes). What Chapter 5 adds:

- **`lexer_test.cpp`**: the five new keywords must lex as their tokens, not
  as identifiers.
- **`parser_test.cpp`**: `ParseIfExprTest` / `ParseForExprTest` pin the
  grammar — including that the step clause is optional and that a missing
  `then`, `else`, `=`, or `in` fails cleanly.
- **`test/filecheck/controlflow.k`**: the CFG-shape checks, written around
  the fact that the driver prints **post-FPM** IR:
  - `testIfCfg` uses calls to *opaque externs* in both arms — possible side
    effects force SimplifyCFG to keep the diamond, so the test can assert
    `then:`/`else:`/`ifcont:` labels and the `phi`.
  - `testIfSelect` does the opposite: with constant arms the diamond *must*
    collapse — `CHECK-NOT: br i1` plus `CHECK: select` asserts the
    if-conversion happened.
  - `testFor` checks the loop phi (`%i = phi double`) — in this chapter the
    induction variable really is a phi (explicit SSA, pre-`mem2reg`).
  - `testNested` puts a diamond inside a loop body and requires both
    structures' labels to survive.
- **`test/filecheck/jit.k`** (carried from Chapter4): the evaluate-loop and
  error-recovery checks still apply, plus `if 1 then 42 else 0;` →
  `Evaluated to 42.000000` and a for-expression's constant `0.0` result.

```sh
ctest --test-dir build                    # everything
lit -v test/filecheck/controlflow.k       # the CFG-shape checks
./build/lexer_test                        # includes the new keyword cases
```
