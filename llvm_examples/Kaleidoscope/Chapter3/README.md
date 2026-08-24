# Chapter 3 — Code Generation to LLVM IR

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
(Upstream opens with two notes: building the lexer and parser was much more
work than generating IR will be, and its code needs LLVM ≥ 3.7 — the version
caveat is long subsumed by this repo's Homebrew-LLVM requirement.)
The lexer and parser are unchanged from [Chapter2](../Chapter2/README.md)
(see its README for those); this README covers only what Chapter 3 adds.

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

## Code generation setup: The `codegen()` methods

Before any IR can be emitted, every AST node gets a code-generation entry
point — a pure-virtual `codegen()` on `ExprAST` that each concrete node
overrides (declarations only; all the bodies live in `src/codegen.cpp`):

**`Chapter3/include/ast.h`**
```cpp
/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() = default;

  // Use a simple virtual method for code generation instead of common visitor pattern
  virtual llvm::Value *codegen(IRGenContext &ctx) = 0;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}

  llvm::Value *codegen(IRGenContext &ctx) override;
};
// ... VariableExprAST, BinaryExprAST, CallExprAST declare the same override;
// PrototypeAST and FunctionAST return llvm::Function* instead (see below).
```

The contract: `codegen()` emits the IR for this node *and everything it
depends on*, and returns the resulting `llvm::Value*`. `Value` is LLVM's
class for an **SSA value** — the result of one instruction (or a constant,
or a function argument). Its defining property is that it is assigned by
exactly the instruction that computes it and can never be reassigned — there
is no way to "change" an SSA value. This is the "infinitely many registers,
each written once" model from the intro, seen from the C++ API side: a
`Value*` in hand *is* dataflow, not storage.

Two notes on the shape of this API:

- **Virtual method, not visitor.** As the header comment says, dispatch is a
  plain virtual method rather than the visitor pattern a production compiler
  might use — upstream makes the same choice with the same shrug ("this
  tutorial won't dwell on good software engineering practices; adding a
  virtual method is simplest").
- **The `IRGenContext&` parameter is the refactor.** Upstream's signature is
  `virtual Value *codegen() = 0;` — no parameters, because the emission
  machinery lives in four file-scope statics. Here that machinery is the
  `IRGenContext` described next, and every `codegen()` receives it
  explicitly (the globals→class bundling from the intro, landing in code).

Codegen also extends the parser's error convention. Upstream introduces the
codegen state and its error helper in a single block — "a 'LogError' method
like we used for the parser, which will be used to report errors found during
code generation (for example, use of an undeclared parameter)":

**`Chapter3/src/log.cpp`**
```cpp
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value *> NamedValues;

Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}
```

The four statics are the file-scope emission machinery just mentioned — they
become the members of the `IRGenContext` described in the next section.
`LogErrorV` becomes `logErrorV`, a third helper in `log.h` (new in this
chapter) beside the parser's `logError`/`logErrorP`:

**`Chapter3/src/log.cpp`**
```cpp
llvm::Value *logErrorV(const char *str) {
  logError(str);
  return nullptr;
}
```

It prints `Error: ...` and returns a null `llvm::Value*`, so a
code-generation failure bubbles up through the recursion exactly like a
parse failure does.

One assumption to keep in mind while reading the next section: expression
`codegen()` never chooses *where* instructions go — it assumes `ctx.builder`
is already aimed at a basic block. Setting that up is
`FunctionAST::codegen()`'s job (it creates the `entry` block and calls
`SetInsertPoint` before walking the body — see "Function code generation").

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

- **`LLVMContext`** — an opaque owner of core LLVM data structures, most
  importantly the type and constant tables. This is why constants and types
  are *requested from* the context (`ConstantFP::get(*theContext, ...)`)
  rather than constructed: LLVM uniques them, so every `2.0` in the module is
  the same object.
- **`Module`** — the top-level IR container: all functions and global variables
  live in one module. It owns the memory of every `Value`
  the codegen produces, which is why `codegen()` can return a raw `Value*`
  rather than some owning `unique_ptr<Value>`.
- **`IRBuilder`** — a helper object that makes it easy to generate LLVM instructions.
  Instances of the IRBuilder class template keep track of the current place to insert
  instructions and has methods to create new instructions. `SetInsertPoint(BB)`
  aims it at a basic block; every `CreateFAdd`/`CreateCall`/... appends one
  instruction there and returns it as a `Value*`.
- **`namedValues`** — the symbol table. In this chapter the only named values
  are **function arguments**, so it is cleared and refilled on every
  function; loop induction variables join it in Chapter 5, and mutable local
  variables in Chapter 7.

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

A number becomes a `ConstantFP`, which stores the value in an `APFloat` —
LLVM's arbitrary-precision float, hence the name. Note the API idiom:
`ConstantFP::get(...)`, never `new ConstantFP(...)` — constants (like types)
are uniqued and shared inside the `LLVMContext`, so code *requests* them
rather than constructs them, as the `theContext` bullet above explained.

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
- **`<` needs two instructions** because LLVM's typing rules are strict: an
  instruction's operand and result types must agree (which is also why
  `fadd`/`fsub`/`fmul` are one-liners — everything is `double`), and `fcmp`
  always produces an `i1` (one-bit bool). Kaleidoscope's only type being
  `double`, `uitofp` converts the `i1` to `0.0`/`1.0`. It must be `uitofp`,
  not `sitofp`: treating the one-bit value as *signed* would read `1` as
  `-1`, making `<` return `0.0`/`-1.0`. (`ULT` = *unordered* less than: it
  returns true if either operand is NaN — the cheap choice for a toy
  language.)
