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

One refactor-specific structural change: the precedence table
`binopPrecedence` **moves from `Parser` to `IRGenContext`**. Installing a new
operator happens in `FunctionAST::codegen()` (see below), which only has the
context — so the map's owner had to follow its writers.

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
parameter. The prototype carries the new facts along
(`PrototypeAST` gains `IsOperator` and `Precedence`, plus the helpers
`isUnaryOp()` / `isBinaryOp()` — arity-based — and `getOperatorName()`,
which is just the last character of the name).

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

## Codegen: Fall back to a call

`UnaryExprAST::codegen()` is nothing but the desugaring:

**`Chapter6/src/codegen.cpp`**
```cpp
llvm::Value *UnaryExprAST::codegen(IRGenContext &ctx) {
  llvm::Value *OperandV = Operand->codegen(ctx);
  if (!OperandV)
    return nullptr;

  llvm::Function *F = getFunction(std::string("unary") + Opcode, ctx);
  if (!F)
    return logErrorV("Unknown unary operator");

  return ctx.builder->CreateCall(F, OperandV, "unop");
}
```

It is simpler than the binary version for one reason: there are no
*built-in* unary operators to special-case, so the call is the whole story.
(Note it also fails softly with `logErrorV` — an undefined unary operator
is reachable from ordinary input, since `parseUnary` presumes any operator
character is unary.)

`BinaryExprAST::codegen()` keeps the four built-ins inline and adds a
fallthrough — the `default:` that used to be an error now `break`s into:

**`Chapter6/src/codegen.cpp`**
```cpp
  // If it wasn't a builtin binary operator, it must be a user defined one. Emit
  // a call to it.
  llvm::Function *F = getFunction(std::string("binary") + Op, ctx);
  assert(F && "binary operator not found!");

  llvm::Value *Ops[] = {L, R};
  return ctx.builder->CreateCall(F, Ops, "binop");
```

Note the `assert` — an upstream quirk kept deliberately. It is justified
upstream by the invariant that the parser only *classifies* a character as a
binary operator if it's in the precedence table, and it only gets into the
table when its definition codegens. But in a release build an undefined
operator here is UB rather than a clean error (the earlier chapters'
`logErrorV` was strictly safer); this is flagged for the v2 refactor.

The registration itself happens in `FunctionAST::codegen()` — a definition's
side effect on the *parser's* future behavior:

**`Chapter6/src/codegen.cpp`**
```cpp
  // If this is an operator, install it.
  if (P.isBinaryOp())
    ctx.binopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();
  ...
  // If the body failed, the operator was never really defined; unregister its
  // precedence so a later use doesn't reach codegen with no function to call.
  if (P.isBinaryOp())
    ctx.binopPrecedence.erase(P.getOperatorName());
```

This is the interesting loop in the architecture: the REPL parses a
definition, codegen installs the precedence, and *the next line of input
parses differently than it would have before*. Language extension as a
runtime side effect.

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

**New files**

| File | Purpose |
| --- | --- |
| `mandel.txt` | The chapter's operator library + Mandelbrot demo, in Kaleidoscope. |
| `run.sh` | Runs `cmd.txt` and `mandel.txt` through the built `toy`. |
| `require.txt` | Note: `view_cfg` needs graphviz/xdot installed. |
| `test/filecheck/userops.k` | End-to-end operator checks (see Tests). |

**Same filename, byte-identical** — safe to skip when reading:
`CMakeLists.txt`, `include/log.h`, `src/log.cpp`, `src/main.cpp`,
`src/extern_d.cpp`, `view_cfg/*`, `test/filecheck/opt.k`,
`test/filecheck/controlflow.k`, `test/filecheck/lit.cfg`.

**Same filename, modified** — before → after:

`Chapter6/include/lexer.h` / `src/lexer.cpp` — two new tokens,
`tok_binary = -11` and `tok_unary = -12`, with the matching keyword checks
in `gettok()`.

`Chapter6/include/ast.h` — adds `UnaryExprAST{Opcode, Operand}`;
`PrototypeAST` grows the operator metadata:

```cpp
// Chapter5                              // Chapter6
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

`Chapter6/include/parser.h` / `include/ir_gen_ctx.h` — the precedence table
changes owner; the parser reads it through `ctx`:

```cpp
// Chapter5: Parser constructor          // Chapter6: IRGenContext constructor
Parser(...) {                             IRGenContext() {
    binopPrecedence['<'] = 10;                ...
    binopPrecedence['+'] = 20;                binopPrecedence['<'] = 10;
    binopPrecedence['-'] = 20;                binopPrecedence['+'] = 20;
    binopPrecedence['*'] = 40;                binopPrecedence['-'] = 20;
}                                             binopPrecedence['*'] = 40;
                                          }
```

`Chapter6/src/parser.cpp` — `getTokPrecedence()` reads `ctx.binopPrecedence`;
`parseUnary()` added; `parseExpression()`/`parseBinOpRHS()` call it where
they called `parsePrimary()`; `parsePrototype()` rewritten as the three-case
switch above.

`Chapter6/src/codegen.cpp` — `UnaryExprAST::codegen()` added;
`BinaryExprAST::codegen()` gains the user-operator fallthrough;
`FunctionAST::codegen()` installs (and on failure erases) the precedence.

`Chapter6/build.sh` — no longer pipes `cmd.txt` automatically; running the
demos moved to the new `run.sh`.

`Chapter6/cmd.txt` — new demo: a sequencing operator
`def binary : 1 (x y) 0;` chaining `printd` calls.

`Chapter6/test/lexer_test.cpp` — `binary`/`unary` must lex as keywords, and
the operator characters (`!`, `@`, `>`, `|`, `&`) as plain ASCII tokens.

`Chapter6/test/parser_test.cpp` — new `ParseUnaryExprTest` (nesting `!!x`,
missing operand fails); prototype cases for operator forms including the
1..100 precedence validation and the operand-count check; and the two flipped
expectations noted above (`+ 10`, `10 ++ 5` now parse).

`Chapter6/test/codegen_test.cpp` — new `UnaryOpTest` (mocks a `unary<op>`
function, checks the generated call) and a `createInsertionPoint()` fixture
helper (expression codegen needs a block to insert into, or non-folded
instructions leak parentless). The `'?'`-is-invalid case from earlier
chapters is deleted, with a comment explaining why: since the `assert`
replaced `logErrorV`, an unknown operator aborts rather than returning an
error — kept for upstream fidelity, to be restored in v2.

`Chapter6/test/filecheck/jit.k` — carried forward from Chapter5 and
extended: a user-defined `binary%` (precedence 40, `a - b`) is defined and
then `10 % 3;` must print `Evaluated to 7.000000` — the full
define → install-precedence → reparse → JIT → execute loop in two lines.

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
  validation, and the two deliberate must-pass flips.
- **`codegen_test.cpp`** — `UnaryOpTest` verifies the desugared call at the
  API level against a mocked `unary<op>`; the invalid-operator case is
  retired (see above — `assert` aborts, nothing to assert on from gtest).
- **`test/filecheck/jit.k`** (carried from Chapter5) — the evaluate-loop and
  error-recovery checks, plus this chapter's addition: `def binary% 40 (a b)
  a - b;` then `10 % 3;` → `Evaluated to 7.000000`, executing a user-defined
  operator end-to-end (`userops.k` checks the IR; this checks the run).

```sh
ctest --test-dir build                # everything
lit -v test/filecheck/userops.k       # operator precedence end-to-end
./run.sh                              # demos incl. the Mandelbrot render
```
