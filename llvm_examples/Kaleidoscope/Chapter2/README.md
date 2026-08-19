# Chapter 1 & 2 — Lexer, Parser, and AST

A compiler frontend turns flat source text into a structured form the rest of
the compiler can work with. Three concepts do that job:

- **Lexer** (also *scanner* or *tokenizer*) — groups the raw character stream
  into **tokens**, the "words" of the language: keywords (`def`), identifiers
  (`foo`), number literals (`4.0`), operators and punctuation. It also
  discards what only humans care about — whitespace and comments — so the
  parser never has to think about individual characters.
- **Parser** — reads the token stream and recognizes the language's
  **grammar**: which token sequences form a valid definition, call, or
  expression. It rejects invalid input with errors, and for valid input it
  builds the AST.
- **AST** (*abstract syntax tree*) — the parser's output: a tree that models
  the program's nested structure directly. Grouping and operator precedence
  become parent/child relationships, and surface details — parentheses,
  whitespace, where a token sat in the file — disappear. "Abstract" means
  exactly that: it abstracts away the concrete syntax. This is the form all
  later compiler stages (code generation, from Chapter 3 on) traverse.

```
  "x + y*2"   ──lexer──▶   x  +  y  *  2   ──parser──▶    '+'
  characters               5 tokens                      /    \
                                                        x     '*'
                                                             /    \
                                                            y      2
```

This directory implements exactly that frontend for the Kaleidoscope
language, following the first two chapters of the LLVM tutorial:

- [Chapter 1: Kaleidoscope Language and Lexer](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl01.html)
- [Chapter 2: Implementing a Parser and AST](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl02.html)

Kaleidoscope itself is a tiny procedural language with exactly one type —
every value is a 64-bit double, so there are no type declarations. At this
stage it consists of `def` function definitions, `extern` declarations,
expressions, and `#` comments (see the Chapter 1 doc for the full
description; one syntax gotcha worth knowing up front: prototype parameters
are space-separated, `def foo(x y)`, while call arguments use commas,
`foo(a, b)`).

The code realizes the pipeline above as two classes. `main.cpp` constructs
one `Lexer` and one `Parser` (which holds a `Lexer&`), then hands control to
`parser.mainLoop()`:

```
                source text (stdin)
                      │  getchar(), one character at a time
                      ▼
        ┌─────────────────────────────┐
        │ toy::Lexer                  │  state: lastChar (1-char lookahead),
        │   int gettok()              │         identifierStr, numVal
        └─────────────────────────────┘
                      │  token stream: tok_def, tok_identifier, '(', '+', ...
                      │  side data pulled via getIdentifierStr() / getNumVal()
                      ▼
        ┌─────────────────────────────┐
        │ toy::Parser                 │  state: curTok (1-token lookahead),
        │   mainLoop()                │         binopPrecedence map
        │   parse*() methods          │
        └─────────────────────────────┘
             │                    │
     success │                    │ failure
             ▼                    ▼
   AST nodes (ast.h)      logError / logErrorP (log.h/.cpp)
   NumberExprAST, ...     print "Error: ..." to stderr and return
                          nullptr, which propagates up the parse calls
```

Note the symmetry: the lexer keeps a **one-character** lookahead (`lastChar`)
over the character stream, and the parser keeps a **one-token** lookahead
(`curTok`) over the token stream. Each stage peeks at exactly one unit of its
input before deciding what to do.

The tutorial's single-file `toy.cpp` maps onto the `src/` + `include/` +
`test/` layout as follows — note this directory covers only the frontend
stage: no code generation yet; the driver just reports what it parsed.

| File | Tutorial counterpart |
| --- | --- |
| `include/lexer.h`, `src/lexer.cpp` | Ch1 lexer: the `Token` enum and `gettok()`. The globals `IdentifierStr`, `NumVal`, and `LastChar` become members of a `Lexer` class (`identifierStr`, `numVal`, `lastChar`). |
| `include/ast.h` | Ch2 AST: `ExprAST` base class with `NumberExprAST`, `VariableExprAST`, `BinaryExprAST`, `CallExprAST`, plus `PrototypeAST` and `FunctionAST`. Header-only — the nodes are pure data at this stage. |
| `include/parser.h`, `src/parser.cpp` | Ch2 parser and driver: all `Parse*`/`Handle*` functions and `MainLoop()` become methods of a `Parser` class. The globals `CurTok` and `BinopPrecedence` become members; the precedence table is filled in the constructor. |
| `include/log.h`, `src/log.cpp` | Ch2 error helpers `LogError` / `LogErrorP` (and `logErrorV`, declared ahead of codegen chapters). They print to stderr and return `nullptr`, so a failed parse propagates up as a null pointer. |
| `src/main.cpp` | The tutorial's `main()`: constructs a `Lexer` and a `Parser`, then runs `parser.mainLoop()`. |

