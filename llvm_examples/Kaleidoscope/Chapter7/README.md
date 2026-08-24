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
(the FPM being extended).
Reference: [Chapter 7: Extending the Language — Mutable Variables](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl07.html).

The changes by layer: one token (`var`), one AST node (`VarExprAST`), one
production (`parseVarExpr`), and a codegen layer rewired around allocas —
including a rewritten for-loop and function-argument handling:

```
  namedValues : map<string, Value*>   ──becomes──▶  map<string, AllocaInst*>
                (an SSA value)                       (a stack ADDRESS)

  read  x  :  return namedValues[x]   ──becomes──▶  CreateLoad(slot)
  write x  :  (was impossible)              new:    '=' → CreateStore(val, slot)
  loop var :  PHI node                ──becomes──▶  alloca + load/store
  arguments:  the Argument itself     ──becomes──▶  alloca + initial store
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
  `CreateEntryBlockAlloca`'s whole job (next section).
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
wherever the *real* builder currently is:

**`Chapter7/src/codegen.cpp`**
```cpp
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                          llvm::StringRef VarName, IRGenContext &ctx) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*ctx.theContext), nullptr, VarName);
}
```

Changing `namedValues` to hold `AllocaInst*` (addresses, not values) is, by
itself, a pure refactoring — it restructures the codegen without changing
any observable behavior; nothing can *see* the mutability until `=` arrives
in the next section. With the map holding addresses, a
variable *reference* must explicitly load:

**`Chapter7/src/codegen.cpp`**
```cpp
llvm::Value *VariableExprAST::codegen(IRGenContext &ctx) {
  llvm::AllocaInst *A = ctx.namedValues[Name];
  if (!A)
    return logErrorV("Unknown variable name");
  return ctx.builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}
```

Function **arguments** get the same treatment in `FunctionAST::codegen()` —
an alloca per parameter, the incoming `Argument` stored into it — which is
what makes `x = x * 2` legal on a parameter. And the **for loop** is
rewritten from the Chapter 5 phi to an alloca: store the start value, and at
the bottom of the loop *reload* the variable before incrementing —
deliberately, because the body may have assigned to it:

**`Chapter7/src/codegen.cpp`**
```cpp
  // Reload, increment, and restore the alloca.  This handles the case where
  // the body of the loop mutates the variable.
  llvm::Value *CurVar =
      ctx.builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
  llvm::Value *NextVar = ctx.builder->CreateFAdd(CurVar, StepVal, "nextvar");
  ctx.builder->CreateStore(NextVar, Alloca);