- **`IRBuilder` constant-folds for free.** If both operands are constants,
  `CreateFAdd` returns a folded `ConstantFP` instead of emitting an
  instruction — type `4+5;` into the REPL and the "function" is just
  `ret double 9.0`. That's why the unit tests for `1.0 <op> 2.0` assert on a
  folded constant, not on an instruction. Apart from this folding the IR is
  a literal transcription of the AST — explicit optimization passes arrive
  in Chapter 4.

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

  std::vector<llvm::Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen(ctx));
    if (!ArgsV.back())
      return nullptr;
  }
  
  return ctx.builder->CreateCall(CalleeF, ArgsV, "calltmp");
}
```

The arity check is the first *semantic* (not syntactic) error in the
compiler: `foo(1)` for a two-argument `foo` parses fine and fails here.

Two more things come for free. LLVM emits `call` with the **native C calling
convention** by default, so the same mechanism reaches standard-library
functions like `sin` and `cos` with no extra glue — that is the whole magic
behind `extern sin(x);`. And the framework is easy to extend: the
[LLVM language reference](https://llvm.org/docs/LangRef.html) is full of
instructions that would plug into `BinaryExprAST::codegen`'s switch just as
easily (upstream's suggested exercise).

## Function code generation

Prototypes and functions involve more bookkeeping than expressions did —
upstream half-apologizes for the "less beautiful" code — but every detail
illustrates something. `PrototypeAST::codegen()` creates the *declaration*:
since every value is a
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

Details worth noting: this `codegen()` returns `llvm::Function*`, not
`Value*` — a prototype describes a function's *interface*, not a computed
value. The `false` argument to `FunctionType::get` means *not vararg*.
Passing `ctx.theModule.get()` as the last argument of `Function::Create`
inserts the new function into the module and registers its name in the
module's symbol table — the very table `CallExprAST::codegen` resolves
callees against. And
the final `setName` loop isn't strictly necessary (LLVM would invent names),
but keeping the user's parameter names makes the IR readable and lets the
body's codegen refer to arguments directly rather than consulting the
prototype. The resulting `Function` is a declaration with no body — exactly
LLVM's representation of an `extern`; for definitions, a body gets attached
next.

`FunctionAST::codegen()` fills in the *body*. In overview:

```
 lookup Proto name in module ──found (prior extern)──▶ reuse that Function
        │ not found                                          │
        ▼                                                    │
 Proto->codegen(ctx)  (create declaration)                   │
        └────────────────────────┬───────────────────────────┘
                                 ▼
              already has a body? ──yes──▶ logErrorV("Function cannot be redefined.")
                                 │ no
                                 ▼
              create "entry" BasicBlock, SetInsertPoint
                                 ▼
              namedValues.clear(); insert each argument
                                 ▼
              Body->codegen(ctx) ──nullptr──▶ TheFunction->eraseFromParent()
                                 │             (don't leave a broken stub)
                                 ▼
              builder->CreateRet(RetVal); verifyFunction(*TheFunction)
```

The full function, with the phases marked:

**`Chapter3/src/codegen.cpp`**
```cpp
llvm::Function *FunctionAST::codegen(IRGenContext &ctx) {
  // First, check for an existing function from a previous 'extern' declaration.
  llvm::Function *TheFunction = ctx.theModule->getFunction(Proto->getName());   // [1]

  if (!TheFunction)
    TheFunction = Proto->codegen(ctx);

  if (!TheFunction)
    return nullptr;

  if (!TheFunction->empty())                                                    // [2]
    return (llvm::Function *)logErrorV("Function cannot be redefined.");

  // Create a new basic block to start insertion into.
  llvm::BasicBlock *BB = llvm::BasicBlock::Create(*ctx.theContext, "entry", TheFunction);  // [3]
  ctx.builder->SetInsertPoint(BB);

  // Record the function arguments in the NamedValues map.
  ctx.namedValues.clear();                                                      // [4]
  for (auto &Arg : TheFunction->args())
    ctx.namedValues[std::string(Arg.getName())] = &Arg;

  if (llvm::Value *RetVal = Body->codegen(ctx)) {                               // [5]
    // Finish off the function.
    ctx.builder->CreateRet(RetVal);

    // Validate the generated code, checking for consistency.
    llvm::verifyFunction(*TheFunction);

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();                                               // [6]
  return nullptr;
}
```

- **[1] Resolve the `Function`.** The module is searched first, in case this
  name was already declared by a previous `extern` — then the existing
  (bodiless) `Function` is reused rather than a new one created. Only when
  the lookup comes back null does `Proto->codegen(ctx)` create the
  declaration.
- **[2] Refuse redefinition.** Either way, the function must still be
  *empty* — no body yet. A body means this `def` is a redefinition, which is
  an error in this chapter. (Chapter 4 drops this guard when every function
  moves into its own module — upstream meant redefinition to become a REPL
  feature there, but modern ORC rejects duplicate symbols, so it fails at
  the JIT layer instead; see Chapter4's README.)
- **[3] Give the builder somewhere to point.** A **basic block** is a
  straight-line run of instructions: execution enters only at the top and
  leaves only at the bottom, which is what makes blocks the nodes of the
  **control-flow graph (CFG)**. `BasicBlock::Create` makes an empty block
  named `entry` *inside* `TheFunction`, and `SetInsertPoint` aims the
  builder at its end — this is the "assumes the builder is already set up"
  promise from the setup section being kept. With no control flow in the
  language yet, one block per function is the entire CFG; Chapter 5 adds
  branching, and with it functions of many blocks.
- **[4] Populate the symbol table.** `namedValues` is cleared (whatever the
  previous function left there is out of scope) and refilled with this
  function's arguments, keyed by the names `PrototypeAST::codegen` set — so
  that when the body's `VariableExprAST::codegen` looks up `x`, it finds the
  corresponding argument's `Value`.
- **[5] Emit the body and finish.** `Body->codegen(ctx)` emits the whole
  expression tree into the entry block and returns the `Value*` holding its
  result; `CreateRet` makes that the function's return value, completing the
  function. `verifyFunction` then runs LLVM's consistency checks over the
  generated code — the tutorial recommends always running it: it is cheap
  and catches a lot of compiler bugs.
- **[6] Or clean up.** If the body failed (`nullptr` — say, an unknown
  variable), the half-built function is deleted with `eraseFromParent`.
  Leaving it would be worse than it looks: it would sit in the module *with
  a body*, so the guard at [2] would forever refuse the user's corrected
  retry. Note the asymmetry this creates: a `def` whose body *failed* may be
  retried, while a `def` that *succeeded* is permanent.

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
and the next top-level expression will create a fresh `__anon_expr` (in
Chapter 4, that anonymous wrapper becomes exactly what the JIT looks up and
executes):

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
`include/lexer.h`, `src/lexer.cpp`, `test/lexer_test.cpp`,
`test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter3/src/main.cpp` — wiring in the codegen state:

```cpp
// Chapter2                          // Chapter3
toy::Lexer lexer;                     toy::Lexer lexer;
toy::Parser parser(lexer);            toy::IRGenContext ctx;            // NEW
                                      toy::Parser parser(lexer, ctx);   // ctx threaded in
```

`Chapter3/include/ast.h` — every node grows a `codegen()` declaration (plus
the LLVM IR headers and `#include "ir_gen_ctx.h"` — Chapter2's copy included
no LLVM at all); otherwise the classes are unchanged:

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

`Chapter3/include/log.h` + `src/log.cpp` — a third helper for the new failure
domain: `logErrorV` (upstream's `LogErrorV`) reports and returns a null
`llvm::Value*` the way `logError` returns a null AST node:

```cpp
// Chapter2                                      // Chapter3
std::unique_ptr<ExprAST> logError(...);           std::unique_ptr<ExprAST> logError(...);
std::unique_ptr<PrototypeAST> logErrorP(...);     std::unique_ptr<PrototypeAST> logErrorP(...);
                                                  llvm::Value *logErrorV(const char *str);  // NEW
```

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

`Chapter3/CMakeLists.txt` — adds `src/codegen.cpp` to `toy_core`, registers the
`codegen_test` executable, and — new this chapter — finds LLVM, links the
`core` component, and pins the macOS deployment target to match the Homebrew
LLVM libraries (Chapter2, like upstream's Chapter 2, has no LLVM build
dependency at all).

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
succeeds — forward-progress for free. (Don't actually *call* `bar`, though:
with no conditionals until Chapter 5 the recursion has no base case, so it
would run forever — upstream makes the same joke.)

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
  variable, unknown function, function redefinition) must be reported *and*
  the driver must keep accepting input — same error-recovery contract the
  parser had, one stage later.

```sh
ctest --test-dir build                 # everything
./build/codegen_test                   # the new unit tests directly
lit -v test/filecheck/codegen.k        # one lit test (TOY_BIN defaults to ./build/toy)
```