## Chapter 1: the lexer

`Lexer::gettok()` reads characters from **stdin** (`getchar()`, as upstream)
and returns either a value from the `Token` enum (`tok_eof`, `tok_def`,
`tok_extern`, `tok_identifier`, `tok_number`) or the raw ASCII value of an
unknown character (so `'+'`, `'('`, `';'` pass through as themselves). Side
data travels with the token: `getIdentifierStr()` after `tok_identifier`,
`getNumVal()` after `tok_number`.

The logic follows the tutorial: skip whitespace, lex `[a-zA-Z][a-zA-Z0-9]*`
identifiers and check them against keywords, lex `[0-9.]+` numbers with
`strtod`, and skip `#` comments to end of line.

### Deviation from upstream: leading-dot handling in the number path

The one deliberate change to `gettok()` is in the number-lexing branch. The
tutorial's version is the naive loop:

```cpp
// upstream (LangImpl01)
if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
  std::string NumStr;
  do {
    NumStr += LastChar;
    LastChar = getchar();
  } while (isdigit(LastChar) || LastChar == '.');
  NumVal = strtod(NumStr.c_str(), nullptr);
  return tok_number;
}
```

This has a real bug the tutorial itself points out: *anything* starting with
`.` becomes a number. A lone `.` produces `NumStr = "."`, and `strtod(".")`
silently yields `0.0` — so typing `.` gives you the number zero instead of an
error.

The refactored version validates the leading dot before committing to the
number path (`src/lexer.cpp:25`):

**`Chapter2/src/lexer.cpp`**
```cpp
if (isdigit(lastChar) || lastChar == '.') {  // Number: [0-9.]+
    std::string numStr;
    if (lastChar == '.') {
        // If we see a dot, it must be followed by a digit to be a valid number.
        int nextChar = getchar();
        if (!isdigit(nextChar)) {
            // Not a valid number, return the dot as a token.
            lastChar = nextChar;  // stash the lookahead for the next gettok() call
            return '.';
        }
        numStr += "0.";      // prepend a zero for numbers like ".5"
        lastChar = nextChar; // continue lexing from the digit after the dot
    }

    do {
        numStr += lastChar;
        lastChar = getchar();
    } while (isdigit(lastChar) || lastChar == '.');

    numVal = strtod(numStr.c_str(), nullptr);
    return tok_number;
}
```

As a flow chart:

```
 lastChar ∈ [0-9] or '.'
        │
        ├── lastChar == '.' ─── peek: nextChar = getchar()
        │        │
        │        ├── nextChar is NOT a digit ──▶ lastChar = nextChar
        │        │        (".", ".x", "..")      return '.'  (plain ASCII token)
        │        │
        │        └── nextChar IS a digit ─────▶ numStr = "0."
        │                 (".5")                 lastChar = nextChar, fall through
        ▼
 do { numStr += lastChar; lastChar = getchar(); }
 while (isdigit(lastChar) || lastChar == '.')
        │
        ▼
 numVal = strtod(numStr);  return tok_number
```

Step by step, the change does three things:

1. **Peek one character past the dot.** Since the dot alone is ambiguous
   (start of `.5`? stray punctuation?), the lexer reads one more character to
   decide. If it is not a digit, the dot was not a number after all: `gettok`
   returns `'.'` as an ordinary ASCII token, exactly like any other unknown
   character.
2. **Preserve the lookahead invariant.** The peeked character has already
   been consumed from stdin, so it must be stored back into `lastChar` before
   returning — otherwise the next `gettok()` call would skip it. This is the
   same "read but not yet processed" bookkeeping `lastChar` does everywhere
   else in the lexer.
3. **Normalize `.5` to `"0.5"` for `strtod`.** When a digit does follow,
   `numStr += "0."` seeds the buffer with an explicit leading zero, and
   `lastChar` is set to the digit so the ordinary do-while loop below takes
   over unchanged. The dot itself is never appended twice — it only enters
   `numStr` via the `"0."` prefix.

