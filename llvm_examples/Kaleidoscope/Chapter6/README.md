# Chapter 6 — User-defined Operators

This chapter lets Kaleidoscope programs define **their own operators** —
new binary operators with a chosen precedence, and unary operators. The
motivation: the language so far is functional but operator-poor — no
division, no logical negation, no comparison besides `<`. Rather than
hard-coding each missing operator, this chapter (a "wild digression",
upstream admits — your language, your call on what's good or bad) lets the
*user* round out the set:

```
def unary!(v)           if v then 0 else 1;    # logical not
def binary| 5 (LHS RHS) if LHS then 1 else     # logical or, precedence 5
                          (if RHS then 1 else 0);
```

This is more general than C++-style operator overloading: C++ only lets you
redefine *existing* operators — you cannot introduce new ones, choose their
precedence, or otherwise change the grammar. Kaleidoscope after this chapter
can do all three, the grammar extending itself dynamically while the JIT
runs. The payoff of that generality is the aspiration many languages have
for their standard runtimes — implementing the language *in its own
library*: `mandel.txt` defines `!`, unary `-`, `>`, `|`, `&`, `=`, and `:`
entirely in Kaleidoscope source.

Two general ideas make this nearly free to implement:

**Grammar as data.** In most compilers the operator set is frozen into the
grammar. But Chapter 2's operator-precedence parser never hard-coded any
operator — `parseBinOpRHS` consults a *precedence table* and treats any
ASCII character with a table entry as a binary operator. The grammar for
binary expressions is a `std::map`, not code; adding an operator at runtime
is inserting one row. This is the payoff of having chosen
operator-precedence parsing back in Chapter 2.

**Operators are just functions (desugaring).** A user-defined operator needs
no new codegen concept: `def binary| 5 (LHS RHS) ...` compiles to a perfectly
ordinary function named `binary|`, and every use `a | b` compiles to a call
to it. The "operator-ness" exists only in the parser (precedence, infix
position); by the time IR exists, it's calls all the way down — LLVM happily
accepts punctuation in symbol names by quoting them (`@"binary|"`). This
compile-away-the-syntax move is called **desugaring**, and it's how real
languages implement operator overloading, properties, and much else.

```
   def binary| 5 (a b) ...          a | b + c
          │                              │ parse ('|' now in the table, prec 5,
          ▼                              ▼        looser than '+' at 20)
   ordinary FunctionAST            BinaryExprAST('|', a, (b+c))
   named "binary|"                       │ codegen: '|' is not built in
          │                              ▼
          ▼                        %sum = fadd double %b, %c
   define double @"binary|"(...)   call double @"binary|"(double %a, double %sum)
```

Prerequisites: control flow from [Chapter5](../Chapter5/README.md) (the
example operators are built from `if`), JIT/optimizer from
[Chapter4](../Chapter4/README.md), parser fundamentals from
[Chapter2](../Chapter2/README.md).
Reference: [Chapter 6: Extending the Language — User-defined Operators](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl06.html).

The two ideas map onto two components, and nothing else moves. The
precedence table stays where Chapter 2 put it — in the `Parser` — and the
parser itself installs a new operator the moment it has parsed the
operator's prototype. The code generator learns one new node kind and one
fallback: an operator it does not know is a call. The public codegen header,
the driver and `main` are byte-identical to Chapter5's.

```
   Parser (parser.h/.cpp)                          CodeGenSession::Impl (codegen.cpp)
     binopPrecedence  ◀── parseDefinition() installs   emitExpr: case Expr_Unary ──▶ emit(UnaryExprAST&)
     parseUnary()          `binary<op> N` on parse        └▶ call "unary<op>"
     parsePrototype()      (erases again if the           emit(BinaryExprAST&):
       3 shapes            body fails to parse)           builtin '+','-','*','<' inline,
                                                          anything else ──▶ call "binary<op>"
```

Upstream does it differently in one respect: it installs the precedence
from *codegen* (`FunctionAST::codegen()` writes into `BinopPrecedence`),
which is why upstream's parser and codegen share that global. Keeping the
table in the parser is a deliberate deviation with a small behavioral
consequence, explained in [Deviations from upstream](#deviations-from-upstream).

(A precise file-by-file diff against Chapter5 is in
[File-by-file](#file-by-file-what-changed-from-chapter5) near the end.)

## Parsing: Operator prototypes and the `unary` layer

`parsePrototype()` now accepts three shapes, tagged by `Kind`:

```
prototype ::= id '(' id* ')'                    Kind 0: plain function
            | 'binary' LETTER number? '(' id id ')'   Kind 2: binary op
            | 'unary'  LETTER '(' id ')'              Kind 1: unary op
```

**`Chapter6/src/parser.cpp`** (the binary case)
```cpp
  case tok_binary:
    getNextToken();
    if (!isascii(curTok))
      return logErrorP("Expected binary operator");
    FnName = "binary";
    FnName += (char)curTok;          // "binary|", "binary@", ...
    Kind = 2;
    getNextToken();

    // Read the precedence if present.
    if (curTok == tok_number) {
      if (lexer.getNumVal() < 1 || lexer.getNumVal() > 100)
        return logErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = (unsigned)lexer.getNumVal();   // default is 30
      getNextToken();
    }
    break;
```

After the argument list is read, `Kind` doubles as an arity check —
`if (Kind && ArgNames.size() != Kind)` rejects a "binary" operator with one
parameter. The prototype carries the new facts along:

**`Chapter6/include/ast.h`**
```cpp
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
  // for user-defined op
  bool IsOperator;
  unsigned Precedence;  // Precedence if a binary op

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args,
               bool IsOperator = false, unsigned Prec = 0)
      : Name(Name), Args(std::move(Args)), IsOperator(IsOperator),
        Precedence(Prec) {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }

  // for user-defined op
  bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
  bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return Name[Name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return Precedence; }
};
```

`isUnaryOp()` / `isBinaryOp()` are arity-based, and `getOperatorName()` is
just the last character of the name — `"binary|"` → `'|'`. The defaults keep
every existing `PrototypeAST(name, args)` call site working unchanged.

### Installing the operator: `parseDefinition()`

This is where the grammar extends itself. Once the prototype of a
definition is in hand, the parser knows everything the precedence table
needs — the character and the number — and writes the row *before* parsing
the body:

**`Chapter6/src/parser.cpp`**
```cpp
// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> Parser::parseDefinition() {
    getNextToken(); // eat def
    auto proto = parsePrototype();
    if (!proto) return nullptr;

    // A user-defined binary operator becomes part of the grammar as soon as its
    // prototype is parsed -- before the body, which may use it recursively, and
    // before any subsequent input. (Upstream installs it in FunctionAST::codegen.)
    bool installedOp = false;
    int previousPrec = -1;  // -1: the operator was not in the table before
    if (proto->isBinaryOp()) {
        char op = proto->getOperatorName();
        auto it = binopPrecedence.find(op);
        if (it != binopPrecedence.end()) previousPrec = it->second;
        binopPrecedence[op] = proto->getBinaryPrecedence();
        installedOp = true;
    }

    auto e = parseExpression();
    if (!e) {
        // The operator was never really (re)defined: put the table back the way
        // it was, so later input does not parse against a function that will
        // not exist -- or against a precedence that never took effect.
        if (installedOp) {
            char op = proto->getOperatorName();
            if (previousPrec < 0) binopPrecedence.erase(op);
            else binopPrecedence[op] = previousPrec;
        }
        return nullptr;
    }
    return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
}
```

Two consequences of installing here rather than after codegen. First, the
operator is usable **inside its own body** — `def binary| 5 (a b) a | b`
parses (and codegens to a recursive call), which is the natural thing for a
definition to allow. Second, the table is kept honest by the parser alone:
if the body fails to *parse*, the row is put back the way it was — removed
if the operator was new, restored to its previous precedence if this was a
redefinition — so a stray `|` on the next line is an unknown *unary*
operator (an ordinary error) rather than a binary operator with no function
behind it, and a failed redefinition does not silently change how an
existing operator binds. The
table never leaves the parser, and codegen never touches it —
`getTokPrecedence()` is exactly Chapter 2's.

### The `unary` layer

Unary operators need one real grammar change. Until now the operand of a
binary operator was a `primary`; now there is a layer in between:

```
expression ::= unary binoprhs
binoprhs   ::= (binop unary)*
unary      ::= primary | <op> unary
```

**`Chapter6/src/parser.cpp`**
```cpp
std::unique_ptr<ExprAST> Parser::parseUnary() {
  // If the current token is not an operator, it must be a primary expr.
  if (!isascii(curTok) || curTok == '(' || curTok == ',')
    return parsePrimary();

  // If this is a unary operator, read it.
  int Opc = curTok;
  getNextToken();
  if (auto Operand = parseUnary())        // recursion: !!x works
    return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
  return nullptr;
}
```

Anything ASCII that isn't `(` or `,` is *presumed* to be a unary operator.
Note what is *absent* here: precedence. Unary operators cannot parse
ambiguously the way binary ones can — a prefix chain like `!!x` has exactly
one reading — so there is no table and no climbing, just recursion.
A behavioral consequence worth knowing: inputs like `+10` or `10 ++ 5` used
to be parse errors and now **parse fine** (as applications of a unary `+`) —
they only fail later, at codegen, if no `unary+` was defined. Two parser
tests flipped from must-fail to must-pass because of this.

The node is the smallest in the AST — an opcode and one operand, with the
kind tag `Expr_Unary`:

**`Chapter6/include/ast.h`**
```cpp
/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
      : ExprAST(Expr_Unary), Opcode(Opcode), Operand(std::move(Operand)) {}

  char getOpcode() const { return Opcode; }
  ExprAST *getOperand() const { return Operand.get(); }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Unary; }
};
```

## Codegen: Fall back to a call

The dispatch in `emitExpr()` grows its one new case, and the `emit()`
overload for it is nothing but the desugaring:

**`Chapter6/src/codegen.cpp`**
```cpp
  llvm::Value *emit(UnaryExprAST &unary) {
    llvm::Value *OperandV = emitExpr(*unary.getOperand());
    if (!OperandV)
      return nullptr;

    llvm::Function *F = getFunction(std::string("unary") + unary.getOpcode());
    if (!F)
      return logErrorV("Unknown unary operator");

    return builder->CreateCall(F, OperandV, "unop");
  }
```

It is simpler than the binary version for one reason: there are no
*built-in* unary operators to special-case, so the call is the whole story.
`getFunction()` is Chapter 4's registry-aware lookup, so an operator defined
three modules ago is re-declared into this one exactly like any other
function. And it fails softly with `logErrorV` — an undefined unary operator
is reachable from ordinary input, since `parseUnary` presumes any operator
character is unary.

`emit(BinaryExprAST&)` keeps the four built-ins inline and adds a
fallthrough — the `default:` that used to be an error now `break`s into:

**`Chapter6/src/codegen.cpp`**
```cpp
    // If it wasn't a builtin binary operator, it must be a user defined one. Emit
    // a call to it. (Upstream asserts here; we report, since a definition whose
    // body failed can leave the parser accepting an operator no function backs.)
    llvm::Function *F = getFunction(std::string("binary") + bin.getOp());
    if (!F)
      return logErrorV("Unknown binary operator");

    llvm::Value *Ops[] = {L, R};
    return builder->CreateCall(F, Ops, "binop");
```

Upstream has an `assert(F && "binary operator not found!")` here, justified
by the invariant that a character only reaches this point if its definition
codegen'd. Here the invariant is weaker on purpose — see
[Deviations from upstream](#deviations-from-upstream) — so the fallback
reports like every other codegen error and the REPL carries on. That is the
whole of the codegen change: no state, no table, no registration. The
"language extension as a runtime side effect" loop the tutorial describes —
parse a definition, and *the next line of input parses differently* — runs
entirely inside the parser.

## The payoff: A Mandelbrot set

With `!`, `-` (unary), `>`, `|`, `&`, `=`, `:` defined *in Kaleidoscope
itself* (see `mandel.txt` — e.g. the sequencing operator
`def binary: 1 (x y) y;` whose whole point is precedence 1, executing both
sides for effect), plus
`putchard`/`printd` from Chapter5, the language is expressive enough for the
tutorial's showcase — an ASCII Mandelbrot renderer. `mandel.txt` builds it
in three steps.

**A pixel.** `printdensity` maps a value to a character whose visual
"density" reflects it — the lower the value, the denser the glyph:

**`Chapter6/mandel.txt`**
```
def printdensity(d)
  if d > 8 then
    putchard(32) # ' '
  else if d > 4 then
    putchard(46) # '.'
  else if d > 2 then
    putchard(43) # '+'
  else
    putchard(42); # '*'
```

so `printdensity(1): printdensity(2): printdensity(3): printdensity(4):
printdensity(5): printdensity(9): putchard(10);` prints `**++.` followed by
a space and a newline — a one-liner built entirely from this chapter's
user-defined `>` and `:`.

**The math.** A point *c* of the complex plane belongs to the Mandelbrot
set if iterating *z = z² + c* from *z = 0* never diverges.
`mandelconverger` counts how many iterations it takes to escape
(|z|² > 4), saturating at 255; complex arithmetic is spelled out in doubles
(`real·real − imag·imag` and `2·real·imag`), the escape test is this
chapter's `|` and `>`, and recursion is the loop:

**`Chapter6/mandel.txt`**
```
# Determine whether the specific location diverges.
# Solve for z = z^2 + c in the complex plane.
def mandelconverger(real imag iters creal cimag)
  if iters > 255 | (real*real + imag*imag > 4) then
    iters
  else
    mandelconverger(real*real - imag*imag + creal,
                    2*real*imag + cimag,
                    iters+1, creal, cimag);

# Return the number of iterations required for the iteration to escape
def mandelconverge(real imag)
  mandelconverger(real, imag, 0, real, imag);
```

Plotting that iteration count over a 2-D window *is* the Mandelbrot set.

**The plot.** Two nested Chapter5 `for` loops sweep the window,
`printdensity(mandelconverge(x,y))` draws each cell, the sequencing `:`
tacks a newline onto each row, and `mandel` converts "start +
magnification" into ranges sized for a terminal (78 columns × 40 rows):

**`Chapter6/mandel.txt`**
```
def mandelhelp(xmin xmax xstep ymin ymax ystep)
  for y = ymin, y < ymax, ystep in (
    (for x = xmin, x < xmax, xstep in
       printdensity(mandelconverge(x,y)))
    : putchard(10)
  )

# mandel - This is a convenient helper function for plotting the mandelbrot set
# from the specified position with the specified Magnification.
def mandel(realstart imagstart realmag imagmag)
  mandelhelp(realstart, realstart+realmag*78, realmag,
  	     imagstart, imagstart+imagmag*40, imagmag);
```

`mandel.txt` ends by rendering three windows —
`mandel(-2.3, -1.3, 0.05, 0.07)` (the whole set),
`mandel(-2, -1, 0.02, 0.04)`, and `mandel(-0.9, -1.4, 0.02, 0.03)` (two
closer views). `./run.sh` renders all of them; a slice of the first:

```
**+++++++++++++++++++++++++....                ...++++++++++++++++*************
*+++++++++++++++++++++++.......                ....++++++++++++++++************
+++++++++++++++++++++..........                .....++++++++++++++++***********
++++++++++++++++++.............                .......+++++++++++++++**********
+++++++++++++++................                ............++++++++++**********
+++++++++++++.................                  .................+++++*********
+++++++++++...       ....                            ..........  .+++++********
++++++++++.....                                       ........  ...+++++*******
++++++++......                                                   ..++++++******
```

(Upstream's closing joke: Kaleidoscope may not be self-similar, but it can
plot things that are.) One capability is still conspicuously missing — a
Kaleidoscope program can call side-effecting functions but cannot *define
or mutate a variable* of its own. That is the next chapter's subject:
[Chapter7](../Chapter7/README.md) adds mutation *without* bolting an "SSA
construction" phase onto the frontend.

## File-by-file: What changed from Chapter5

The exact split (established with `diff -rq ../Chapter5 .`):

**New files**

| File | Purpose |
| --- | --- |
| `mandel.txt` | The chapter's operator library + Mandelbrot demo, in Kaleidoscope. |
| `run.sh` | Runs `cmd.txt` and `mandel.txt` through the built `toy`. |
| `require.txt` | Note: `view_cfg` needs graphviz/xdot installed. |
| `test/filecheck/userops.k` | End-to-end operator checks (see Tests). |

**Same filename, byte-identical** — safe to skip when reading:
`include/codegen.h`, `include/driver.h`, `src/driver.cpp`, `src/main.cpp`
(the facade, the driver and `main` know nothing about operators),
`include/log.h`, `src/log.cpp`, `src/extern_d.cpp`, `CMakeLists.txt`,
`view_cfg/*`, `test/filecheck/opt.k`, `test/filecheck/controlflow.k`,
`test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter6/include/lexer.h` / `src/lexer.cpp` — two new tokens,
`tok_binary = -11` and `tok_unary = -12`, with the matching keyword checks
in `gettok()`.

`Chapter6/include/ast.h` — one new kind tag and node, and the prototype
grows the operator metadata:

```cpp
// Chapter5                              // Chapter6
enum ExprASTKind { ..., Expr_For };       enum ExprASTKind { ..., Expr_For, Expr_Unary };
                                          class UnaryExprAST : public ExprAST { ... };   // NEW

PrototypeAST(const std::string &Name,     PrototypeAST(const std::string &Name,
             std::vector<std::string>                  std::vector<std::string> Args,
             Args)                                     bool IsOperator = false,
    : Name(Name),                                      unsigned Prec = 0)
      Args(std::move(Args)) {}                : ... IsOperator(IsOperator),
                                                    Precedence(Prec) {}
                                          bool isUnaryOp()  const;  // arity == 1
                                          bool isBinaryOp() const;  // arity == 2
                                          char getOperatorName() const; // last char
                                          unsigned getBinaryPrecedence() const;
```

`Chapter6/include/parser.h` — `parseUnary()` declared; the precedence table
keeps its owner and gains a comment saying who writes it now.

`Chapter6/src/parser.cpp` — `parseUnary()` added;
`parseExpression()`/`parseBinOpRHS()` call it where they called
`parsePrimary()`; `parsePrototype()` rewritten as the three-case switch
above; `parseDefinition()` installs (and on parse failure erases) the
operator's precedence:

```cpp
// Chapter5                                   // Chapter6
auto proto = parsePrototype();                 auto proto = parsePrototype();
if (!proto) return nullptr;                    if (!proto) return nullptr;
                                               bool installedOp = false;
                                               if (proto->isBinaryOp()) {            // NEW
                                                 binopPrecedence[op] = prec;
                                                 installedOp = true;
                                               }
if (auto e = parseExpression())                auto e = parseExpression();
  return make_unique<FunctionAST>(...);        if (!e) {
return nullptr;                                  if (installedOp) /* erase, or restore old row */;
                                                 return nullptr;
                                               }
                                               return make_unique<FunctionAST>(...);
```

`Chapter6/src/codegen.cpp` — one new `case` in `emitExpr()`,
`emit(UnaryExprAST&)` added, and `emit(BinaryExprAST&)`'s `default:` goes
from `return logErrorV("invalid binary operator")` to `break` + the
user-operator fallthrough. `diff` shows exactly one removed line.

`Chapter6/build.sh` — no longer pipes `cmd.txt` automatically; running the
demos moved to the new `run.sh`.

`Chapter6/cmd.txt` — new demo: a sequencing operator
`def binary : 1 (x y) 0;` chaining `printd` calls.

`Chapter6/test/lexer_test.cpp` — `binary`/`unary` must lex as keywords, and
the operator characters (`!`, `@`, `>`, `|`, `&`) as plain ASCII tokens.

`Chapter6/test/parser_test.cpp` — new `ParseUnaryExprTest` (nesting `!!x`,
missing operand fails); prototype cases for operator forms including the
1..100 precedence validation and the operand-count check; definition cases
including an operator used in its own body; the two flipped expectations
noted above (`+ 10`, `10 ++ 5` now parse); and two tree-shape tests —
`UserOperatorChangesLaterParse` parses `def binary| 5 (a b) a;` and then
checks that the *same* parser reads `a | b + c` as `a | (b + c)`, and
`UnaryTree` checks `!!x` nests.

`Chapter6/test/codegen_test.cpp` — four operator cases through the facade:
a unary and a user binary operator each desugar to a call of the declared
function, an unknown unary operator is an error, and a builtin stays inline
even when a `binary+` is declared. The `'?'`-is-invalid case is kept (see
Deviations).

`Chapter6/test/jit_test.cpp` — `JITCustomOperatorParamTest`: one `def`, then
an expression parsed by the same parser and run natively — a binary and a
unary operator, loose vs. tight precedence against `+` (same text shape,
different results: `6.0` vs `7.0`), and an operator used recursively in
its own body.

`Chapter6/test/filecheck/jit.k` — carried forward from Chapter5 and
extended: a user-defined `binary%` (precedence 40, `a - b`) is defined and
then `10 % 3;` must print `Evaluated to 7.000000` — the full
define → install-precedence → reparse → JIT → execute loop in two lines.

## Deviations from upstream

Chapter2–9 keep the tutorial's behavior, quirks included. This chapter
departs from it in two related places, collected here so the main narrative
above can follow the tutorial's order. Neither changes what any of the
chapter's inputs print — `cmd.txt`, `mandel.txt` and every `.k` file
produce byte-identical output — but both change what happens on *bad*
input.

### The precedence table stays in the parser

Upstream installs a new operator's precedence from codegen:

```cpp
// upstream (LangImpl06)
Function *FunctionAST::codegen() {
  // Transfer ownership of the prototype to the FunctionProtos map, but keep a
  // reference to it for use below.
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  Function *TheFunction = getFunction(P.getName());
  if (!TheFunction)
    return nullptr;

  // If this is an operator, install it.
  if (P.isBinaryOp())
    BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();
  ...
```

— which is why the earlier version of this chapter moved the table from the
parser into the codegen state: the map had to follow its writer. Here the
writer is the parser itself, in `parseDefinition()`, as shown above:

**`Chapter6/src/parser.cpp`** (abridged)
```cpp
    bool installedOp = false;
    int previousPrec = -1;
    if (proto->isBinaryOp()) {
        ...remember the old row, if any...
        binopPrecedence[op] = proto->getBinaryPrecedence();
        installedOp = true;
    }
    auto e = parseExpression();
    if (!e) {
        if (installedOp) ...erase the row, or restore the old one...
        return nullptr;
    }
```

Why: the precedence of an operator is a *grammar* fact — it determines how
tokens group — and the parser is the component that owns the grammar. The
code generator does not need to know that `|` binds looser than `+`; it
only needs to know that `|` is not one of its four builtins. Routing the
information through codegen forces the parser and the code generator to
share state, which is exactly the coupling the facade exists to avoid
(`codegen.h` would otherwise need to expose the table). It also fixes a
timing oddity: upstream installs the row only after the whole definition —
prototype *and* body — has been parsed, so the operator is not available
inside its own body.

|                                          | upstream (installed by codegen)                 | here (installed by the parser)                  |
| ---------------------------------------- | ----------------------------------------------- | ----------------------------------------------- |
| `def binary\| 5 (a b) a \| b`            | body fails to parse: `\|` is not yet an operator | parses; the body is a recursive call            |
| definition parses, body fails to codegen | row stays installed                              | row stays installed                              |
| definition's body fails to *parse*       | row never installed / old row untouched          | row installed then erased or restored — same net effect |
| who owns `binopPrecedence`               | codegen state (shared global)                    | `Parser`                                         |

### An unknown binary operator is an error, not an assertion

Upstream ends `BinaryExprAST::codegen()` with:

```cpp
// upstream (LangImpl06)
  // If it wasn't a builtin binary operator, it must be a user defined one. Emit
  // a call to it.
  Function *F = getFunction(std::string("binary") + Op);
  assert(F && "binary operator not found!");
```

Here the same spot reports and returns null:

**`Chapter6/src/codegen.cpp`**
```cpp
    llvm::Function *F = getFunction(std::string("binary") + bin.getOp());
    if (!F)
      return logErrorV("Unknown binary operator");
```

Why: the assertion encodes an invariant — "if the parser accepted the
character as a binary operator, its function exists" — that does not hold
in either design once a definition's *body fails to codegen*. Upstream
leaves the precedence row installed in that case (see the table above), so
the next use of the operator parses fine and reaches this line with no
function to call: an abort in a debug build, undefined behavior in release.
With the parser owning the table the row stays installed too, but the
failed definition is never registered (Chapter 4's register-on-success
rule), so `getFunction()` returns null and the user sees
`Error: Unknown binary operator` — the REPL continues. It also means the
`'?'`-is-invalid case in `codegen_test.cpp`, which upstream's assertion
made untestable, is back.

|                                             | upstream                                   | here                                         |
| ------------------------------------------- | ------------------------------------------ | -------------------------------------------- |
| `1 ? 2` with no `binary?` defined           | `1` evaluates; `? 2` is `Error: Unknown unary operator` | same (the unary layer, not the table, claims `?`) |
| `def binary% 40 (a b) y;` then `10 % 3;`    | abort (debug) / UB (release)                | `Error: Unknown binary operator`, REPL continues |
| a defined operator                          | call                                        | call                                         |

## Build and run

`./build.sh` to build, then `./run.sh` for both demos (or pipe by hand). The
`cmd.txt` session shows sequencing via a user-defined `:` operator:

```
ready> def binary : 1 (x y) 0;   # low precedence, ignores operands
Read function definition:
define double @"binary:"(double %x, double %y) {
entry:
  ret double 0.000000e+00
}

ready> printd(123) : printd(456);
Read top-level expression:
define double @__anon_expr() {
entry:
  %calltmp = call double @printd(double 1.230000e+02)
  %calltmp1 = call double @printd(double 4.560000e+02)
  %binop = call double @"binary:"(double %calltmp, double %calltmp1)
  ret double %binop
}

123.000000
456.000000
Evaluated to 0.000000
```

Note the quoted symbol `@"binary:"` — LLVM symbol names may contain any
character (upstream notes even embedded NULs are legal); the printed form
just needs quoting.

## Tests

Same two-scheme setup — rationale and lit mechanics in the [top-level
README](../README.md#testing-the-two-schemes). What Chapter 6 adds:

- **`test/filecheck/userops.k`** — the star of this chapter, because the key
  property is *stateful*: defining an operator changes how **subsequent**
  input parses, which only an end-to-end run can observe. It defines
  `binary|` at precedence 5 (looser than `+`) and checks the `fadd` feeds
  the call (`a | b + c` ⇒ `a | (b + c)`), then defines `binary@` at 50
  (tighter) and checks the call feeds the `fadd`
  (`a @ b + c` ⇒ `(a @ b) + c`). Same source text shape, opposite tree —
  purely because of the precedence in the *earlier* definition.
- **`parser_test.cpp` / `lexer_test.cpp`** — grammar-level pinning: unary
  nesting, operator-prototype forms, precedence range validation, arity
  validation, the two deliberate must-pass flips, and — now that the parser
  owns the table — the stateful property at the unit level too:
  `UserOperatorChangesLaterParse` shows one `Parser` instance reading
  `a | b + c` differently after it has parsed `def binary| 5 ...`.
- **`codegen_test.cpp`** — the desugaring at the API level: a unary and a
  user binary operator become calls to the declared `unary<op>` /
  `binary<op>` function, a builtin stays an `fadd` even if a `binary+`
  exists, and both "unknown operator" paths return null instead of
  asserting.
- **`jit_test.cpp`** — `JITCustomOperatorParamTest` runs define → parse →
  JIT → call for a table of operators, including the loose-vs-tight
  precedence pair (`1 @ 2 + 3` is `6.0` at precedence 5 and would be `7.0`
  for `2 @ 3 + 1` at precedence 50) and an operator calling itself.
- **`test/filecheck/jit.k`** (carried from Chapter5) — the evaluate-loop and
  error-recovery checks, plus this chapter's addition: `def binary% 40 (a b)
  a - b;` then `10 % 3;` → `Evaluated to 7.000000`, executing a user-defined
  operator end-to-end (`userops.k` checks the IR; this checks the run).

```sh
ctest --test-dir build                # everything
lit -v test/filecheck/userops.k       # operator precedence end-to-end
./build/jit_test                      # includes the custom-operator table
./run.sh                              # demos incl. the Mandelbrot render
```