```

The pipeline addition that makes all this free:

**`Chapter7/include/ir_gen_ctx.h`**
```cpp
        // Promote allocas to registers.
        theFPM->addPass(llvm::PromotePass());   // mem2reg, first in the pipeline
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
known, structured shape, so `IfExprAST::codegen` was left alone: making a
phi there is easier than making an alloca. Second, this IR is never visible
in the REPL, because the driver prints post-FPM IR only.

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
the `IRGenContext` constructor. That one line is the entire parser change:
to the parser, `=` is just another binary operator. The difference is in
codegen — unlike Chapter 6's user-defined operators it is handled
*internally*, never dispatched to a user function. Codegen special-cases it **before** the
normal children-first recursion, because the LHS of an assignment must not
be evaluated (it's a *location*, not a value — what C calls an lvalue):

**`Chapter7/src/codegen.cpp`**
```cpp
  if (Op == '=') {
    // Assignment requires the LHS to be an identifier.
    VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
    if (!LHSE)
      return logErrorV("destination of '=' must be a variable");

    llvm::Value *Val = RHS->codegen(ctx);          // evaluate the RHS only
    if (!Val)
      return nullptr;

    llvm::Value *Variable = ctx.namedValues[LHSE->getName()];
    if (!Variable)
      return logErrorV("Unknown variable name in Binary Expr");

    ctx.builder->CreateStore(Val, Variable);
    return Val;                                    // assignment yields the value (C-style)
  }
```

An upstream quirk kept deliberately: that `static_cast` is *not* a checked
cast (LLVM builds without RTTI, so upstream can't use `dynamic_cast`) — if
the LHS is anything other than a variable, e.g. `(a+b) = 3`, the cast is
undefined behavior and the `if (!LHSE)` guard never actually fires. Upstream
ships exactly this code with the same comment; a real frontend would add a
"kind" field to the AST (the planned v2 does).

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

`parseVarExpr()` collects `(name, optional-init)` pairs into a `VarExprAST`;
missing initializers default to `0.0` at codegen. Codegen evaluates each
initializer **before** pushing the name into scope — so
`var a = a in ...` initializes the new `a` from the *outer* `a` — then saves
the shadowed bindings and restores them after the body:

**`Chapter7/src/codegen.cpp`**
```cpp
    OldBindings.push_back(ctx.namedValues[VarName]);   // save (may be null)
    ctx.namedValues[VarName] = Alloca;                 // bind
  }
  llvm::Value *BodyVal = Body->codegen(ctx);           // body sees all vars
  ...
  for (int i = VarNames.size()-1; i>=0; --i)           // restore in REVERSE
    ctx.namedValues[VarNames[i].first] = OldBindings[i];
```

### Deviation from upstream: reverse-order restoration

The restore loop is this refactor's one deliberate fix. Upstream restores in
*forward* order, which mishandles the same name declared twice in one `var`
(legal, per the init-before-scope rule above). Trace `var a = 1, a = 2 in ...`:

| step | upstream (forward restore) | refactored (reverse restore) |
| --- | --- | --- |
| bind a₁ | OldBindings[0] = *outer* (null), map = Alloca₁ | same |
| bind a₂ | OldBindings[1] = Alloca₁, map = Alloca₂ | same |
| restore | i=0: map = null → i=1: map = **Alloca₁** (!) | i=1: map = Alloca₁ → i=0: map = **null** ✓ |

Forward order leaves the *inner* binding alive after the expression ends —
the scope leaks. Unwinding must mirror binding: last bound, first restored.
The `InternalShadowing` cases in `codegen_test.cpp` pin this down (including
a length-4 chain), and the long comment at the loop documents the same trace.

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

**New files**

| File | Purpose |
| --- | --- |
| `mem2reg_ex/example.ll`, `mem2reg_ex/run.sh` | The tutorial's standalone mem2reg demonstration. |
| `test/filecheck/mutablevars.k` | Asserts no alloca survives the FPM (see Tests). |

**Removed**: `mandel.txt`, `require.txt`, and `view_cfg/` (Chapter 6's demos;
`run.sh` no longer runs the Mandelbrot).

**Same filename, byte-identical** — safe to skip when reading:
`CMakeLists.txt`, `build.sh`, `include/log.h`, `src/log.cpp`, `src/main.cpp`,
`src/extern_d.cpp`, `test/filecheck/opt.k`, `test/filecheck/userops.k`,
`test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter7/include/lexer.h` / `src/lexer.cpp` — one new token,
`tok_var = -13`, and its keyword check.

`Chapter7/include/ast.h` — adds `VarExprAST{VarNames, Body}` (a vector of
name/initializer pairs); `VariableExprAST` gains `getName()` (the assignment
special case needs to ask the LHS for its name).

`Chapter7/include/ir_gen_ctx.h` — the symbol table changes type, `'='`
joins the precedence table, and `PromotePass` joins the pipeline:

```cpp
// Chapter6                                  // Chapter7
std::map<std::string, llvm::Value*>           std::map<std::string, llvm::AllocaInst*>
    namedValues;                                  namedValues;
binopPrecedence['<'] = 10; ...                binopPrecedence['='] = 2;  // new, lowest
                                              binopPrecedence['<'] = 10; ...
theFPM->addPass(InstCombinePass()); ...       theFPM->addPass(PromotePass());  // new, first
                                              theFPM->addPass(InstCombinePass()); ...
```

`Chapter7/include/parser.h` / `src/parser.cpp` — adds `parseVarExpr()`;
`parsePrimary()` gains `case tok_var`.

`Chapter7/src/codegen.cpp` — the chapter's substance:
`CreateEntryBlockAlloca` helper; `VariableExprAST` loads; `BinaryExprAST`
gains the `'='` special case; `ForExprAST` rewritten phi → alloca;
`FunctionAST` stores each argument into an alloca; `VarExprAST::codegen`
added (with the reverse-restore fix).

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
`x = y + 1` join the expression tests; JIT table gains var/assignment cases
(`var a in a + 1.0` → `1.0` — default init; `var a = 1.0 in (a = a + 2.0) + a`
→ `6.0`).

`Chapter7/test/codegen_test.cpp` — `VarExprTest` suite over `VarExprAST`
directly, including the `InternalShadowing` cases that motivated the
reverse-restore fix.

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
- **`codegen_test.cpp`** — `VarExprTest` exercises `VarExprAST::codegen`
  at the API level; its `InternalShadowing` / `InternalShadowingLength4`
  cases are the regression tests for the reverse-order restoration fix.
- **`parser_test.cpp`** — grammar for `var/in` and assignment; the JIT table
  proves the semantics numerically (default-init to 0.0, assignment yielding
  its value, read-after-write).
- **`test/filecheck/jit.k`** (carried from Chapter6) — the evaluate-loop and
  error-recovery checks, plus the REPL-level mutable-variable contract:
  `var a = 1.0 in (a = a + 2.0) + a;` → `Evaluated to 6.000000`.

```sh
ctest --test-dir build                   # everything
lit -v test/filecheck/mutablevars.k      # "no alloca survives" checks
./build/codegen_test                     # includes InternalShadowing
```