Behavior comparison:

| input     | upstream                              | refactored                            |
| --------- | ------------------------------------- | ------------------------------------- |
| `.5`      | `tok_number` 0.5 (`strtod(".5")`)     | `tok_number` 0.5 (`strtod("0.5")`)    |
| `.`       | `tok_number` **0.0** (silent)         | `'.'` ASCII token                     |
| `.x`      | `tok_number` 0.0, then `x`            | `'.'` token, then identifier `x`      |
| `3.14.15` | `tok_number` 3.14 (strtod stops at 2nd dot) | same — quirk deliberately kept  |

The multi-dot quirk (`3.14.15` consumed as one token, `strtod` stopping at the
second dot) is upstream behavior and is intentionally preserved, per the
Chapter2–9 policy of staying faithful to the tutorial except where noted.

Other lexer facts worth knowing: identifiers do **not** allow `_` (upstream
uses `isalnum`), and scientific notation is not supported — `1.234567e+10`
lexes as the number `1.234567` followed by separate `e`, `+`, `10` tokens.
Both are covered by cases in `test/lexer_test.cpp`.

## Chapter 2, part 1: the AST (`ast.h`)

The AST captures the program's structure in a form that later compiler stages
(codegen, from Chapter 3 on) can walk without caring about surface syntax —
once the tree is built, parentheses, whitespace, and token order are gone.

Expressions form a small class hierarchy. They share a common base class so
any expression can be stored uniformly as `std::unique_ptr<ExprAST>`;
prototypes and functions are separate roots because they are not expressions:

```
                     ExprAST  (abstract base)
                        ▲
        ┌───────────────┼────────────────┬──────────────────┐
        │               │                │                  │
  NumberExprAST   VariableExprAST   BinaryExprAST      CallExprAST
  (double Val)    (string Name)     (char Op,          (string Callee,
                                     LHS, RHS ────┐     vector of Args ──┐
                                     subtrees)    │     subtrees)        │
                                                  ▼                     ▼
                                             ExprAST children      ExprAST children

  FunctionAST ──owns──▶ PrototypeAST   (function name + parameter names)
       │
       └──────owns──▶ ExprAST          (the body expression tree)
```

The base class is nothing but a virtual destructor — needed so a tree held
through `ExprAST*` pointers destructs correctly — and each node is plain data
captured at construction time:

**`Chapter2/include/ast.h`**
```cpp
class ExprAST {
public:
  virtual ~ExprAST() = default;
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};
```

Two things to notice:

- **Ownership is strictly top-down.** Every child is a `std::unique_ptr`, so
  a `FunctionAST` owns its whole subtree; destroying the root frees the tree.
  This is why the parser passes nodes around with `std::move` — ownership
  transfers up as productions complete.
- **No behavior yet.** At this stage the nodes have no methods besides
  constructors (upstream adds `codegen()` in Chapter 3). Because Kaleidoscope
  has one type, there is also no type field anywhere — everything is a
  `double`.

Functions are described by two non-expression classes. `PrototypeAST` records
the *interface* — the name and parameter names (and therefore, implicitly, the
arity); `FunctionAST` pairs a prototype with a body expression:

**`Chapter2/include/ast.h`**
```cpp
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}
  const std::string &getName() const { return Name; }
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;
public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
};
```

Keeping `PrototypeAST` separate (rather than folding it into `FunctionAST`)
matters because `extern sin(a);` is a prototype *without* a body — the same
class serves both function definitions and extern declarations.

Concretely, `def foo(x y) x + foo(y, 4.0);` parses into this tree:

```
FunctionAST
 ├─ Proto: PrototypeAST("foo", ["x", "y"])
 └─ Body:  BinaryExprAST('+')
            ├─ LHS: VariableExprAST("x")
            └─ RHS: CallExprAST("foo")
                     ├─ VariableExprAST("y")
                     └─ NumberExprAST(4.0)
```

## Chapter 2, part 2: the parser

The parser combines **recursive descent parsing** for most constructs with
**operator-precedence parsing** for binary expressions. Each `parse*` method
implements one grammar production, eats exactly the tokens of that production,
and returns an AST node (or `nullptr` on error):

