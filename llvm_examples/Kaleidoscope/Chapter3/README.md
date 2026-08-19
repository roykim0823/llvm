# Chapter 3 — Code Generation to LLVM IR

This chapter turns the AST from [Chapter2](../Chapter2/README.md) into
**LLVM IR**. Two general compiler concepts first:

**Code generation** is the phase that translates the structured program the
frontend built (the AST) into a lower-level language. The frontend stages
were about *understanding* the input — characters into tokens, tokens into a
tree; code generation is where the compiler starts *producing* output, by
walking that tree and emitting an instruction sequence for each node. A
classical compiler would emit assembly for one CPU directly, but that couples
every language to every target. Modern compilers instead emit an
**intermediate representation (IR)**: a language that is lower-level than any
source language (instructions, not expressions) but still
machine-independent. The payoff is the classic M×N argument — M frontends and
N backends need only M+N translators instead of M×N — plus a single place to
write every optimization, once, for all languages and all targets.

**LLVM IR** is that middle language for the entire LLVM ecosystem — the same
IR clang emits for C++ and rustc emits for Rust, and the input every LLVM
optimization pass and backend consumes. It looks like assembly for an
idealized machine: strongly **typed** (`double`, `i1`, function types),
with infinitely many named registers (`%a`, `%addtmp`) in **SSA form**
(*static single assignment* — every value is assigned by exactly one
instruction, which is what makes dataflow trivial for the optimizer to
trace). It is organized the way the last two chapters would suggest: a
**module** contains functions, a **function** contains basic blocks, a
**basic block** contains instructions. The same IR exists in three equivalent
forms: the in-memory C++ objects this chapter builds (`llvm::Function`,
`llvm::Value`), human-readable text (`.ll`, what the driver prints), and
compact on-disk bitcode (`.bc`).

From IR onward, LLVM's optimizer and backends do the heavy lifting for free —
which is why "compile Kaleidoscope" really means "get to the IR."
Kaleidoscope needs only a tiny slice of it: everything is `double`, so
functions have type `double (double, ...)` and the instructions used are
`fadd`/`fsub`/`fmul`, `fcmp`+`uitofp`, `call`, and `ret`. Two pieces do the
translation:

- **Code generation as a tree walk** — each AST node knows how to emit the IR
  for itself: expressions return the `llvm::Value*` holding their result,
  after recursively emitting their children. There is no separate "codegen
  pass" object — the AST *is* the traversal (a virtual `codegen()` method
  rather than the visitor pattern, as the header comment notes).
- **The codegen state** — emitting IR needs shared machinery: somewhere to
  put functions (a *module*), a cursor that knows where the next instruction
  goes (a *builder*), and a symbol table mapping source names like `a` to the
  `llvm::Value*` currently holding them.

```
        AST (from Chapter2)                    LLVM IR (this chapter)
                                                define double @cmp(...) {
             BinaryExprAST('<')                 entry:
              /            \        ──codegen──▶  %cmptmp  = fcmp ult double %x, %y
   VariableExprAST    VariableExprAST             %booltmp = uitofp i1 %cmptmp to double
        ("x")              ("y")                  ret double %booltmp
                                                }
```

Reference: [Chapter 3: Code generation to LLVM IR](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl03.html).
The lexer and parser are unchanged from Chapter2 (see its README for those);
this README covers only what Chapter 3 adds.

The pipeline gains one stage after the parser. The parser now holds an
`IRGenContext&` and, instead of just reporting "Parsed a ...", each `handle*`
calls `codegen()` on the resulting AST and prints the generated IR:

```
   tokens ──▶ ┌─────────────────────────┐   AST    ┌──────────────────────────────┐
              │ toy::Parser             │ ───────▶ │ codegen(ctx)  (codegen.cpp)  │
              │   handle*() now calls   │          │  each AST node emits its IR  │
              │   codegen() + print IR  │          └──────────────┬───────────────┘
              └─────────────────────────┘                         │ reads/writes
                                                                  ▼
              ┌────────────────────────────────────────────────────────────────┐
              │ toy::IRGenContext  (ir_gen_ctx.h)                              │
              │   theContext : LLVMContext   — owns types/constants (uniquing) │
              │   theModule  : Module        — container for all functions     │
              │   builder    : IRBuilder<>   — instruction cursor + factory    │
              │   namedValues: map<string, Value*> — symbol table (args only)  │
              └────────────────────────────────────────────────────────────────┘
                                                                  │
                                                                  ▼
                                       llvm::Function printed to stderr
```

