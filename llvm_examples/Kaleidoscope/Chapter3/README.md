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

- **Code generation as a tree walk** — a code generator walks the AST and
  emits the IR for each node: expressions produce the `llvm::Value*` holding
  their result, after recursively emitting their children. In this repo the
  walk is a separate component that *dispatches on the node's kind*; the AST
  itself stays pure data (the tutorial instead puts a virtual `codegen()` on
  every node — see [Deviations from upstream](#codegen-on-the-ast-kind-tags-instead-of-a-virtual-codegen) for why we differ).
- **The codegen state** — emitting IR needs shared machinery: somewhere to
  put functions (a *module*), a cursor that knows where the next instruction
  goes (a *builder*), and a symbol table mapping source names like `a` to the
  `llvm::Value*` currently holding them. That state is private to the code
  generator.

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
The lexer and the `parse*` methods are unchanged from
[Chapter2](../Chapter2/README.md) (see its README for those); this README
covers only what Chapter 3 adds.

The pipeline gains one stage after the parser. Chapter2's `Driver` — which
already owns the lexer and the parser and runs the read-eval-print loop —
gains a code generator as its third member, hands each parsed top-level
construct to it, and prints the IR that comes back:

```
   stdin ──▶ ┌──────────────────────────────────────────────────────────────────────┐
             │ toy::Driver  (driver.h / driver.cpp)     — the composition root      │
             │                                                                      │
             │   Lexer ──tokens──▶ Parser ──AST──▶ CodeGenSession ──llvm::Function* │
             │                     parse*()        emitFunction()        │          │
             │                                     emitPrototype()       ▼          │
             │   mainLoop(): ready> ... handle*() ... print IR to stderr            │
             └──────────────────────────────────────────────────────────────────────┘
                                                        │
                                                        ▼  (codegen.h shows only this)
             ┌──────────────────────────────────────────────────────────────────────┐
             │ toy::CodeGenSession  — public facade, no LLVM IR headers             │
             │   emitPrototype(PrototypeAST&)  emitFunction(FunctionAST&)           │
             │   currentModule()                                                    │
             ├──────────────────────────────────────────────────────────────────────┤
             │ CodeGenSession::Impl  (codegen.cpp)  — private                       │
             │   theContext : LLVMContext   — owns types/constants (uniquing)       │
             │   theModule  : Module        — container for all functions           │
             │   builder    : IRBuilder<>   — instruction cursor + factory          │
             │   namedValues: map<string, Value*> — symbol table (args only)        │
             │   emitExpr(ExprAST&)  ── switch (getKind()) ──▶ emit(NumberExprAST&) │
             │                                                emit(VariableExprAST&)│
             │                                                 emit(BinaryExprAST&) │
             │                                                 emit(CallExprAST&)   │
             └──────────────────────────────────────────────────────────────────────┘
```

The main structural decision of this chapter, and the one every later
chapter builds on. The tutorial keeps codegen state in four **static
globals** (`TheContext`, `TheModule`, `Builder`, `NamedValues`) and gives
every AST node a virtual `codegen()` that reads them. Here:

- **The four globals and the `codegen()` bodies move into one private
  struct**, `CodeGenSession::Impl`, defined entirely inside `codegen.cpp`.
  The public class `CodeGenSession` (`codegen.h`) is a *facade* over it — the
  **pImpl** idiom: a header that shows four methods and a
  `std::unique_ptr<Impl>`, and nothing else. Consumers of the code generator
  (the driver, the tests) compile without any LLVM IR header.
- **The AST stays pure data — byte-identical to Chapter2's.** Instead of a
  `codegen()` virtual on every node, the `Impl` walks the tree from the
  outside, switching on the kind tag and reading through the getters that
  Chapter2 already gave the nodes — the way clang's `CodeGenFunction` and
  the MLIR Toy tutorial's `MLIRGenImpl` do it. `ast.h` still includes no
  LLVM, so the lexer and parser never depend on it.
- **The driver gains one member.** Chapter2's `Driver` already owned the
  lexer and parser and ran the loop; now it owns a `CodeGenSession` too and
  is the one place that knows a parsed function goes to codegen next. The
  parser is untouched — it never learns the code generator exists. In
  Chapter 4 the JIT lands here as well.

Same lifetimes as upstream, same printed output (the lit tests prove it), but
the dependencies are explicit and one-directional: `Driver → Parser → AST`
and `Driver → CodeGenSession → AST`. The whole frontend — `ast.h`,
`parser.*`, `lexer.*`, `main.cpp` and their tests — is byte-identical to
Chapter2's: this chapter *adds* a stage, it does not modify the ones before
it. That is also what makes the codegen unit tests possible (each test
builds a fresh session instead of resetting globals).

(A precise file-by-file diff against Chapter2 — which files are new, which
are byte-identical, which changed and how — is in
[File-by-file](#file-by-file-what-changed-from-chapter2) near the end.)

## Code generation setup: Reading the AST from outside

The tutorial's first step is to give every AST node a code-generation entry
point: a pure-virtual `codegen()` on `ExprAST` that each concrete node
overrides. We take the road not taken, and in fact took it already —
[Chapter2](../Chapter2/README.md#21-the-abstract-syntax-tree)'s nodes are
pure data carrying a **kind tag** (`getKind()`, with a `classof()` per
subclass) and **getters**, so a consumer can tell which node it is holding
and read its data without the node knowing the consumer exists. `ast.h` does
not change in this chapter at all, and still includes no LLVM header. Why
the walk lives outside the tree rather than in a virtual method is in
[Deviations from upstream](#codegen-on-the-ast-kind-tags-instead-of-a-virtual-codegen).

What *is* new is the first consumer that uses those tags for real, through
LLVM's own templates. Chapter2 linked no LLVM, so its tests compared
`getKind()` by hand; from here on the code generator goes through the
**LLVM-style RTTI** trio the tags were designed for — `llvm::isa<T>(x)`,
`llvm::cast<T>(x)` (asserting) and `llvm::dyn_cast<T>(x)` (null on
mismatch), each dispatching to `T::classof` (see
[Chapter2](../Chapter2/README.md#21-the-abstract-syntax-tree) for the scheme
and why LLVM hand-rolls it). It uses them on our nodes exactly as it uses
`isa<ConstantFP>(V)` on LLVM's own IR: `emitExpr()` below is one `switch`
over `getKind()` plus a `cast<>` to the concrete node, and Chapter 7 uses
`dyn_cast<VariableExprAST>` to recognize an assignment target.

Codegen also extends the parser's error convention. Upstream introduces the
codegen state and its error helper in a single block — "a 'LogError' method
like we used for the parser, which will be used to report errors found during
code generation (for example, use of an undeclared parameter)":

```cpp
// upstream (LangImpl03)
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value *> NamedValues;

Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}
```

The four statics are the emission machinery; they become the members of the
`Impl` described in the next section. `LogErrorV` becomes `logErrorV`, a
third helper in `log.h` beside the parser's `logError`/`logErrorP`:

**`Chapter3/src/log.cpp`**
```cpp
llvm::Value *logErrorV(const char *str) {
  logError(str);
  return nullptr;
}
```

It prints `Error: ...` and returns a null `llvm::Value*`, so a
code-generation failure bubbles up through the recursion exactly like a
parse failure does. (`log.h` only *forward-declares* `llvm::Value` for this
signature, so including it still pulls in no LLVM headers.)

One assumption to keep in mind while reading on: expression emission never
chooses *where* instructions go — it assumes `builder` is already aimed at a
basic block. Setting that up is `emitFunction()`'s job (it creates the
`entry` block and calls `SetInsertPoint` before walking the body — see
"Function code generation").

## The codegen component: `CodeGenSession` and its `Impl`

The public header is small on purpose:

**`Chapter3/include/codegen.h`**
```cpp
namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace toy {

class PrototypeAST;
class FunctionAST;

/// A code generation session owning one LLVMContext, one Module and the
/// IRBuilder that emits into it.
class CodeGenSession {
public:
  CodeGenSession();
  ~CodeGenSession();

  /// Emit a function declaration (`extern`). Returns the llvm::Function.
  llvm::Function *emitPrototype(PrototypeAST &proto);

  /// Emit a function definition (`def ...` or an anonymous top-level
  /// expression). Returns the llvm::Function, or nullptr after reporting an
  /// error -- in which case nothing half-built is left in the module.
  llvm::Function *emitFunction(FunctionAST &fn);

  /// The module being populated (e.g. to print it or look functions up).
  llvm::Module &currentModule();

  /// Remove a function this session emitted -- the driver uses it to discard a
  /// top-level expression once it has been printed. The module is the
  /// session's, so removals go through the session too: it also drops any
  /// state it keeps about the function (from Chapter 4 on, the analyses the
  /// pass managers cached for it, which would otherwise dangle).
  void eraseFunction(llvm::Function *fn);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // end namespace toy
```

Three things to notice. Only two LLVM types cross the boundary, and both are
*forward-declared*: a consumer that just wants to call `emitFunction` and
print the result includes `llvm/IR/Function.h` itself; one that never touches
the result (a test that only checks for `nullptr`) needs nothing. The
constructor and destructor are declared here but *defined in `codegen.cpp`*
— a `std::unique_ptr<Impl>` can only be destroyed where `Impl` is a complete
type, so `~CodeGenSession() = default;` must live next to the struct
definition, not in the header. And the name is *session*, not *context*:
the object owns the whole lifetime of one module being built, and Chapter 4
will hand that module off to the JIT and start a fresh one through this same
object.

Behind the facade sits the state the tutorial kept in globals:

**`Chapter3/src/codegen.cpp`**
```cpp
struct CodeGenSession::Impl {
  std::unique_ptr<llvm::LLVMContext> theContext;
  std::unique_ptr<llvm::Module> theModule;
  std::unique_ptr<llvm::IRBuilder<>> builder;
  std::map<std::string, llvm::Value *> namedValues;

  Impl() {
    // Open a new context and module.
    theContext = std::make_unique<llvm::LLVMContext>();
    theModule = std::make_unique<llvm::Module>("my cool jit", *theContext);
    // Create a new builder for the module.
    builder = std::make_unique<llvm::IRBuilder<>>(*theContext);
  }

  // ... emitExpr / emit(...) / emitPrototype / emitFunction (next sections)
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
  the codegen produces, which is why `emit*()` can return a raw `Value*`
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

`Impl` is the struct that grows over the coming chapters — the function pass
manager and the module hand-off in Chapter 4, the function-prototype registry
that survives across modules, debug-info builders in Chapter 9 — while the
public header changes in only two of them. That stability is the point of
the facade: the driver's view of "the code generator" is fixed early, and
everything the LLVM API forces on us stays behind it.

The `Impl` also owns the walk. Every expression enters through one method
that looks at the kind tag and hands the node to the matching overload:

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Value *emitExpr(ExprAST &expr) {
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    case ExprAST::Expr_Var:
      return emit(llvm::cast<VariableExprAST>(expr));
    case ExprAST::Expr_BinOp:
      return emit(llvm::cast<BinaryExprAST>(expr));
    case ExprAST::Expr_Call:
      return emit(llvm::cast<CallExprAST>(expr));
    }
    llvm_unreachable("unknown expression kind");
  }
```

This one function replaces the virtual dispatch: where upstream writes
`LHS->codegen()`, we write `emitExpr(*bin.getLHS())`, and the `switch` does
what the vtable did. `llvm::cast<>` is the asserting member of the RTTI trio
above — it fires an assertion in debug builds if the tag and the type
disagree, and compiles to a plain pointer conversion in release. The `switch`
has no `default:` on purpose: with every enumerator listed, clang's
`-Wswitch` warns the moment a later chapter adds a kind and forgets a case,
and `llvm_unreachable` after the switch documents that falling out of it is
a bug, not a path. The four `emit()` overloads are, line for line, the
tutorial's four `codegen()` bodies with the `ctx.`/`this->` shuffle undone:
`Val` becomes `num.getVal()`, `TheContext` becomes `theContext`.

## Expression code generation

Every `emit()` has the same contract: emit the IR computing this
subexpression (via `builder`), and return the `Value*` that holds the
result — or `nullptr` after `logErrorV`, propagating failure exactly like the
parser does.

Numbers and variables are the leaves — no instructions at all, just producing
a `Value*`:

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Value *emit(NumberExprAST &num) {
    return llvm::ConstantFP::get(*theContext, llvm::APFloat(num.getVal()));
  }

  llvm::Value *emit(VariableExprAST &var) {
    // Look this variable up in the function.
    llvm::Value *V = namedValues[var.getName()];   // note: operator[] inserts a
    if (!V)                                          // null entry on a failed lookup
      return logErrorV("Unknown variable name");
    return V;
  }
```

A number becomes a `ConstantFP`, which stores the value in an `APFloat` —
LLVM's arbitrary-precision float, hence the name. Note the API idiom:
`ConstantFP::get(...)`, never `new ConstantFP(...)` — constants (like types)
are uniqued and shared inside the `LLVMContext`, so code *requests* them
rather than constructs them, as the `theContext` bullet above explained.
`Value` itself is LLVM's class for an **SSA value** — the result of one
instruction (or a constant, or a function argument). Its defining property is
that it is assigned by exactly the instruction that computes it and can never
be reassigned; a `Value*` in hand *is* dataflow, not storage.

Binary operators show the recursive pattern — children first, then one
instruction combining them:

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Value *emit(BinaryExprAST &bin) {
    llvm::Value *L = emitExpr(*bin.getLHS());
    llvm::Value *R = emitExpr(*bin.getRHS());
    if (!L || !R)
      return nullptr;

    switch (bin.getOp()) {
    case '+': return builder->CreateFAdd(L, R, "addtmp");
    case '-': return builder->CreateFSub(L, R, "subtmp");
    case '*': return builder->CreateFMul(L, R, "multmp");
    case '<':
      L = builder->CreateFCmpULT(L, R, "cmptmp");
      // Convert bool 0/1 to double 0.0 or 1.0
      return builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*theContext), "booltmp");
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
  llvm::Value *emit(CallExprAST &call) {
    // Look up the name in the global module table.
    llvm::Function *CalleeF = theModule->getFunction(call.getCallee());
    if (!CalleeF)
      return logErrorV("Unknown function referenced");

    // If argument mismatch error.
    const auto &Args = call.getArgs();
    if (CalleeF->arg_size() != Args.size())
      return logErrorV("Incorrect # arguments passed");

    std::vector<llvm::Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      ArgsV.push_back(emitExpr(*Args[i]));
      if (!ArgsV.back())
        return nullptr;
    }

    return builder->CreateCall(CalleeF, ArgsV, "calltmp");
  }
```

The arity check is the first *semantic* (not syntactic) error in the
compiler: `foo(1)` for a two-argument `foo` parses fine and fails here.

Two more things come for free. LLVM emits `call` with the **native C calling
convention** by default, so the same mechanism reaches standard-library
functions like `sin` and `cos` with no extra glue — that is the whole magic
behind `extern sin(x);`. And the framework is easy to extend: the
[LLVM language reference](https://llvm.org/docs/LangRef.html) is full of
instructions that would plug into `emit(BinaryExprAST&)`'s switch just as
easily (upstream's suggested exercise).

## Function code generation

Prototypes and functions involve more bookkeeping than expressions did —
upstream half-apologizes for the "less beautiful" code — but every detail
illustrates something. `emitPrototype()` creates the *declaration*: since
every value is a double, the function type is fully determined by the
argument count — `double(double, double)` for two parameters.
`ExternalLinkage` means the function is visible outside this module (callable
from — and defined in — a library like `libm`, or referenced by later JIT
chapters):

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Function *emitPrototype(PrototypeAST &proto) {
    const auto &Args = proto.getArgs();

    // Make the function type:  double(double,double) etc.
    std::vector<llvm::Type *> Doubles(Args.size(), llvm::Type::getDoubleTy(*theContext));
    llvm::FunctionType *FT =
        llvm::FunctionType::get(llvm::Type::getDoubleTy(*theContext), Doubles, false);

    // Create the IR Function corresponding to the Prototype
    llvm::Function *F =
        llvm::Function::Create(FT, llvm::Function::ExternalLinkage, proto.getName(), theModule.get());

    // Set names for all arguments.
    unsigned Idx = 0;
    for (auto &Arg : F->args())
      Arg.setName(Args[Idx++]);

    return F;
  }
```

Details worth noting: this returns `llvm::Function*`, not `Value*` — a
prototype describes a function's *interface*, not a computed value (which is
also why prototypes are not part of the `emitExpr` switch: `PrototypeAST` and
`FunctionAST` are not expressions and have no kind tag). The `false` argument
to `FunctionType::get` means *not vararg*. Passing `theModule.get()` as the
last argument of `Function::Create` inserts the new function into the module
and registers its name in the module's symbol table — the very table
`emit(CallExprAST&)` resolves callees against. And the final `setName` loop
isn't strictly necessary (LLVM would invent names), but keeping the user's
parameter names makes the IR readable and lets the body's codegen refer to
arguments directly rather than consulting the prototype. The resulting
`Function` is a declaration with no body — exactly LLVM's representation of
an `extern`; for definitions, a body gets attached next.

`emitFunction()` fills in the *body*. In overview:

```
 lookup Proto name in module ──found (prior extern)──▶ reuse that Function
        │ not found                                          │
        ▼                                                    │
 emitPrototype(Proto)  (create declaration)                  │
        └────────────────────────┬───────────────────────────┘
                                 ▼
              already has a body? ──yes──▶ logErrorV("Function cannot be redefined.")
                                 │ no
                                 ▼
              create "entry" BasicBlock, SetInsertPoint
                                 ▼
              namedValues.clear(); insert each argument
                                 ▼
              emitExpr(*Body) ──nullptr──▶ TheFunction->eraseFromParent()
                                 │          (don't leave a broken stub)
                                 ▼
              builder->CreateRet(RetVal); verifyFunction(*TheFunction)
```

The full function, with the phases marked:

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Function *emitFunction(FunctionAST &fn) {
    PrototypeAST &Proto = *fn.getProto();

    // First, check for an existing function from a previous 'extern' declaration.
    llvm::Function *TheFunction = theModule->getFunction(Proto.getName());     // [1]

    if (!TheFunction)
      TheFunction = emitPrototype(Proto);

    if (!TheFunction)
      return nullptr;

    if (!TheFunction->empty())                                                  // [2]
      return (llvm::Function *)logErrorV("Function cannot be redefined.");

    // Create a new basic block to start insertion into.
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(*theContext, "entry", TheFunction);  // [3]
    builder->SetInsertPoint(BB);

    // Record the function arguments in the NamedValues map.
    namedValues.clear();                                                        // [4]
    for (auto &Arg : TheFunction->args())
      namedValues[std::string(Arg.getName())] = &Arg;

    if (llvm::Value *RetVal = emitExpr(*fn.getBody())) {                        // [5]
      // Finish off the function.
      builder->CreateRet(RetVal);

      // Validate the generated code, checking for consistency.
      llvm::verifyFunction(*TheFunction);

      return TheFunction;
    }

    // Error reading body, remove function.
    TheFunction->eraseFromParent();                                             // [6]
    return nullptr;
  }
```

- **[1] Resolve the `Function`.** The module is searched first, in case this
  name was already declared by a previous `extern` — then the existing
  (bodiless) `Function` is reused rather than a new one created. Only when
  the lookup comes back null does `emitPrototype(Proto)` create the
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
  function's arguments, keyed by the names `emitPrototype` set — so that when
  the body's `emit(VariableExprAST&)` looks up `x`, it finds the
  corresponding argument's `Value`.
- **[5] Emit the body and finish.** `emitExpr(*fn.getBody())` emits the whole
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
  retried, while a `def` that *succeeded* is permanent. This is the
  "nothing half-built is left in the module" promise in `codegen.h`'s
  doc comment, and the unit tests check it.

One upstream behavior is kept deliberately — **the documented signature
bug**. If a function was first declared `extern foo(a);` and is then defined
as `def foo(b) b;`, the *earlier* declaration wins (the module lookup finds
it before `emitPrototype()` runs), so the body is codegen'd against argument
name `a` — and `b` hits "Unknown variable name". The tutorial points this
out and leaves the fix as an exercise; this refactor keeps the bug. (One
upstream behavior is *not* kept: the whole-module dump at exit — see
[Deviations from upstream](#deviations-from-upstream).)

Finally, the thin public methods — the facade forwards, nothing more:

**`Chapter3/src/codegen.cpp`**
```cpp
CodeGenSession::CodeGenSession() : impl(std::make_unique<Impl>()) {}

// Out of line so that Impl is a complete type where unique_ptr<Impl> is destroyed.
CodeGenSession::~CodeGenSession() = default;

llvm::Function *CodeGenSession::emitPrototype(PrototypeAST &proto) {
  return impl->emitPrototype(proto);
}

llvm::Function *CodeGenSession::emitFunction(FunctionAST &fn) {
  return impl->emitFunction(fn);
}

llvm::Module &CodeGenSession::currentModule() { return *impl->theModule; }
```

## The driver: Printing IR

Upstream's "Top-Level parsing and JIT Driver" section is a set of free
functions (`HandleDefinition`, `HandleExtern`, `HandleTopLevelExpression`,
`MainLoop`) that call the parser, then `codegen()`, then print. In Chapter2
they are already the methods of `Driver`, the class that owns the lexer and
parser and runs the loop. This chapter gives that class its second stage —
one new member, one new include:

**`Chapter3/include/driver.h`**
```cpp
class Driver {
public:
    Driver() : parser(lexer) {}

    /// top ::= definition | external | toplevelexpr | ';'
    void mainLoop();

private:
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

    // Declaration order matters: the parser holds a reference to the lexer.
    Lexer lexer;
    Parser parser;
    CodeGenSession codegen;
};
```

The driver is the **composition root**: the one object that owns an instance
of every stage and therefore the only place that knows the pipeline's order.
`Parser` knows nothing about `CodeGenSession`; `CodeGenSession` knows nothing
about `Parser`; both know the AST. `mainLoop()` is byte-for-byte Chapter2's
— the dispatch on the current token has nothing to do with what happens to
a parsed item afterwards:

**`Chapter3/src/driver.cpp`**
```cpp
void Driver::mainLoop() {
    parser.getNextToken(); // Bootstrap the first token
    while (true) {
        fprintf(stderr, "ready> ");
        switch (parser.getCurToken()) {
        case tok_eof: return;
        case ';':     parser.getNextToken(); break;  // ignore top-level semicolons.
        case tok_def: handleDefinition(); break;
        case tok_extern: handleExtern(); break;
        default:      handleTopLevelExpression(); break;
        }
    }
}
```

Each `handle*` now generates and prints; note the top-level case still
deletes the anonymous function after showing it — it has served its purpose,
and the next top-level expression will create a fresh `__anon_expr` (in
Chapter 4, that anonymous wrapper becomes exactly what the JIT looks up and
executes):

**`Chapter3/src/driver.cpp`**
```cpp
void Driver::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = parser.parseTopLevelExpr()) {
    if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      fprintf(stderr, "Read top-level expression:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
      codegen.eraseFunction(FnIR);   // Remove the anonymous expression.
    }
  } else {
    parser.getNextToken();       // error recovery, as in Chapter2
  }
}
```

`driver.cpp` is the one consumer that includes `llvm/IR/Function.h` — it
needs the complete type to call `print()` on the result. Removing the
anonymous function, on the other hand, goes back through the session:
`eraseFunction()` exists because the module is the session's, and from
Chapter 4 on the session keeps per-function state (cached analyses) that
must go with it — a driver that called `eraseFromParent()` itself would
leave that state dangling. That is the intended shape: LLVM headers appear in `codegen.cpp`
(which does the work) and in the driver (which shows the result), and nowhere
in the frontend. `main.cpp` is Chapter2's, unchanged.

## File-by-file: What changed from Chapter2

Chapter 3 adds new files and touches some existing ones; several files carry
the same name as in Chapter2 but are NOT all identical. The exact split
(established with `diff -rq ../Chapter2 .`):

**New files**

| File | Purpose |
| --- | --- |
| `include/codegen.h` | `CodeGenSession` — the public facade over the tutorial's codegen globals and `codegen()` methods. Forward-declares `llvm::Function`/`llvm::Module` only. |
| `src/codegen.cpp` | `CodeGenSession::Impl`: the four codegen globals as members, the `emitExpr()` dispatch, and the bodies of the tutorial's "Expression Code Generation" and "Function Code Generation" sections as `emit*()` methods. |
| `test/codegen_test.cpp` | Unit tests for codegen through the public API (see Tests). |
| `test/filecheck/codegen.k`, `codegen-error.k` | Replace Chapter2's `parse.k`/`parse-error.k` — the driver output to check is now IR, not "Parsed a ..." lines. |

**Same filename, byte-identical** — safe to skip when reading: the entire
frontend and its tests — `include/ast.h`, `include/lexer.h`,
`include/parser.h`, `src/lexer.cpp`, `src/parser.cpp`, `src/main.cpp`,
`test/lexer_test.cpp`, `test/parser_test.cpp` — plus `test/filecheck/lit.cfg`
and `build.sh`. This chapter adds a stage after the parser; nothing before
that point changes.

**Same filename, modified** — before → after:

`Chapter3/include/driver.h` — one include and one member:

```cpp
// Chapter2                          // Chapter3
#include "lexer.h"                    #include "codegen.h"     // NEW
#include "parser.h"                   #include "lexer.h"
                                      #include "parser.h"
class Driver {                        class Driver {
  ...                                   ...
  Lexer lexer;                          Lexer lexer;
  Parser parser;                        Parser parser;
};                                      CodeGenSession codegen;  // NEW
                                      };
```

`Chapter3/include/log.h` + `src/log.cpp` — a third helper for the new failure
domain: `logErrorV` (upstream's `LogErrorV`) reports and returns a null
`llvm::Value*` the way `logError` returns a null AST node. The header only
forward-declares `llvm::Value`:

```cpp
// Chapter2                                      // Chapter3
                                                  namespace llvm { class Value; }        // NEW
std::unique_ptr<ExprAST> logError(...);           std::unique_ptr<ExprAST> logError(...);
std::unique_ptr<PrototypeAST> logErrorP(...);     std::unique_ptr<PrototypeAST> logErrorP(...);
                                                  llvm::Value *logErrorV(const char *str);  // NEW
```

`Chapter3/src/driver.cpp` — `mainLoop()` is identical apart from a note that
an unreachable end-of-loop module dump was removed; the three `handle*`
methods go from "report the parse" to "codegen and print the IR" (and the
top-level case erases the anonymous function after printing):

```cpp
// Chapter2                               // Chapter3
void Driver::handleDefinition() {          void Driver::handleDefinition() {
  if (parser.parseDefinition()) {            if (auto FnAST = parser.parseDefinition()) {
    fprintf(stderr,                            if (auto *FnIR = codegen.emitFunction(*FnAST)) {
      "Parsed a function definition.\n");        fprintf(stderr, "Read function definition:\n");
  } else {                                       FnIR->print(llvm::errs());
    parser.getNextToken(); // error recovery      fprintf(stderr, "\n");
  }                                            }
}                                            } else {
                                               parser.getNextToken(); // error recovery (unchanged)
                                             }
                                           }
```

`Chapter3/CMakeLists.txt` — adds `src/codegen.cpp` and `src/driver.cpp` to
`toy_core`, registers the `codegen_test` executable, and — new this chapter —
finds LLVM, links the `core` component, and pins the macOS deployment target
to match the Homebrew LLVM libraries (Chapter2, like upstream's Chapter 2,
has no LLVM build dependency at all).

`Chapter3/cmd.txt` — new demo input matching the chapter (functions worth
looking at as IR).

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter
departs from it in two places, both collected here so the main narrative
above can follow the tutorial's order undisturbed: one structural, invisible
in the generated IR, and one that changes what the session prints at exit.
(The chapter's other architectural moves — the `CodeGenSession` facade, the
driver as composition root — only repackage the tutorial's globals and free
functions and are covered by the design notes in the introduction.)

### Codegen on the AST: Kind tags instead of a virtual `codegen()`

Upstream opens Chapter 3 by adding a pure-virtual `codegen()` to the AST
hierarchy, so every node emits its own IR:

```cpp
// upstream (LangImpl03)
class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

class NumberExprAST : public ExprAST {
  double Val;
public:
  NumberExprAST(double Val) : Val(Val) {}
  Value *codegen() override;
};
```

The tutorial is upfront about the trade: it "won't dwell on good software
engineering practices", and "adding a virtual method is simplest". Here the
AST never gains that virtual — `ast.h` is Chapter2's file, unchanged — and
the walk lives in the consumer, dispatching on the kind tag (the full
function is in [The codegen component](#the-codegen-component-codegensession-and-its-impl)
above):

**`Chapter3/src/codegen.cpp`**
```cpp
  llvm::Value *emitExpr(ExprAST &expr) {
    switch (expr.getKind()) {
    case ExprAST::Expr_Num:
      return emit(llvm::cast<NumberExprAST>(expr));
    // ... one case per kind
    }
    llvm_unreachable("unknown expression kind");
  }
```

The two dispatch paths, side by side:

```
 upstream                                   refactored
 ────────                                   ──────────
 HandleDefinition()                         Driver::handleDefinition()
   └─▶ FnAST->codegen()                       └─▶ codegen.emitFunction(*fnAST)
         └─▶ Body->codegen()                        └─▶ Impl::emitExpr(*fn.getBody())
               │ virtual call                             │ switch (expr.getKind())
               ▼ vtable slot in the node                  ▼ cast<> to the concrete node
       BinaryExprAST::codegen()                      Impl::emit(BinaryExprAST &)
               │                                           │
               ├── LHS->codegen()                          ├── emitExpr(*bin.getLHS())
               └── RHS->codegen()                          └── emitExpr(*bin.getRHS())

 ast.h ──includes──▶ llvm/IR/Value.h        ast.h ──includes──▶ <memory>, <string>, ...
      AST depends on the backend                  backend depends on the AST
```

Step by step, what the change buys:

1. **The dependency edge flips.** A `Value *codegen()` declaration forces
   every AST header to know LLVM's types, so the lexer, the parser and their
   tests all pull LLVM in transitively — and the frontend's data structure
   ends up naming the backend it happens to have. With the walk outside,
   `ast.h` includes only the standard library, and the arrows run one way:
   `Driver → Parser → AST` and `Driver → CodeGenSession → AST`.
2. **New consumers stop editing the nodes.** Adding a second thing that
   reads the tree — a pretty-printer, a type checker, a second backend —
   means a new virtual on all six classes upstream, versus a new walker
   here. This is where real compilers land: clang's AST is walked by
   `CodeGenFunction`, by `Sema` and by the static analyzer, none of which are
   methods on the nodes, and the MLIR Toy tutorial's `MLIRGenImpl` does the
   same. The tag-plus-`switch` is the lightweight cousin of the visitor
   pattern: same dispatch, none of the double-indirection boilerplate.
3. **The compiler checks the dispatch table.** The `switch` lists every
   enumerator and has no `default:`, so `-Wswitch` flags each walker that
   forgets a case when Chapter 5 and Chapter 7 add node kinds. Upstream gets
   the mirror-image guarantee from the pure virtual (a node that forgets
   `codegen()` stays abstract), so neither scheme can silently lose a kind —
   but only one of them keeps the check where the walk is.
4. **The emitted code is the tutorial's.** Each `emit()` overload is the
   corresponding `codegen()` body line for line, with the globals renamed to
   `Impl` members and `LHS->codegen()` rewritten as
   `emitExpr(*bin.getLHS())`. Nothing about the IR depends on how the
   recursion is reached.

|                            | upstream                                    | refactored                                        |
| -------------------------- | ------------------------------------------- | ------------------------------------------------- |
| printed IR for any input   | identical                                   | identical                                         |
| per-node dispatch          | one indirect call through the vtable        | one `switch` plus an asserting `cast<>`           |
| `ast.h` dependencies       | `llvm/IR/Value.h` (whole frontend gets LLVM) | standard library only                            |
| adding a tree consumer     | a virtual on all six AST classes            | a new walker; AST untouched                       |
| adding a node kind         | missing override → class stays abstract     | missing case → `-Wswitch` warning in each walker  |

### No whole-module dump at exit

Upstream's `main()` prints the entire module once the REPL loop has ended:

```cpp
// upstream (LangImpl03)
int main() {
  ...
  // Run the main "interpreter loop" now.
  MainLoop();

  // Print out all of the generated code.
  TheModule->print(errs(), nullptr);

  return 0;
}
```

Here `main()` only constructs the `Driver` and runs `mainLoop()`; nothing is
printed afterwards:

**`Chapter3/src/main.cpp`**
```cpp
int main() {
    toy::Driver driver;

    // Run the main "interpreter loop" now.
    driver.mainLoop();

    return 0;
}
```

Why: every `def` and `extern` is already printed the moment it is generated
(and every top-level expression is printed and then erased), so the exit
dump would repeat the whole session's IR at the end — and for piped input,
right after the last `ready>`. Dropping it keeps the transcript one
definition = one printout, which is also what the lit tests match. The
driver's `mainLoop()` carries a note where the dump used to be reachable
(it only ever ended via `return` on `tok_eof`, so code after the loop had to
move into the loop or go).

|                            | upstream                                  | refactored                |
| -------------------------- | ----------------------------------------- | ------------------------- |
| each `def` / `extern`      | printed as generated                      | printed as generated      |
| top-level expression       | printed, then erased                      | printed, then erased      |
| at `tok_eof`               | whole module printed again (defs + externs) | nothing                 |

Chapter 4 makes the question moot: each function gets its own module, which
is handed to the JIT immediately, so there is no single module left to dump
at exit.

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

Note `bar` calls *itself*: by the time the body is emitted, `bar`'s own
declaration is already in the module (created a few lines earlier in
`emitFunction`), so the recursive `getFunction("bar")` lookup succeeds —
forward-progress for free. (Don't actually *call* `bar`, though: with no
conditionals until Chapter 5 the recursion has no base case, so it would run
forever — upstream makes the same joke.)

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
  (no parser involved) and emits them through a fresh `CodeGenSession` per
  test — **only the public API**: the tests cannot reach the builder or the
  symbol table, so every expression is checked through the function that
  returns it (`def tmp(a b) a <op> b`, then inspect the entry block). They
  pin in-memory IR properties that textual matching can't express directly:
  `isa<ConstantFP>` / `isa<CallInst>` on the returned value, the folded
  value of constant expressions (`1.0 + 2.0` must *be* the constant `3.0`,
  since `IRBuilder` folds it), the argument a variable resolves to, the
  entry block and terminator of a generated function, that a definition
  fills in a prior `extern`'s declaration in place, and the
  `nullptr`-on-error contract *plus its cleanup* (unknown variable, unknown
  callee, arity mismatch, invalid operator, redefinition — and that a failed
  body leaves no function behind in `currentModule()`).
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