```
numberexpr     ::= number
parenexpr      ::= '(' expression ')'
identifierexpr ::= identifier | identifier '(' expression (',' expression)* ')'
primary        ::= identifierexpr | numberexpr | parenexpr
expression     ::= primary binoprhs
binoprhs       ::= (binop primary)*
prototype      ::= id '(' id* ')'
definition     ::= 'def' prototype expression
external       ::= 'extern' prototype
toplevelexpr   ::= expression
```

The call graph mirrors the grammar one-to-one. `mainLoop()` dispatches on the
current token, and the expression machinery calls back into itself — the
recursion is what lets flat method calls handle arbitrarily nested input:

```
 mainLoop()  ── dispatch on curTok ──┐
   ├─ tok_def    ─▶ handleDefinition()        ─▶ parseDefinition()
   │                                                ├─▶ parsePrototype()
   │                                                └─▶ parseExpression()
   ├─ tok_extern ─▶ handleExtern()            ─▶ parseExtern()
   │                                                └─▶ parsePrototype()
   ├─ ';'        ─▶ eat and continue
   └─ default    ─▶ handleTopLevelExpression() ─▶ parseTopLevelExpr()
                                                    └─▶ parseExpression()

 parseExpression()
   ├─▶ parsePrimary()
   │     ├─ tok_number     ─▶ parseNumberExpr()
   │     ├─ tok_identifier ─▶ parseIdentifierExpr()
   │     │                       └─ call arguments ──▶ parseExpression()   ←─ recursion
   │     └─ '('            ─▶ parseParenExpr()
   │                             └─ inner expr ──────▶ parseExpression()   ←─ recursion
   └─▶ parseBinOpRHS(0, lhs)
         ├─▶ parsePrimary()                    (the operand after each binop)
         └─▶ parseBinOpRHS(tokPrec + 1, rhs)   ←─ recursion, only when the next
                                                  operator binds tighter
```

### The token buffer and error helpers

Everything in the parser is built on a one-token buffer. `curTok` is the token
every `parse*` method looks at; "eating" a token means calling
`getNextToken()`, which refills the buffer from the lexer:

**`Chapter2/src/parser.cpp`**
```cpp
// Helper to bridge the Lexer to the Parser's curTok
int Parser::getNextToken() {
    return curTok = lexer.gettok();
}
```

The invariant every method maintains: **on entry, `curTok` is the first token
of your production; on exit, `curTok` is the first token *after* it.** Keeping
that invariant is what lets the methods compose freely.

Errors use the helpers from `log.h`. They print and return `nullptr`, so a
failure anywhere deep in a parse simply bubbles up as a null pointer through
the `if (!x) return nullptr;` checks:

**`Chapter2/src/log.cpp`**
```cpp
std::unique_ptr<ExprAST> logError(const char* str) {
    fprintf(stderr, "Error: %s\n", str);
    return nullptr;
}
```

### Basic expression parsing

The simplest production shows the pattern — grab the value the lexer stashed,
build a node, eat the token:

**`Chapter2/src/parser.cpp`**
```cpp
// numberexpr ::= number
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
    auto result = std::make_unique<NumberExprAST>(lexer.getNumVal());
    getNextToken();  // eat the number
    return std::move(result);
}
```

`parseParenExpr()` demonstrates two more ideas. First, recursion: it calls
`parseExpression()`, which can contain parentheses again — nested grammars
fall out of plain method calls. Second, the parentheses produce **no AST
node**; they exist only to guide the parser, and once the tree shape encodes
the grouping they can be discarded:

**`Chapter2/src/parser.cpp`**
```cpp
// parenexpr ::= '(' expression ')'
std::unique_ptr<ExprAST> Parser::parseParenExpr() {
    getNextToken(); // eat (
    auto v = parseExpression();
    if (!v) return nullptr;

    if (curTok != ')') return logError("expected ')'");
    getNextToken(); // eat )
    return v;
}
```

`parseIdentifierExpr()` handles both `x` and `foo(a, b)` — it decides which by
**lookahead**: after eating the identifier, if `curTok` is not `'('` it was a
plain variable reference, otherwise it loops over comma-separated call
arguments (each a full `parseExpression()`) until the closing `')'`.

`parsePrimary()` is just the dispatcher over these three:

**`Chapter2/src/parser.cpp`**
```cpp
// primary ::= identifierexpr | numberexpr | parenexpr
std::unique_ptr<ExprAST> Parser::parsePrimary() {
    switch (curTok) {
    case tok_identifier: return parseIdentifierExpr();
    case tok_number:     return parseNumberExpr();
    case '(':            return parseParenExpr();
    default:             return logError("unknown token when expecting an expression");
    }
}
```