The main structural refactor of this chapter: the tutorial keeps codegen
state in four **static globals** (`TheContext`, `TheModule`, `Builder`,
`NamedValues`); here they are bundled into an `IRGenContext` class that
`main.cpp` constructs and every `codegen()` takes by reference. Same
lifetimes, but the dependency is explicit — which is also what makes the
codegen unit tests possible (each test builds a fresh context instead of
resetting globals).

(A precise file-by-file diff against Chapter2 — which files are new, which
are byte-identical, which changed and how — is in
[File-by-file](#file-by-file-what-changed-from-chapter2) near the end.)

## The codegen state: `IRGenContext`

**`Chapter3/include/ir_gen_ctx.h`**
```cpp
class IRGenContext {
public:
    std::unique_ptr<llvm::LLVMContext> theContext;
    std::unique_ptr<llvm::Module> theModule;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::map<std::string, llvm::Value *> namedValues;

    IRGenContext() {
        theContext = std::make_unique<llvm::LLVMContext>();
        theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);
        builder = std::make_unique<llvm::IRBuilder<>>(*theContext);
    }
};
```

What each member is for:

- **`theContext`** — an opaque owner of core LLVM data structures, most
  importantly the type and constant tables. This is why constants and types
  are *requested from* the context (`ConstantFP::get(*theContext, ...)`)
  rather than constructed: LLVM uniques them, so every `2.0` in the module is
  the same object.
- **`theModule`** — the top-level IR container: all functions (and, later,
  global variables) live in one module. It owns the memory of every `Value`
  the codegen produces, which is why `codegen()` can return raw `Value*`.
- **`builder`** — a cursor plus instruction factory. `SetInsertPoint(BB)`
  aims it at a basic block; every `CreateFAdd`/`CreateCall`/... appends one
  instruction there and returns it as a `Value*`.
- **`namedValues`** — the symbol table. In this chapter the only named values
  are **function arguments**, so it is cleared and refilled on every
  function; mutable variables arrive in Chapter 7.

## Expression code generation

Every override has the same contract: emit the IR computing this
subexpression (via `ctx.builder`), and return the `Value*` that holds the
result — or `nullptr` after `logErrorV`, propagating failure exactly like the
parser does.

Numbers and variables are the leaves — no instructions at all, just producing
a `Value*`:

**`Chapter3/src/codegen.cpp`**
```cpp
llvm::Value *NumberExprAST::codegen(IRGenContext &ctx) {
  return llvm::ConstantFP::get(*ctx.theContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen(IRGenContext &ctx) {
  llvm::Value *V = ctx.namedValues[Name];   // note: operator[] inserts a
  if (!V)                                   // null entry on a failed lookup
    return logErrorV("Unknown variable name");
  return V;
}
```

Binary operators show the recursive pattern — children first, then one
instruction combining them:

**`Chapter3/src/codegen.cpp`**
```cpp
llvm::Value *BinaryExprAST::codegen(IRGenContext &ctx) {
  llvm::Value *L = LHS->codegen(ctx);
  llvm::Value *R = RHS->codegen(ctx);
  if (!L || !R)
    return nullptr;

  switch (Op) {
  case '+': return ctx.builder->CreateFAdd(L, R, "addtmp");
  case '-': return ctx.builder->CreateFSub(L, R, "subtmp");
  case '*': return ctx.builder->CreateFMul(L, R, "multmp");
  case '<':
    L = ctx.builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return ctx.builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*ctx.theContext), "booltmp");
  default:  return logErrorV("invalid binary operator");
  }
}
```

Three details worth knowing:

- **The `"addtmp"` strings are only hints.** LLVM appends a numeric suffix on
  collision (`%multmp`, `%multmp1`, `%multmp2`, ...) — SSA requires unique
  names, the hint just keeps the IR readable.
- **`<` needs two instructions** because LLVM is strictly typed: `fcmp`
  produces an `i1` (one-bit bool), but Kaleidoscope's only type is `double`,
  so `uitofp` converts the `i1` to `0.0`/`1.0`. (`ULT` = *unordered* less
  than: it returns true if either operand is NaN — the cheap choice for a toy
  language.)
- **`IRBuilder` constant-folds for free.** If both operands are constants,
  `CreateFAdd` returns a folded `ConstantFP` instead of emitting an
  instruction — type `4+5;` into the REPL and the "function" is just
  `ret double 9.0`. That's why the unit tests for `1.0 <op> 2.0` assert on a
  folded constant, not on an instruction.

Calls look the callee up **in the module** — the module's function table is
effectively the symbol table for functions, which is also why an `extern`
declaration is enough to make a function callable:

**`Chapter3/src/codegen.cpp`**
```cpp
llvm::Value *CallExprAST::codegen(IRGenContext &ctx) {
  llvm::Function *CalleeF = ctx.theModule->getFunction(Callee);
  if (!CalleeF)
    return logErrorV("Unknown function referenced");

  if (CalleeF->arg_size() != Args.size())
    return logErrorV("Incorrect # arguments passed");
  ...
  return ctx.builder->CreateCall(CalleeF, ArgsV, "calltmp");
}
```

The arity check is the first *semantic* (not syntactic) error in the
compiler: `foo(1)` for a two-argument `foo` parses fine and fails here.

## Function code generation

`PrototypeAST::codegen()` creates the *declaration*: since every value is a
double, the function type is fully determined by the argument count —
`double(double, double)` for two parameters. `ExternalLinkage` means the
function is visible outside this module (callable from — and defined in — a
library like `libm`, or referenced by later JIT chapters):

**`Chapter3/src/codegen.cpp`**
```cpp
llvm::Function *PrototypeAST::codegen(IRGenContext &ctx) {
  std::vector<llvm::Type *> Doubles(Args.size(), llvm::Type::getDoubleTy(*ctx.theContext));
  llvm::FunctionType *FT =
      llvm::FunctionType::get(llvm::Type::getDoubleTy(*ctx.theContext), Doubles, false);

  llvm::Function *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, ctx.theModule.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);
  return F;
}
```

`FunctionAST::codegen()` fills in the *body*:

```
 lookup Proto name in module ──found (prior extern)──▶ reuse that Function
        │ not found                                          │
        ▼                                                    │
 Proto->codegen(ctx)  (create declaration)                   │
        └────────────────────────┬───────────────────────────┘
                                 ▼
              create "entry" BasicBlock, SetInsertPoint
                                 ▼
              namedValues.clear(); insert each argument
                                 ▼
              Body->codegen(ctx) ──nullptr──▶ TheFunction->eraseFromParent()
                                 │             (don't leave a bodiless stub)
                                 ▼
              builder->CreateRet(RetVal); verifyFunction(*TheFunction)
```

The pieces in order: a **basic block** is a straight-line sequence of
instructions with no branches into or out of the middle — with no control
flow yet (that's Chapter 5), one `entry` block per function is the whole CFG.
`verifyFunction()` is LLVM's consistency check on the generated IR — cheap
insurance the tutorial recommends running always. And `eraseFromParent()` on
error removes the half-built function so a later, corrected `def` can start
clean.

Two upstream behaviors are kept deliberately:

- **The documented signature bug.** If a function was first declared
  `extern foo(a);` and is then defined as `def foo(b) b;`, the *earlier*
  declaration wins (the module lookup finds it before `Proto->codegen()`
  runs), so the body is codegen'd against argument name `a` — and `b` hits
  "Unknown variable name". The tutorial points this out and leaves the fix
  as an exercise; this refactor keeps the bug.
- **No whole-module dump at exit.** Upstream's `main()` prints the entire
  module after the loop finishes; here each definition is printed as it is
  generated and the exit dump was dropped (see the note at the end of
  `mainLoop()` in `src/parser.cpp`).

## The driver: Printing IR

Each `handle*` now generates and prints; note the top-level case still
deletes the anonymous function after showing it — it has served its purpose,
and the next top-level expression will create a fresh `__anon_expr`:

**`Chapter3/src/parser.cpp`**
```cpp
void Parser::handleTopLevelExpression() {
  if (auto FnAST = parseTopLevelExpr()) {
    if (auto *FnIR = FnAST->codegen(ctx)) {
      fprintf(stderr, "Read top-level expression:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
      FnIR->eraseFromParent();   // Remove the anonymous expression.
    }
  } else {
    getNextToken();              // error recovery, as in Chapter2
  }
}
```

## File-by-file: What changed from Chapter2

Chapter 3 adds new files and touches some existing ones; several files carry
the same name as in Chapter2 but are NOT all identical. The exact split:

**New files**

| File | Purpose |
| --- | --- |
| `include/ir_gen_ctx.h` | `IRGenContext` — bundles the tutorial's four codegen globals. |
| `src/codegen.cpp` | All `codegen()` bodies — the tutorial's "Expression Code Generation" and "Function Code Generation" sections. |
| `test/codegen_test.cpp` | Unit tests for codegen (see Tests). |
| `test/filecheck/codegen.k`, `codegen-error.k` | Replace Chapter2's `parse.k`/`parse-error.k` — the driver output to check is now IR, not "Parsed a ..." lines. |

**Same filename, byte-identical** — safe to skip when reading:
`include/lexer.h`, `src/lexer.cpp`, `src/log.cpp`, `include/log.h` (its
`logErrorV` was declared ahead of time in Chapter2 and is finally used now),
`test/lexer_test.cpp`, `test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter3/src/main.cpp` — wiring in the codegen state:

```cpp
// Chapter2                          // Chapter3
toy::Lexer lexer;                     toy::Lexer lexer;
toy::Parser parser(lexer);            toy::IRGenContext ctx;            // NEW
                                      toy::Parser parser(lexer, ctx);   // ctx threaded in
```

`Chapter3/include/ast.h` — every node grows a `codegen()` declaration (plus
`#include "ir_gen_ctx.h"`); otherwise the classes are unchanged:

```cpp
// Chapter2: pure data                        // Chapter3: data + codegen
class NumberExprAST : public ExprAST {         class NumberExprAST : public ExprAST {
  double Val;                                    double Val;
public:                                        public:
  NumberExprAST(double Val) : Val(Val) {}        NumberExprAST(double Val) : Val(Val) {}
};                                               llvm::Value *codegen(IRGenContext &ctx) override;
                                               };
```

(`ExprAST` gains the pure-virtual `codegen`; `PrototypeAST`/`FunctionAST`
gain the `llvm::Function*`-returning variant.)

`Chapter3/include/parser.h` — the constructor takes and stores the context; every
`parse*` method is untouched:

```cpp
// Chapter2                                   // Chapter3
Parser(Lexer& lexer) : lexer(lexer) {...}      Parser(Lexer& lexer, IRGenContext& ctx)
                                                   : lexer(lexer), ctx(ctx) {...}
private:                                       private:
  Lexer& lexer;                                  Lexer& lexer;
                                                 IRGenContext& ctx;   // NEW
```

`Chapter3/src/parser.cpp` — ALL `parse*` functions are identical to Chapter2; only
the three `handle*` wrappers change, from "report the parse" to "codegen and
print the IR" (and the top-level case erases the anonymous function after
printing):

```cpp
// Chapter2                            // Chapter3
void Parser::handleDefinition() {       void Parser::handleDefinition() {
  if (parseDefinition()) {                if (auto FnAST = parseDefinition()) {
    fprintf(stderr,                         if (auto *FnIR = FnAST->codegen(ctx)) {
      "Parsed a function definition.\n");     fprintf(stderr, "Read function definition:\n");
  } else {                                    FnIR->print(llvm::errs());
    getNextToken(); // error recovery         fprintf(stderr, "\n");
  }                                         }
}                                         } else {
                                            getNextToken(); // error recovery (unchanged)
                                          }
                                        }
```

`mainLoop()` itself is unchanged except for a note documenting that an
unreachable end-of-loop module dump was removed.

`Chapter3/test/parser_test.cpp` — mechanical constructor update only (each test now
builds an `IRGenContext ctx;` and calls `Parser parser(lexer, ctx);`); the
test cases themselves are identical.

`Chapter3/CMakeLists.txt` — adds `src/codegen.cpp` to `toy_core` and registers the
`codegen_test` executable.

`Chapter3/cmd.txt` — new demo input matching the chapter (functions worth looking at
as IR).

## Build and run

Same recipe as Chapter2 (`./build.sh`, or cmake + Ninja by hand). A session
with `./build/toy < cmd.txt`:

```
ready> 4+5;
Read top-level expression:
define double @__anon_expr() {
entry:
  ret double 9.000000e+00        ; <-- IRBuilder constant-folded 4+5
}

ready> def foo(a b) a*a + 2*a*b + b*b;
Read function definition:
define double @foo(double %a, double %b) {
entry:
  %multmp = fmul double %a, %a
  %multmp1 = fmul double 2.000000e+00, %a
  %multmp2 = fmul double %multmp1, %b
  %addtmp = fadd double %multmp, %multmp2
  %multmp3 = fmul double %b, %b
  %addtmp4 = fadd double %addtmp, %multmp3
  ret double %addtmp4
}

ready> def bar(a) foo(a, 4.0) + bar(31337);
Read function definition:
define double @bar(double %a) {
entry:
  %calltmp = call double @foo(double %a, double 4.000000e+00)
  %calltmp1 = call double @bar(double 3.133700e+04)
  %addtmp = fadd double %calltmp, %calltmp1
  ret double %addtmp
}
```

Note `bar` calls *itself*: by the time the body is codegen'd, `bar`'s own
declaration is already in the module (created a few lines earlier in
`FunctionAST::codegen`), so the recursive `getFunction("bar")` lookup
succeeds — forward-progress for free.

```
ready> extern cos(x);
Read extern:
declare double @cos(double)      ; declare (no body) vs define

ready> cos(1.234);
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @cos(double 1.234000e+00)
  ret double %calltmp
}
```

## Tests

Same two-scheme setup as Chapter2 — see the [top-level
README](../README.md#testing-the-two-schemes) for why the two layers exist,
how LLVM itself uses them, and how lit works with `lit.cfg`. What Chapter 3
adds:

- **`test/codegen_test.cpp`** (GoogleTest, new): builds AST nodes *by hand*
  (no parser involved) against a fresh `IRGenContext` per test — exactly what
  bundling the globals into a class buys. It checks in-memory IR properties
  that textual matching can't express directly: `isa<ConstantFP>` /
  `isa<CallInst>` on the returned `Value*`, the folded value of constant
  expressions (`1.0 + 2.0` must *be* the constant `3.0`, since `IRBuilder`
  folds it), the entry block and terminator of a generated function, and the
  `nullptr`-on-error contract (unknown callee, arity mismatch, invalid
  operator).
- **`test/filecheck/codegen.k`**: the lit tests now check the **printed IR**,
  which is where FileCheck starts to shine — `CHECK-LABEL` splits the output
  per function so matches can't bleed across, regex captures like
  `[[CMP:%.*]] = fcmp ult ...` then `uitofp i1 [[CMP]]` verify *dataflow*
  (the uitofp consumes the fcmp's result, whatever LLVM named it), and
  `CHECK-NEXT`/`CHECK-COUNT-2` pin ordering and counts. Chapter 3 emits raw,
  unoptimized IR, so the expected instruction structure is stable.
- **`test/filecheck/codegen-error.k`**: codegen-stage errors (unknown
  variable, unknown function) must be reported *and* the driver must keep
  accepting input — same error-recovery contract the parser had, one stage
  later.

```sh
ctest --test-dir build                 # everything
./build/codegen_test                   # the new unit tests directly
lit -v test/filecheck/codegen.k        # one lit test (TOY_BIN defaults to ./build/toy)
```
