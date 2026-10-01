# Chapter 7 — Mutable Variables and mem2reg

Kaleidoscope gets real **mutable variables**: an assignment operator
(`x = x + 1`), `var/in` declarations, and mutability for loop variables and
function parameters. The chapter is really about one big compiler idea:

**The SSA construction problem.** Chapters 1–6 built what is, so far, a
*functional* language — every value is defined exactly once, which makes
emitting SSA-form IR almost accidental (upstream: functional languages are
"too easy" to generate LLVM IR for). LLVM IR requires SSA form — there is no
"non-SSA mode" to postpone it — but *imperative* programs
mutate variables. In [Chapter5](../Chapter5/README.md) we could emit phi
nodes by hand only because the *only* merge points were the two structured
constructs (`if`, `for`), each with a known shape. Once the user can write
`x = ...` anywhere — inside either branch of an `if`, in a loop body — the
frontend would need real SSA construction: placing phis at exactly the right
merge blocks using *iterated dominance frontiers*, a classic but genuinely
tricky algorithm.

**The LLVM answer: don't build SSA in the frontend.** SSA constrains
*registers*, not *memory* — `load` and `store` are ordinary instructions
with no single-assignment rule. So the frontend takes the easy road:

- every mutable variable becomes a **stack slot** (`alloca double`),
- every read becomes a `load`, every write a `store`,
- and the **`mem2reg`** pass (run as `PromotePass`, now first in the FPM
  pipeline) promotes those stack slots back into SSA registers, inserting
  optimal phi nodes where needed — the dominance-frontier algorithm lives in
  LLVM, implemented once, instead of in every frontend.

```
   var a = 1 in         frontend emits            after mem2reg
   (if c then                                    (phis rebuilt,
      a = 2                %a = alloca double     allocas gone)
    else                   store 1.0, %a
      a = 3) : a           then:  store 2.0, %a    then / else
                           else:  store 3.0, %a       \   /
                           %r = load %a            %a.0 = phi [2.0,then],[3.0,else]
```

This is not a toy shortcut — it is *the* standard technique: clang emits
alloca/load/store for every local C/C++ variable and lets mem2reg do the
rest. The frontend stays simple; the promised SSA quality is the optimizer's
job. The `mem2reg_ex/` directory holds the tutorial's own demonstration
(`example.ll` — an alloca written in two branches) and a one-liner
`run.sh` (`llvm-as < example.ll | opt -passes=mem2reg | llvm-dis`) to watch
the promotion happen.

Prerequisites: [Chapter6](../Chapter6/README.md) (user-defined operators —
the demos use a sequencing `:` operator), [Chapter5](../Chapter5/README.md)
(the CFG/phi background this chapter replaces), [Chapter4](../Chapter4/README.md)
(the FPM being extended), and the codegen architecture — the
`CodeGenSession` facade over a private `Impl` — from
[Chapter3](../Chapter3/README.md).
Reference: [Chapter 7: Extending the Language — Mutable Variables](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl07.html).

The changes by layer: one token (`var`), one AST node (`VarExprAST`), one
production (`parseVarExpr`) plus one precedence-table row (`=`), and a
codegen `Impl` rewired around allocas — including a rewritten for-loop and
function-argument handling. Everything above the `Impl` is untouched: the
facade header, the driver and `main` are byte-identical to Chapter6's, and
so is the FPM hand-off — the pipeline just gains a pass.

```
  Impl::namedValues : map<string, Value*>   ──becomes──▶  map<string, AllocaInst*>
                      (an SSA value)                       (a stack ADDRESS)

  read  x  :  return namedValues[x]         ──becomes──▶  CreateLoad(slot)
  write x  :  (was impossible)                    new:    '=' → CreateStore(val, slot)
  loop var :  PHI node                      ──becomes──▶  alloca + load/store
  arguments:  the Argument itself           ──becomes──▶  alloca + initial store
  pipeline :  InstCombine, Reassociate, ... ──becomes──▶  PromotePass first, then the rest
```