### Operator-precedence parsing

Binary expressions are the one place recursive descent alone gets awkward
(`a + b * c` is ambiguous without precedence), so the tutorial uses
operator-precedence parsing. The precedence table lives in the `Parser`
constructor — `'<'` → 10, `'+'`/`'-'` → 20, `'*'` → 40 — and
`getTokPrecedence()` returns `-1` for anything that is not a binary operator,
which conveniently fails every `tokPrec < exprPrec` comparison and stops the
loop:

**`Chapter2/src/parser.cpp`**
```cpp
int Parser::getTokPrecedence() {
    if (!isascii(curTok)) return -1;
    auto it = binopPrecedence.find(static_cast<char>(curTok));
    if (it == binopPrecedence.end()) return -1;
    return it->second;
}
```

An expression is a primary followed by a possibly-empty sequence of
`[binop, primary]` pairs; `parseBinOpRHS` consumes that sequence:

**`Chapter2/src/parser.cpp`**
```cpp
std::unique_ptr<ExprAST> Parser::parseExpression() {
    auto lhs = parsePrimary();
    if (!lhs) return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
}

// binoprhs ::= (binop primary)*
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        int tokPrec = getTokPrecedence();

        // Only consume operators that bind at least as tightly as exprPrec.
        if (tokPrec < exprPrec) return lhs;

        int binOp = curTok;
        getNextToken(); // eat binop

        auto rhs = parsePrimary();
        if (!rhs) return nullptr;

        // If the NEXT operator binds tighter than this one, it deserves rhs
        // as its lhs — recurse to let it take everything above tokPrec.
        int nextPrec = getTokPrecedence();
        if (tokPrec < nextPrec) {
            rhs = parseBinOpRHS(tokPrec + 1, std::move(rhs));
            if (!rhs) return nullptr;
        }
        lhs = std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
    }
}
```

`exprPrec` means: "only consume operators at least this strong; anything
weaker belongs to my caller." Tracing `a + b * c;`:

```
parseExpression
  lhs = a
  parseBinOpRHS(0, a)
    tokPrec('+') = 20 ≥ 0    → eat '+', rhs = b
    nextPrec('*') = 40 > 20  → '*' binds tighter than '+', so b belongs to it:
      parseBinOpRHS(21, b)
        tokPrec('*') = 40 ≥ 21 → eat '*', rhs = c
        nextPrec(';') = -1     → no recursion
        lhs = (b * c)
        tokPrec(';') = -1 < 21 → return (b * c)
    lhs = a + (b * c)
    tokPrec(';') = -1 < 0    → return a + (b * c)
```

Left-associativity comes from the `while` loop: in `a - b - c`, the second
`'-'` has the *same* precedence as the first (20, not > 20), so there is no
recursion — the loop just merges `(a - b)` first and then `((a - b) - c)`.
The `tokPrec + 1` in the recursive call is exactly what makes equal-precedence
operators stay with the outer loop.

### Prototypes, definitions, externs, top-level expressions

`parsePrototype()` reads `id '(' id* ')'` — note the parameter names are
**space-separated** (no commas). `parseDefinition()` and `parseExtern()` are
thin compositions:

**`Chapter2/src/parser.cpp`**
```cpp
// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> Parser::parseDefinition() {
    getNextToken(); // eat def
    auto proto = parsePrototype();
    if (!proto) return nullptr;

    if (auto e = parseExpression())
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> Parser::parseExtern() {
    getNextToken(); // eat extern
    return parsePrototype();  // no body — extern is just an interface
}
```

A bare expression at the top level gets wrapped in an anonymous zero-argument
function, so the rest of the pipeline only ever sees functions. The name
`__anon_expr` is what later chapters look up to JIT-execute the expression:

**`Chapter2/src/parser.cpp`**
```cpp
// top-level expression ::= expression
std::unique_ptr<FunctionAST> Parser::parseTopLevelExpr() {
    if (auto e = parseExpression()) {
        auto proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    }
    return nullptr;
}
```

### The driver loop

`mainLoop()` bootstraps the token buffer once, then dispatches on `curTok`
forever. Top-level `';'` is simply eaten, which is what lets you end
statements without the parser complaining:

**`Chapter2/src/parser.cpp`**
```cpp
void Parser::mainLoop() {
    getNextToken(); // Bootstrap the first token
    while (true) {
        fprintf(stderr, "ready> ");
        switch (curTok) {
        case tok_eof: return;
        case ';':     getNextToken(); break;  // ignore top-level semicolons.
        case tok_def: handleDefinition(); break;
        case tok_extern: handleExtern(); break;
        default:      handleTopLevelExpression(); break;
        }
    }
}
```

Each `handle*` wrapper prints `Parsed a ...` on success; on failure the error
helpers have already printed `Error: ...`, and the handler eats one token for
**error recovery** so the loop can resynchronize instead of crashing — the
behavior `test/filecheck/parse-error.k` pins down.

## Build and run

Requires LLVM (found via `find_package(LLVM)`; only the `core` component is
linked) and CMake ≥ 3.20 with Ninja.

```sh
./build.sh            # clean configure + build, then runs ./build/toy < cmd.txt
```

or manually:

```sh
cmake -B build -G Ninja -S .
cmake --build build
./build/toy           # interactive REPL, reads from stdin
./build/toy < cmd.txt # feed it the sample input
```

Example session (as in the Chapter 2 tutorial):

```
ready> def foo(x y) x+foo(y, 4.0);
Parsed a function definition.
ready> def foo(x y) x+y y;
Parsed a function definition.
Parsed a top-level expr
ready> def foo(x y) x+y );
Parsed a function definition.
Error: unknown token when expecting an expression
ready> extern sin(a);
ready> Parsed an extern
```

(When input is piped rather than typed, the same output appears with extra
`ready>` prompts interleaved — e.g. `ready> ready> Parsed a function
definition.` — because the prompt is printed before each read but nothing
echoes the input between them.)

## Tests

The test setup mirrors how the LLVM project itself is tested — GoogleTest
unit tests plus lit + FileCheck regression tests. The rationale for the two
schemes, the "if a tool's output shows it, write a lit test" rule of thumb,
and the full lit/`lit.cfg` mechanics are in the
[top-level README](../README.md#testing-the-two-schemes); this section covers
what the two layers pin down **here**. `toy` plays the role of `opt`/`clang`,
the `.k` files play the role of `llvm/test/*.ll`, and the gtest binaries
cover the class-level contracts of `Lexer` and `Parser`:

- **GoogleTest unit tests** (`test/lexer_test.cpp`, `test/parser_test.cpp`)
  test **one component in isolation, at the C++ API level**. They call
  `Lexer::gettok()` or a single `parse*` entry point directly and assert on
  in-memory results — exact token kinds, `getNumVal()` values, whether a
  `parse*` call returns an AST node or `nullptr`. This is the right tool for
  pinning down component behavior precisely (number edge cases like `.5` and
  `3.14.15`, comment skipping, which malformed inputs each parser production
  rejects), independently of the driver or the other components.

- **lit + FileCheck end-to-end tests** (`test/filecheck/*.k`) test the
  **whole compiler as a black box**: real `.k` source goes in through the
  actual `toy` binary, and the textual output is verified. Each file starts
  with a `RUN:` line telling lit how to execute it
  (`%toy < %s 2>&1 | FileCheck %s`), which for `parse.k` expands to:

  ```sh
  build/toy < test/filecheck/parse.k 2>&1 | FileCheck test/filecheck/parse.k
  ```

  `parse.k` checks the happy path; `parse-error.k` checks that errors are
  reported *and* parsing continues — FileCheck's `CHECK` lines enforce
  *ordering*, so an `Error:` line followed by a later
  `Parsed a function definition.` proves the driver kept going. Because the
  test only sees the binary's output, it keeps passing across internal
  refactors (globals → classes, etc.) — it pins down user-visible behavior,
  not implementation.

Running everything via CTest:

```sh
ctest --test-dir build                # everything
ctest --test-dir build -R filecheck   # just the lit tests
./build/lexer_test                    # run a gtest binary directly
```

Running an individual FileCheck test with lit (`%toy` resolves via the
`TOY_BIN` env var, defaulting to `./build/toy`):

```sh
lit -v test/filecheck/parse.k                            # single test
TOY_BIN=$PWD/build/toy lit -v test/filecheck/parse-error.k   # explicit binary
```

Or replay a `RUN:` line by hand, without lit:

```sh
./build/toy < test/filecheck/parse.k 2>&1 | FileCheck test/filecheck/parse.k
```