(A precise file-by-file diff against Chapter6 is in
[File-by-file](#file-by-file-what-changed-from-chapter6) near the end.)

## The problem in C, and the memory loophole

Upstream grounds the problem in five lines of C:

```c
// upstream (LangImpl07)
int G, H;
int test(_Bool Condition) {
  int X;
  if (Condition)
    X = G;
  else
    X = H;
  return X;
}
```

`X` holds a different value depending on the path taken, so the SSA form of
`test` needs a phi node at the merge point to reconcile the two versions:

```
; upstream (LangImpl07) — the IR we want
define i32 @test(i1 %Condition) {
entry:
  br i1 %Condition, label %cond_true, label %cond_false

cond_true:
  %X.0 = load i32, i32* @G
  br label %cond_next

cond_false:
  %X.1 = load i32, i32* @H
  br label %cond_next

cond_next:
  %X.2 = phi i32 [ %X.1, %cond_false ], [ %X.0, %cond_true ]
  ret i32 %X.2
}
```

The chapter's question is: *who places that phi?* Three facts about how
LLVM treats **memory** open the loophole the frontend uses instead of
answering it:

- **A variable's name is its address.** Look at the globals above: `@G` is
  *defined* as an `i32`, but the type of `@G` itself is `i32*` — the name
  refers to the address of the storage. LLVM deliberately has no
  "address-of" operator because it never needs one; every memory access is
  an explicit `load` or `store` through such an address. Stack variables
  work exactly the same way, just declared with the `alloca` instruction
  instead of a global definition.
- **Memory is exempt from SSA.** Single assignment is required of register
  values and *not required (or even permitted)* of memory objects — the
  loads from `@G`/`@H` above access them directly, unrenamed and
  unversioned. (Some compiler systems version memory objects in their IR;
  LLVM instead keeps memory dataflow out of the IR and computes it on
  demand with analysis passes.)
- **`alloca` is fully general.** The address it returns is a first-class
  pointer: it can be stored elsewhere, passed to functions, offset with
  pointer arithmetic. That generality is also exactly what mem2reg must
  prove *wasn't* exploited before it may promote (below).

Rewriting `test` with an alloca eliminates every phi — store to `%X` in
both branches, a single load at the merge — and that rewritten function is
verbatim this repo's `mem2reg_ex/example.ll`: run `mem2reg_ex/run.sh` and
mem2reg hands back the phi version above. Hence the full recipe from the
intro — a stack slot per mutable variable, a load per read, a store per
write — plus its fourth, degenerate rule: *taking a variable's address*
just uses the stack address directly.

mem2reg promotes an alloca only under specific conditions — all easy for an
imperative language to satisfy, and each one visible in this chapter's code:

- It is **alloca-driven**: it promotes allocas it can prove safe and never
  touches global variables or heap allocations.
- The alloca must sit in the **entry block**, which guarantees it executes
  exactly once and keeps the analysis simple — providing that is
  `createEntryBlockAlloca`'s whole job (next section).
- Every use must be a **direct load or store**. An alloca whose address
  escapes — passed to a function, stored, or fed to "funny pointer
  arithmetic" — is not promoted.
- It must hold a **first-class value** (scalar, pointer, vector) with array
  size 1. Structs and arrays are out of scope — the more powerful `sroa`
  pass promotes many of those.

Should a frontend bother with this instead of building SSA itself?
Upstream's answer is an emphatic yes, three times over: the technique is
**proven and well-tested** (clang lowers every local C/C++ variable this
way, so LLVM's most common client shakes bugs out fast), **extremely fast**
(mem2reg fast-paths the common degenerate cases — variables used in a
single block, variables with one assignment point, heuristics that avoid
inserting unneeded phis), and **needed for debug info anyway** (debug
information attaches to a variable's exposed address, which this style
provides for free). If nothing else, it is the quickest way to get a
frontend up and running.

## The alloca infrastructure

Everything funnels through one helper. mem2reg only promotes allocas in the
**entry block** (that guarantees the slot dominates every use), so the helper
uses a second, temporary `IRBuilder` aimed at the top of the entry block —
wherever the *real* builder currently is. It is a method of the `Impl`,
next to `getFunction()`, because it needs the context for the type:

**`Chapter7/src/codegen.cpp`**
```cpp
  /// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
  /// the function.  This is used for mutable variables etc.
  llvm::AllocaInst *createEntryBlockAlloca(llvm::Function *TheFunction,
                                           llvm::StringRef VarName) {
    llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                           TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*theContext), nullptr, VarName);
  }
```

Changing the symbol table to hold `AllocaInst*` (addresses, not values) is,
by itself, a pure refactoring — it restructures the codegen without changing
any observable behavior; nothing can *see* the mutability until `=` arrives
in the next section:

**`Chapter7/src/codegen.cpp`**
```cpp
  // llvm::Value* -> llvm::AllocaInst* to use alloca
  std::map<std::string, llvm::AllocaInst *> namedValues;  // a.k.a. symbol table -- each entry is a stack slot's ADDRESS
```

With the map holding addresses, a variable *reference* must explicitly load:

**`Chapter7/src/codegen.cpp`**
```cpp
  llvm::Value *emit(VariableExprAST &var) {
    // Look this variable up in the function.
    llvm::AllocaInst *A = namedValues[var.getName()];  // llvm::Value* -> llvm::AllocaInst*
    if (!A)
      return logErrorV("Unknown variable name");

    // Load the value instead of simple Value return
    return builder->CreateLoad(A->getAllocatedType(), A, var.getName().c_str());
  }
```

Function **arguments** get the same treatment in `emitFunction()` — an
alloca per parameter, the incoming `Argument` stored into it — which is
what makes `x = x * 2` legal on a parameter:

**`Chapter7/src/codegen.cpp`**
```cpp
    // Record the function arguments in the namedValues map.
    namedValues.clear();
    for (auto &Arg : TheFunction->args()) {
      // Create an alloca for this variable.
      llvm::AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, Arg.getName());

      // Store the initial value into the alloca.
      builder->CreateStore(&Arg, Alloca);

      // Add arguments to variable symbol table.
      namedValues[std::string(Arg.getName())] = Alloca;
    }
```

And the **for loop** is rewritten from the Chapter 5 phi to an alloca: the
slot is created up front, the start value stored into it, and at the bottom
of the loop the variable is *reloaded* before incrementing — deliberately,
because the body may have assigned to it:

**`Chapter7/src/codegen.cpp`**
```cpp
    // Create an alloca for the variable in the entry block.
    llvm::AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, VarName);

    // Emit the start code first, without 'variable' in scope.
    llvm::Value *StartVal = emitExpr(*forExpr.getStart());
    if (!StartVal)
      return nullptr;

    // Store the value into the alloca.
    builder->CreateStore(StartVal, Alloca);
    ...
    // Reload, increment, and restore the alloca.  This handles the case where
    // the body of the loop mutates the variable.
    llvm::Value *CurVar =
        builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
    llvm::Value *NextVar = builder->CreateFAdd(CurVar, StepVal, "nextvar");
    builder->CreateStore(NextVar, Alloca);
```

Gone with the phi are `PreheaderBB`, `LoopEndBB` and the two `addIncoming`
calls: the loop no longer has to remember which block the backedge leaves
from, because mem2reg works that out. The shadow-and-restore of the loop
variable's binding is unchanged, only its type is now `AllocaInst*`.

The pipeline addition that makes all this free, in `initializeModule()`:

**`Chapter7/src/codegen.cpp`**
```cpp
    // Add transform passes.
    // Promote allocas to registers.
    theFPM->addPass(llvm::PromotePass());   // mem2reg, first in the pipeline
    // Do simple "peephole" optimizations and bit-twiddling optzns.
    theFPM->addPass(llvm::InstCombinePass());
```

### Before and after: mem2reg on `fib`

Upstream pauses to show the cost and the cure on plain recursive `fib` —
one variable, the parameter `x`. What the frontend now emits, before any
pass runs, is deliberately dumb:

```
; upstream (LangImpl07) — fib as emitted, before mem2reg (abridged)
define double @fib(double %x) {
entry:
  %x1 = alloca double
  store double %x, double* %x1
  %x2 = load double, double* %x1
  %cmptmp = fcmp ult double %x2, 3.000000e+00
  ...
else:
  %x3 = load double, double* %x1
  %subtmp = fsub double %x3, 1.000000e+00
  %calltmp = call double @fib(double %subtmp)
  %x4 = load double, double* %x1
  ...
ifcont:
  %iftmp = phi double [ 1.000000e+00, %then ], [ %addtmp, %else ]
  ret double %iftmp
```

One alloca in the entry block, the incoming argument stored once, and a
fresh reload at *every* reference. Two things to notice. First, the
if/then/else still produces its `iftmp` phi directly — that merge has a
known, structured shape, so `emit(IfExprAST&)` was left alone: making a
phi there is easier than making an alloca. Second, this IR is never visible
in the REPL, because the driver prints post-FPM IR only — and for the same
reason the unit tests never see an alloca either; they check what the
promoted function *computes*, and `mutablevars.k` checks that nothing is
left unpromoted.

After mem2reg alone, the alloca, the store, and all the reloads collapse
into direct uses of `%x`. This `fib` is a *trivial* promotion — the
variable is never reassigned, so not even a phi is needed — and upstream
shows it precisely "to calm your tension" about the blatant stack traffic
the frontend now emits. After the *rest* of the pipeline, this chapter's
binary prints:

```
ready> def fib(x) if (x < 3) then 1 else fib(x-1)+fib(x-2);
Read function definition:
define double @fib(double %x) {
entry:
  %cmptmp = fcmp ult double %x, 3.000000e+00
  br i1 %cmptmp, label %ifcont, label %else

else:                                             ; preds = %entry
  %subtmp = fadd double %x, -1.000000e+00
  %calltmp = call double @fib(double %subtmp)
  %subtmp5 = fadd double %x, -2.000000e+00
  %calltmp6 = call double @fib(double %subtmp5)
  %addtmp = fadd double %calltmp, %calltmp6
  br label %ifcont

ifcont:                                           ; preds = %entry, %else
  %iftmp = phi double [ %addtmp, %else ], [ 1.000000e+00, %entry ]
  ret double %iftmp
}
```

— zero memory traffic, and the cleanup passes kept simplifying:
instcombine rewrote `x - 1` as `x + -1.0` and folded Chapter 5's
`<`-comparison boolean dance (`uitofp` + compare-against-zero) straight
into the branch. (Upstream's older output shows a different final shape —
simplifycfg cloning the `ret` into the `else` block to delete the phi; the
exact shape shifts with the LLVM version, the promise doesn't.)

## Assignment: `=` as a binary operator

`'='` enters the precedence table at 2 — lower than everything, so
`x = y + 1` parses as `x = (y + 1)` — installed alongside the built-ins in
the `Parser` constructor:

**`Chapter7/include/parser.h`**
```cpp
    Parser(Lexer& lexer) : lexer(lexer) {
        binopPrecedence['='] = 2;  // Mutable Variable: assignment binds loosest
        binopPrecedence['<'] = 10;
        binopPrecedence['+'] = 20;
        binopPrecedence['-'] = 20;
        binopPrecedence['*'] = 40;
    }
```

That one line is the entire parser change for assignment: to the parser,
`=` is just another binary operator (the tree test `AssignmentBindsLoosest`
checks the shape). The difference is in codegen — unlike Chapter 6's
user-defined operators it is handled *internally*, never dispatched to a
user function. `emit(BinaryExprAST&)` special-cases it **before** the
normal children-first recursion, because the LHS of an assignment must not
be evaluated (it's a *location*, not a value — what C calls an lvalue):

**`Chapter7/src/codegen.cpp`**
```cpp
    // Special case '=' because we don't want to emit the LHS as an expression.
    if (bin.getOp() == '=') {
      // Assignment requires the LHS to be an identifier.
      auto *LHSE = llvm::dyn_cast<VariableExprAST>(bin.getLHS());
      if (!LHSE)
        return logErrorV("destination of '=' must be a variable");

      // Codegen the RHS.
      llvm::Value *Val = emitExpr(*bin.getRHS());          // evaluate the RHS only
      if (!Val)
        return nullptr;

      // Look up the name.
      llvm::Value *Variable = namedValues[LHSE->getName()];
      if (!Variable)
        return logErrorV("Unknown variable name in Binary Expr");

      builder->CreateStore(Val, Variable);
      return Val;                                    // assignment yields the value (C-style)
    }
```

The `dyn_cast<VariableExprAST>` is the AST's kind tag from Chapter 2 doing
its job: it returns null for anything that is not a variable reference, so
`(a+b) = 3` is reported as an error. Upstream cannot do this — see
[Deviations from upstream](#deviations-from-upstream).

Returning `Val` makes assignment an expression, so it supports chained
assignments (`X = (Y = Z)`) and composes
with the sequencing operator: `printd(x) : x = 4 : printd(x)`.

## `var/in`: Declarations with scope

With `=` alone, the only mutable things are function parameters and loop
induction variables — upstream: "redefining those only goes so far :)". And
declaring new variables is useful regardless of whether you mutate them.
Hence `var/in`:

```
varexpr ::= 'var' identifier ('=' expression)?
                  (',' identifier ('=' expression)?)* 'in' expression
```

`parseVarExpr()` collects `(name, optional-init)` pairs into a `VarExprAST`
tagged `Expr_VarDecl`; a missing initializer is a null pointer in the pair
and defaults to `0.0` at codegen:

**`Chapter7/include/ast.h`**
```cpp
/// VarExprAST - Expression class for var/in
class VarExprAST : public ExprAST {
  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
  std::unique_ptr<ExprAST> Body;

public:
  VarExprAST(
      std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
      std::unique_ptr<ExprAST> Body)
      : ExprAST(Expr_VarDecl), VarNames(std::move(VarNames)), Body(std::move(Body)) {}

  /// (name, initializer) pairs in declaration order; an initializer may be null.
  const std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> &getVarNames() const {
    return VarNames;
  }
  ExprAST *getBody() const { return Body.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_VarDecl; }
};
```

Codegen evaluates each initializer **before** pushing the name into scope —
so `var a = a in ...` initializes the new `a` from the *outer* `a` — then
saves the shadowed bindings and restores them after the body:

**`Chapter7/src/codegen.cpp`**
```cpp
  llvm::Value *emit(VarExprAST &varExpr) {
    std::vector<llvm::AllocaInst *> OldBindings;

    llvm::Function *TheFunction = builder->GetInsertBlock()->getParent();

    // Register all variables and emit their initializer.
    const auto &VarNames = varExpr.getVarNames();
    for (unsigned i = 0, e = VarNames.size(); i != e; ++i) {
      const std::string &VarName = VarNames[i].first;
      ExprAST *Init = VarNames[i].second.get();

      // Emit the initializer before adding the variable to scope, this prevents
      // the initializer from referencing the variable itself, and permits stuff
      // like this:
      //  var a = 1 in
      //    var a = a in ...   # refers to outer 'a'.
      llvm::Value *InitVal;
      if (Init) {
        InitVal = emitExpr(*Init);
        if (!InitVal)
          return nullptr;
      } else { // If not specified, use 0.0.
        InitVal = llvm::ConstantFP::get(*theContext, llvm::APFloat(0.0));
      }

      llvm::AllocaInst *Alloca = createEntryBlockAlloca(TheFunction, VarName);
      builder->CreateStore(InitVal, Alloca);

      // Remember the old variable binding so that we can restore the binding when
      // we unrecurse.
      OldBindings.push_back(namedValues[VarName]);      // save (may be null)

      // Remember this binding.
      namedValues[VarName] = Alloca;                    // bind
    }

    // Codegen the body, now that all vars are in scope.
    llvm::Value *BodyVal = emitExpr(*varExpr.getBody());
    if (!BodyVal)
      return nullptr;

    // Pop all our variables from scope -- in REVERSE order of binding ...
    for (int i = VarNames.size() - 1; i >= 0; --i)
      namedValues[VarNames[i].first] = OldBindings[i];

    // Return the body computation.
    return BodyVal;
  }
```

The restore loop runs backwards on purpose; upstream's runs forwards, and
the difference is observable — see
[Deviations from upstream](#deviations-from-upstream).

## Seeing it work

`cmd.txt` ends with the chapter's classic: iterative Fibonacci with three
mutable variables. The frontend emitted an alloca + load/store soup; the
printed (post-FPM) IR shows what mem2reg made of it — three phis with
generated names, zero allocas, zero memory traffic:

```
ready> def fibi(x)
  var a = 1, b = 1, c in
  (for i = 3, i < x in
    c = a + b :
    a = b :
    b = c) :
  b;
Read function definition:
define double @fibi(double %x) {
entry:
  br label %loop

loop:                                             ; preds = %loop, %entry
  %a.0 = phi double [ 1.000000e+00, %entry ], [ %b.0, %loop ]
  %b.0 = phi double [ 1.000000e+00, %entry ], [ %addtmp, %loop ]
  %i.0 = phi double [ 3.000000e+00, %entry ], [ %nextvar, %loop ]
  %addtmp = fadd double %a.0, %b.0
  ...
```

This is exactly the IR a hand-written SSA frontend would have produced —
but nobody had to write the hard part.

## File-by-file: What changed from Chapter6

The exact split (established with `diff -rq ../Chapter6 .`):

**New files**

| File | Purpose |
| --- | --- |
| `mem2reg_ex/example.ll`, `mem2reg_ex/run.sh` | The tutorial's standalone mem2reg demonstration. |
| `test/filecheck/mutablevars.k` | Asserts no alloca survives the FPM (see Tests). |

**Removed**: `mandel.txt`, `require.txt`, and `view_cfg/` (Chapter 6's demos;
`run.sh` no longer runs the Mandelbrot).

**Same filename, byte-identical** — safe to skip when reading:
`include/codegen.h`, `include/driver.h`, `src/driver.cpp`, `src/main.cpp`
(the facade, the driver and `main` know nothing about mutability),
`CMakeLists.txt`, `build.sh`, `include/log.h`, `src/log.cpp`,
`src/extern_d.cpp`, `test/filecheck/opt.k`, `test/filecheck/userops.k`,
`test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter7/include/lexer.h` / `src/lexer.cpp` — one new token,
`tok_var = -13`, and its keyword check.

`Chapter7/include/ast.h` — one new kind tag, `Expr_VarDecl`, and
`VarExprAST{VarNames, Body}` (a vector of name/initializer pairs) with
`getVarNames()`/`getBody()`. No other node changes: `VariableExprAST`
already had the `getName()` the assignment case needs.

`Chapter7/include/parser.h` / `src/parser.cpp` — `'='` joins the
precedence table in the constructor; `parseVarExpr()` added;
`parsePrimary()` gains `case tok_var`:

```cpp
// Chapter6: Parser constructor            // Chapter7
Parser(...) {                               Parser(...) {
    binopPrecedence['<'] = 10;                  binopPrecedence['='] = 2;  // new, lowest
    ...                                         binopPrecedence['<'] = 10;
}                                               ...
                                            }
```

`Chapter7/src/codegen.cpp` — the chapter's substance, all inside the
`Impl`: the symbol table changes type; `PromotePass` opens the pipeline;
`createEntryBlockAlloca` helper; `emit(VariableExprAST&)` loads;
`emit(BinaryExprAST&)` gains the `'='` special case; `emit(ForExprAST&)`
rewritten phi → alloca; `emitFunction()` stores each argument into an
alloca; `emit(VarExprAST&)` added (with the reverse-restore fix):

```cpp
// Chapter6                                  // Chapter7
std::map<std::string, llvm::Value*>           std::map<std::string, llvm::AllocaInst*>
    namedValues;                                  namedValues;
theFPM->addPass(InstCombinePass()); ...       theFPM->addPass(PromotePass());  // new, first
                                              theFPM->addPass(InstCombinePass()); ...
```

`Chapter7/cmd.txt` / `run.sh` — new demo: recursive `fib` vs iterative
`fibi`, plus `test(x)` showing parameter assignment observed through
`printd`.

`Chapter7/test/filecheck/controlflow.k` — updated for the alloca era: the
loop-phi check can no longer name `%i` (mem2reg generates `%i.0`-style
names), so it matches `{{%.*}} = phi double` and adds `CHECK-NOT: alloca`;
the `testNested` case is kept (all its block labels survive mem2reg).

`Chapter7/test/filecheck/jit.k` — carried forward from Chapter6 and
extended with the chapter's evaluate-loop check:
`var a = 1.0 in (a = a + 2.0) + a;` → `Evaluated to 6.000000`.

`Chapter7/test/lexer_test.cpp` — `var` must lex as `tok_var`.

`Chapter7/test/parser_test.cpp` — `ParseVarExprTest` (multiple declarations,
optional initializers, missing `in`/identifier fail); `x = 1` and
`x = y + 1` join the expression tests; two tree-shape tests
(`AssignmentBindsLoosest`, `VarTree` — including a null initializer).

`Chapter7/test/codegen_test.cpp` — ten mutable-variable cases through the
facade, all reading the promoted function's return value: initializer and
read, default `0.0`, assignment yielding its value, parameter mutation, the
two error paths (`(x+1) = 2`, unknown target), an initializer seeing the
outer binding, and the scope-restoration cases (double shadow, a chain of
four, a new name unbound afterwards).

`Chapter7/test/jit_test.cpp` — the full-pipeline table gains eight
var/assignment rows, including `DoubleShadowScope` (`12.0`) and two loops
whose bodies mutate a variable — one of them the loop variable itself,
which the reload-before-increment makes visible.

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter
departs from it in two places, both in the codegen of this chapter's new
constructs and both concerning *incorrect* programs: every valid input
prints exactly what upstream prints.

### `(a+b) = 3` is an error, not undefined behavior

Upstream's assignment case downcasts the LHS without checking:

```cpp
// upstream (LangImpl07)
  if (Op == '=') {
    // Assignment requires the LHS to be an identifier.
    // This assume we're building without RTTI because LLVM builds that way by
    // default.  If you build LLVM with RTTI this can be changed to a
    // dynamic_cast for automatic error checking.
    VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
    if (!LHSE)
      return LogErrorV("destination of '=' must be a variable");
```

A `static_cast` from `ExprAST*` to `VariableExprAST*` never yields null, so
the guard below it can never fire: for `(a+b) = 3` the cast is undefined
behavior — in practice `getName()` reads garbage out of a `BinaryExprAST`
— and the error message upstream wrote is unreachable. Upstream's own
comment says what it would rather do. Here:

**`Chapter7/src/codegen.cpp`**
```cpp
      auto *LHSE = llvm::dyn_cast<VariableExprAST>(bin.getLHS());
      if (!LHSE)
        return logErrorV("destination of '=' must be a variable");
```

Why: this repo's AST has carried a kind tag since Chapter 2 precisely so
that consumers can ask a node what it is. `llvm::dyn_cast<>` uses
`VariableExprAST::classof()` and returns null on a mismatch, which makes
the check upstream wanted a one-word change — the same `dyn_cast` idiom
LLVM's own code uses everywhere, RTTI or not. `codegen_test.cpp`'s
`AssignmentToNonVariableIsAnError` pins the message.

|                                | upstream                                       | here                                       |
| ------------------------------ | ---------------------------------------------- | ------------------------------------------ |
| `x = 3` (x a variable)         | store                                          | store                                      |
| `(a+b) = 3`                    | UB (`static_cast` to the wrong type)           | `Error: destination of '=' must be a variable` |
| `y = 3` (y unknown)            | `Error: Unknown variable name`                 | `Error: Unknown variable name in Binary Expr` |

(The last row is a message-text difference inherited from the earlier
version of this chapter and kept for output stability; upstream's text is
the shorter one.)

### Reverse-order scope restoration in `var/in`

Upstream pops the bindings of a `var` expression in the order they were
pushed:

```cpp
// upstream (LangImpl07)
  // Pop all our variables from scope.
  for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
    NamedValues[VarNames[i].first] = OldBindings[i];
```

Here the loop runs backwards:

**`Chapter7/src/codegen.cpp`**
```cpp
    for (int i = VarNames.size() - 1; i >= 0; --i)
      namedValues[VarNames[i].first] = OldBindings[i];
```

Why: forward order mishandles the same name declared twice in one `var`
(legal, per the init-before-scope rule). Trace `var a = 1, a = 2 in ...`
with an outer `a` bound to `Outer`:

| step    | upstream (forward restore)                            | here (reverse restore)                              |
| ------- | ----------------------------------------------------- | --------------------------------------------------- |
| bind a₁ | OldBindings[0] = Outer, map = Alloca₁                 | same                                                |
| bind a₂ | OldBindings[1] = Alloca₁, map = Alloca₂               | same                                                |
| restore | i=0: map = Outer → i=1: map = **Alloca₁** (!)         | i=1: map = Alloca₁ → i=0: map = **Outer** ✓         |

Forward order leaves the *inner* binding alive after the expression ends —
the scope leaks, and a trailing reference to `a` reads the inner `1`
instead of the outer value. Unwinding must mirror binding: last bound,
first restored. `VarScopeRestoresArgumentAfterDoubleShadow` (and a
length-4 chain) in `codegen_test.cpp` pin this at the API level — with the
forward loop, `def f(a) (var a = 1, a = 2 in a) + a` folds to the constant
`3.0`; with the reverse loop it is `2.0 + %a` — and `jit_test.cpp`'s
`DoubleShadowScope` evaluates `var a = 10 in (var a = 1, a = 2 in a) + a`
to `12.0`.

## Build and run

Same recipe (`./build.sh`, then `./run.sh` or pipe `cmd.txt`). Beyond the
`fibi` IR above, the session shows parameter mutation working end-to-end:

```
ready> def test(x)
  printd(x) :
  x = 4 :
  printd(x);
Read function definition:            (IR elided — post-FPM, so the alloca is
...                                   already promoted away: two printd calls,
                                      the second with the constant 4.0)

ready> test(123);
Read top-level expression:
...
123.000000
4.000000
Evaluated to 0.000000

ready> fibi(10);
Read top-level expression:
...
Evaluated to 55.000000
```

(Matching the recursive `fib(10)` — and note it relies on Chapter5's loop
quirk: the end condition is tested after the body, so `for i = 3, i < x`
runs the body for `i = 3..10`, exactly the eight updates needed.)

## Tests

Same two-scheme setup — rationale and lit mechanics in the [top-level
README](../README.md#testing-the-two-schemes). What Chapter 7 adds:

- **`test/filecheck/mutablevars.k`** — the mem2reg contract, stated
  negatively: the REPL prints post-FPM IR, so **no `alloca` may survive**
  (`CHECK-NOT: alloca` bracketing the expected computation). One test mutates
  a `var`, the other mutates a *parameter* — both must promote cleanly.
- **`codegen_test.cpp`** — mutable-variable semantics observed through the
  facade. Because the session always runs mem2reg, the tests cannot see
  allocas or stores; they ask what the promoted function returns and
  whether it still reads the *argument* after a `var` scope closes. The
  double-shadow and chain-of-four cases are the regression tests for the
  reverse-order restoration; `AssignmentToNonVariableIsAnError` pins the
  `dyn_cast` check.
- **`parser_test.cpp`** — grammar for `var/in` and assignment, and the two
  tree shapes: `=` at the root of `x = y + 1`, and a `VarExprAST` whose
  second declaration has no initializer.
- **`jit_test.cpp`** — the semantics numerically: default-init to 0.0,
  assignment yielding its value, read-after-write, initializer-from-outer,
  the double-shadow scope (`12.0`), and loop bodies that mutate an outer
  variable and the loop variable itself.
- **`test/filecheck/jit.k`** (carried from Chapter6) — the evaluate-loop and
  error-recovery checks, plus the REPL-level mutable-variable contract:
  `var a = 1.0 in (a = a + 2.0) + a;` → `Evaluated to 6.000000`.

```sh
ctest --test-dir build                   # everything
lit -v test/filecheck/mutablevars.k      # "no alloca survives" checks
./build/codegen_test                     # includes the scope-restoration cases
./build/jit_test                         # includes DoubleShadowScope
```
