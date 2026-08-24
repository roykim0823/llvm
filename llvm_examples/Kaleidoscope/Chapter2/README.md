# Chapter 1 & 2 — Lexer, Parser, and AST

A compiler **frontend** turns flat source text into a structured form the
rest of the compiler can work with. 
Three concepts do the frontend's job:

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

Kaleidoscope itself is a tiny procedural language with exactly one type —
every value is a 64-bit double, so there are no type declarations anywhere.
Everything the language consists of at this stage:

- **Function definitions**: `def` + a prototype + a body that is a *single
  expression* — its value is the return value. There is no `return`, and no
  statements at all:

  ```
  # a comment: '#' runs to end of line
  def foo(x y)          # parameters are SPACE-separated
    x + y*2;
  ```

- **Extern declarations**: `extern sin(x);` declares a function defined
  elsewhere (later chapters link such calls to libm) so it can be called.
- **Expressions**: number literals (`4.0`, `.5`), references to function
  parameters (`x` — the only variables that exist for now), the four binary
  operators `<` `+` `-` `*` with the usual precedence, parenthesized
  expressions, and calls — `foo(a, b)`, with **comma**-separated arguments.
- **Top-level expressions**: a bare expression like `1 + 2;` is valid at the
  top level; `;` separates top-level items.

One syntax gotcha worth knowing up front, visible in the snippets above:
prototype parameters are space-separated (`def foo(x y)`), while call
arguments use commas (`foo(a, b)`).

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

The code is organized into three directories — `src/`, `include/`, and
`test/` — as tabulated below. Note this directory covers only the frontend
stage: no code generation yet; the driver just reports what it parsed.

| File | Contents — and its tutorial counterpart |
| --- | --- |
| `include/lexer.h`, `src/lexer.cpp` | The `Token` enum and the `Lexer` class: `gettok()` plus the token's side data. Upstream's globals `IdentifierStr`/`NumVal` became the members `identifierStr`/`numVal`, and `gettok()`'s `static int LastChar` local became the member `lastChar`. |
| `include/ast.h` | The AST: `ExprAST` base class with `NumberExprAST`, `VariableExprAST`, `BinaryExprAST`, `CallExprAST`, plus `PrototypeAST` and `FunctionAST` — upstream's classes unchanged, moved from an anonymous namespace into `namespace toy`. Header-only — the nodes are pure data at this stage. |
| `include/parser.h`, `src/parser.cpp` | The `Parser` class: upstream's free `Parse*`/`Handle*` functions and `MainLoop()` became the `parse*`/`handle*` methods and `mainLoop()`; the globals `CurTok` and `BinopPrecedence` became the members `curTok` and `binopPrecedence` (the table now filled in the constructor rather than in `main()`). |
| `include/log.h`, `src/log.cpp` | Error helpers `logError`/`logErrorP` — upstream's `LogError`/`LogErrorP` free functions. They print to stderr and return `nullptr`, so a failed parse propagates up as a null pointer. |
| `src/main.cpp` | `main()`: constructs a `Lexer` and a `Parser`, then runs `parser.mainLoop()` — upstream's `main()` minus the precedence-table setup and the token priming, both of which moved into the `Parser` (see the deviation subsections). |

## Chapter 1: The Lexer

Each token the lexer returns is a **token code and potentially some
metadata**. The codes are an enum — note the values are all *negative*, which
leaves the positive range free: an unknown character like `'+'` or `'('` is
returned **as its own ASCII value**, so single-character operators and
punctuation need no enum entries at all:

**`Chapter2/include/lexer.h`**
```cpp
// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5
};

class Lexer {
    public:
    int gettok();
    double getNumVal() const { return numVal; }
    std::string getIdentifierStr() const { return identifierStr; }

    private:
    std::string identifierStr; // Filled in if tok_identifier
    double numVal = 0.0;       // Filled in if tok_number
    int lastChar = ' ';  // Used by gettok
};
```

The metadata rides alongside the code: after `gettok()` returns
`tok_identifier`, `getIdentifierStr()` holds the name; after `tok_number`,
`getNumVal()` holds the value.

The whole lexer is the single function `gettok()`, which reads characters
one at a time from **stdin** via `getchar()` and consumes exactly one
token's worth per call. One invariant before reading it: `lastChar` always
holds the last character **read but not yet processed**. Recognizing where a
token *ends* requires reading one character *past* it (the `f` of `def` is
only the end of the identifier once the following space is seen), and since
stdin can't be un-read, that overshoot character must be carried over to the
next call — that is `lastChar`'s job, and why it lives in the class rather
than as a local. The numbered markers key the notes below:

**`Chapter2/src/lexer.cpp`**
```cpp
int Lexer::gettok() {

    while (isspace(lastChar))                    // [1] skip whitespace
        lastChar = getchar();

    if (isalpha(lastChar)) {                     // [2] identifier: [a-zA-Z][a-zA-Z0-9]*
        identifierStr = lastChar;
        while (isalnum((lastChar = getchar())))
            identifierStr += lastChar;

        if (identifierStr == "def") return tok_def;
        if (identifierStr == "extern") return tok_extern;
        return tok_identifier;
    }

    if (isdigit(lastChar) || lastChar == '.') {  // [3] number: [0-9.]+
        ...                                      //     (full branch in the
    }                                            //      deviation section below)

    if (lastChar == '#') {                       // [4] comment until end of line
        do lastChar = getchar();
        while (lastChar != EOF && lastChar != '\n' && lastChar != '\r');

        if (lastChar != EOF) return gettok();
    }

    if (lastChar == EOF) return tok_eof;         // [5] don't eat the EOF

    int thisChar = lastChar;                     // [6] anything else: return the
    lastChar = getchar();                        //     character as its ASCII value
    return thisChar;
}
```

- **[1] Whitespace** separates tokens and means nothing, so it is eaten
  before anything else. `lastChar`'s initial value of `' '` is a small
  trick: it *is* whitespace, so the very first call falls straight through
  this loop into real input.
- **[2] Identifiers and keywords** share one loop — a keyword is just an
  identifier whose spelling is special-cased after the fact. The loop also
  fills the `identifierStr` metadata as it goes.
- **[3] Numbers** follow the same accumulate-then-convert shape: collect
  `[0-9.]+` into a string, let C's `strtod` turn it into the `double`
  stored in `numVal`, return `tok_number`. This is the one place the
  *lexer* deliberately deviates from upstream (validating a leading `.`),
  so the branch is shown in full in its own subsection below — including
  upstream's acknowledged non-check that makes `1.23.45.67` silently lex
  as `1.23`.
- **[4] Comments** run to end of line; `gettok()` then *recurses* to return
  whatever token follows the comment, so the parser never sees one.
- **[5] End of input** maps to `tok_eof` — and EOF is deliberately *not*
  eaten: `lastChar` keeps it, so every later call also returns `tok_eof`
  instead of blocking on a closed stream.
- **[6] Everything else** — `+`, `(`, `;` — is returned as its raw ASCII
  value (the negative-enum trick from above), with `lastChar` advanced
  past it.

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

Other lexer facts worth knowing: identifiers do **not** allow `_` (the
identifier loop accepts `isalnum` characters only, so `my_var` lexes as
`my`, `'_'`, `var`), and scientific notation is not supported — `1.234567e+10`
lexes as the number `1.234567` followed by separate `e`, `+`, `10` tokens.
Both are covered by cases in `test/lexer_test.cpp`, alongside the
leading-dot behaviors from the deviation section above (`.` and `.x`
returning the ASCII `'.'` token).

## Chapter 2: The Parser and AST

The parser combines two techniques: **recursive descent parsing** for most of
the grammar (definitions, calls, primaries — each production becomes one
method that calls the others), and **operator-precedence parsing** for the
one place recursive descent alone is awkward, binary expressions. Before
getting to the parsing itself, though, it helps to look at what the parser
*produces*: the Abstract Syntax Tree.

## 2.1 The Abstract Syntax Tree

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
through `ExprAST*` pointers destructs correctly — and every node is plain
data captured at construction time. The whole expression hierarchy:

**`Chapter2/include/ast.h`**
```cpp
/// ExprAST - Base class for all expression nodes.
class ExprAST {                                                   // [1]
public:
  virtual ~ExprAST() = default;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {                            // [2]
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {                          // [3]
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
};

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {                            // [4]
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {                              // [5]
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(const std::string &Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
};
```

- **[1] `ExprAST`** exists only so the other four can be held, moved, and
  destroyed uniformly as `std::unique_ptr<ExprAST>` — which is what lets a
  `BinaryExprAST` hold *any* expression as an operand without knowing its
  concrete kind.
- **[2] `NumberExprAST`** is a leaf: it captures the `double` the lexer
  stashed in `numVal`, and nothing else.
- **[3] `VariableExprAST`** is the other leaf: just the referenced name.
  Whether the name actually exists is not checked here — that is a semantic
  question, deferred to codegen (Chapter 3's "unknown variable" error).
- **[4] `BinaryExprAST`** is the interior node that gives the tree its
  shape: an operator character plus two owned subtrees. Note there is one
  class for *all* binary operators — which operator it is, is data (`Op`),
  not a type.
- **[5] `CallExprAST`** holds the callee's *name* (not a pointer to its
  function — linking name to definition is also codegen's job) and an owned
  vector of argument subtrees.

Two things to notice:

- **Ownership is strictly top-down.** Every child is a `std::unique_ptr`, so
  a `FunctionAST` owns its whole subtree; destroying the root frees the tree.
  This is why the parser passes nodes around with `std::move` — ownership
  transfers up as productions complete.
- **No behavior yet.** At this stage the nodes have no methods besides
  constructors (a `codegen()` method arrives in Chapter 3). Because Kaleidoscope
  has one type, there is also no type field anywhere — everything is a
  `double`.
- **This is the complete expression vocabulary — and it cannot branch.**
  Nothing in the hierarchy expresses conditional control flow, so the
  language at this stage is **not Turing-complete**: every function body
  unconditionally evaluates one expression. Chapter 5 fixes that by adding
  `if`/`then`/`else` and `for` as new `ExprAST` subclasses.

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

## 2.2 The Parser

Now that we have an AST to build, we need the parser code to build it. The
goal is to parse something like `x+y` — which the lexer delivers as three
tokens — into an AST that could have been constructed by hand like this:

```cpp
auto LHS = std::make_unique<VariableExprAST>("x");
auto RHS = std::make_unique<VariableExprAST>("y");
auto Result = std::make_unique<BinaryExprAST>('+', std::move(LHS),
                                              std::move(RHS));
```

**Recursive descent** is the most direct way to turn a grammar into a
program: write **one function per grammar production**, and let the functions
call each other exactly the way the productions reference each other. The
name says how it runs — parsing starts at the topmost production and
*descends* into sub-productions, and because the grammar is *recursive* (an
expression can contain expressions), the functions are too. Here is
Kaleidoscope's grammar; every line below becomes one `parse*` method:

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

The grammar-to-code mapping is mechanical, and a few concrete instances show
the whole trick (each method appears in full later in this section):

- `numberexpr ::= number` is a single terminal, so `parseNumberExpr()` eats
  one token and builds one node — the smallest possible parser.
- `parenexpr ::= '(' expression ')'` is a *sequence*, so `parseParenExpr()`
  handles its three elements in order: eat `'('`, call `parseExpression()`,
  check-and-eat `')'`. The middle element is a *reference* to another
  production, and a reference always becomes a call to that production's
  method — here recursively, since the inner expression may contain
  parentheses again.
- `primary ::= identifierexpr | numberexpr | parenexpr` is a *choice*, so
  `parsePrimary()` is a `switch` on `curTok` — one token of lookahead
  decides which alternative applies.
- the `(',' expression)*` *repetition* inside `identifierexpr` becomes the
  argument loop in `parseIdentifierExpr()` — loop while the input can still
  continue the repetition.

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

We start with the simplest production, numeric literals — it shows the
shape every `parse*` method follows:

**`Chapter2/src/parser.cpp`**
```cpp
// numberexpr ::= number
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
    auto result = std::make_unique<NumberExprAST>(lexer.getNumVal());
    getNextToken();  // eat the number
    return std::move(result);
}
```

It expects to be called when `curTok` is `tok_number`, grabs the value the
lexer stashed in `numVal`, builds a `NumberExprAST`, advances the lexer past
the consumed token, and returns. That last step is the recursive-descent
handshake in action: eat *all* the tokens of your production — no more, no
less — and leave `curTok` on whatever comes next, ready for the caller.

The parenthesis production adds recursion and error handling:

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

The call to `parseExpression()` is recursive descent earning its keep: the
inner expression may itself contain parentheses, calls, anything, and this
method neither knows nor cares — it just calls the production's function and
gets a subtree back. Nesting to any depth costs zero extra code; it rides on
the call stack. Also note what is *not* built: the parentheses produce **no
AST node**. They exist only to guide the parser into the right grouping, and
once the tree shape encodes that grouping they can be discarded — this is
part of what "abstract" in AST means. Finally, the error path: if the inner
expression failed, the null just propagates; if the closing `')'` is missing,
`logError` reports and returns null itself.

Identifiers are more interesting because one production covers two different
things — a variable reference (`x`) and a function call (`foo(a, b)`):

**`Chapter2/src/parser.cpp`**
```cpp
// identifierexpr ::= identifier | identifier '(' expression* ')'
std::unique_ptr<ExprAST> Parser::parseIdentifierExpr() {
    std::string idName = lexer.getIdentifierStr();
    getNextToken(); // eat identifier

    if (curTok != '(')  // Simple variable ref.
        return std::make_unique<VariableExprAST>(idName);

    getNextToken(); // eat (
    std::vector<std::unique_ptr<ExprAST>> args;
    if (curTok != ')') {
        while (true) {
            if (auto arg = parseExpression())
                args.push_back(std::move(arg));
            else
                return nullptr;

            if (curTok == ')') break;
            if (curTok != ',') return logError("Expected ')' or ',' in argument list");
            getNextToken();
        }
    }
    getNextToken(); // eat )
    return std::make_unique<CallExprAST>(idName, std::move(args));
}
```

The decision is made by **lookahead**: after eating the identifier itself,
one glance at `curTok` settles which alternative of the production this is —
no `'('`, and it was a plain variable reference, done. Otherwise it is a
call, and the argument loop takes over: each argument is a full
`parseExpression()` (so arguments can be arbitrary expressions, including
nested calls), separated by commas, until the closing `')'`. The two failure
modes differ: a failed argument expression propagates its null silently
(the message was already printed deeper down), while a missing separator is
*this* production's own error to report.

Finally, the dispatcher that bundles the three into the `primary`
nonterminal the rest of the grammar refers to:

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

This is where "one token of lookahead" earns its keep: the current token
alone determines which production applies, so the choice is a `switch` —
and it is also why the three helpers above may safely *assume* their first
token is already the right one.

### Operator-precedence parsing

Binary expressions are the one place recursive descent alone gets awkward.
The production `binoprhs ::= (binop primary)*` says nothing about *grouping*:
for `a + b * c` it happily yields either tree, and the grammar rules above
give no way to prefer `a + (b * c)`. The classic recursive-descent fix is to
encode precedence into the grammar itself — one production per precedence
level, each referring to the next-tighter one:

```
expression ::= term (('+' | '-') term)*      // loosest level
term       ::= factor (('*') factor)*        // tighter level
factor     ::= primary                       // tightest
```

That works, but every precedence level is another production, another
method, and another call on the stack for *every* expression — and adding an
operator means grammar surgery. **Operator-precedence parsing** keeps the
single `binoprhs` production and moves the precedence knowledge into *data*:
a table mapping each operator to a number, consulted by one loop that decides
— for each operator it meets — whether to keep it or leave it for the caller.
(This particular formulation is also known as *precedence climbing*.)

The mental model behind it: an expression is **a stream of primary
expressions separated by binary operators** —

```
  a + b + (c + d) * e * f + g

  primary a, then the pairs:  [+, b]  [+, (c+d)]  [*, e]  [*, f]  [+, g]
```

Note that the parenthesized subexpression is *just another primary*:
recursive descent has already collapsed it into a single unit by the time the
operator machinery looks at it, so the pair-consuming loop never thinks about
nesting — the two techniques hand off cleanly at the `parsePrimary()` call.
Parsing then means: take the first primary as a provisional LHS, and consume
pairs one at a time, deciding for each — by precedence — whether it attaches
to the current LHS or belongs to something tighter.

The precedence lookup comes first. The table itself is filled in the `Parser`
constructor — `'<'` → 10, `'+'`/`'-'` → 20, `'*'` → 40 — and the helper
returns `-1` for anything that is not a binary operator at all:

**`Chapter2/src/parser.cpp`**
```cpp
int Parser::getTokPrecedence() {
    if (!isascii(curTok)) return -1;
    auto it = binopPrecedence.find(static_cast<char>(curTok));
    if (it == binopPrecedence.end()) return -1;
    return it->second;
}
```

The `-1` sentinel is doing quiet work: any non-operator token — `;`, `)`,
`,`, EOF — fails every `tokPrec < exprPrec` comparison below, so the
pair-consuming loop simply stops at it. Expression parsing never needs an
explicit "am I done?" check; running out of operators *is* the check.

#### Deviation from upstream: `find()` instead of `operator[]`

Upstream reads the table through `std::map::operator[]`:

```cpp
// upstream (LangImpl02)
static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;

  // Make sure it's a declared binop.
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0) return -1;
  return TokPrec;
}
```

On a `std::map`, `operator[]` **default-inserts** an entry for every key it
is asked about but does not find. Upstream is still correct — the freshly
inserted `0` fails the `<= 0` check and comes out as `-1` — but as a side
effect the precedence table silently grows a `{token, 0}` entry for every
distinct non-operator ASCII token the parser ever peeks at (`;`, `)`, `,`,
…). The refactored version uses `find()`, so the lookup answers the same
question without mutating the table:

| `curTok`                    | upstream                                        | refactored          |
| --------------------------- | ----------------------------------------------- | ------------------- |
| `'+'` (declared binop)      | 20                                              | 20                  |
| `';'`, `')'`, `','`, …      | -1, **and `{token, 0}` inserted into the map**  | -1, map untouched   |
| `tok_identifier` (negative) | -1 (fails the `isascii` check; no map lookup)   | same                |

Every input yields the same precedence — the only observable difference is
whether the map mutates.

`parseExpression()` states the stream model directly — a primary, then
whatever pairs follow:

**`Chapter2/src/parser.cpp`**
```cpp
// expression ::= primary binoprhs
std::unique_ptr<ExprAST> Parser::parseExpression() {
    auto lhs = parsePrimary();
    if (!lhs) return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
}
```

The `0` is the *minimum precedence* the callee is allowed to consume — and
since every real operator has precedence above 0, this call means "take
every pair you can".

`parseBinOpRHS(exprPrec, lhs)` is the core of the technique:

**`Chapter2/src/parser.cpp`**
```cpp
// binoprhs ::= (binop primary)*
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        int tokPrec = getTokPrecedence();

        // Only consume operators that bind at least as tightly as the current binop,
        if (tokPrec < exprPrec) return lhs;

        // Okay, we know this is a binop.
        int binOp = curTok;
        getNextToken(); // eat binop

        // Parse the primary expression after the binary operator.
        auto rhs = parsePrimary();
        if (!rhs) return nullptr;

        // If BinOp binds less tightly with RHS than the operator after RHS, let
        // the pending operator take RHS as its LHS.
        int nextPrec = getTokPrecedence();
        if (tokPrec < nextPrec) {
            rhs = parseBinOpRHS(tokPrec + 1, std::move(rhs));
            if (!rhs) return nullptr;
        }
        // Merge LHS/RHS.
        lhs = std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
    }
}
```

Read it as a loop over `[binop, primary]` pairs. `exprPrec` is the method's
contract with its caller: "only consume operators at least this strong;
anything weaker belongs to you." The first line of the loop enforces it —
if the pending operator is too weak (or is no operator at all, precedence
`-1`), return whatever LHS has been accumulated so far.

Having committed to a pair, the subtle moment arrives *after* parsing the
pair's primary: which operator does that primary belong to? Peek at the
*next* operator. If it binds no tighter than the current one, the primary
belongs to the current operator — merge `lhs = lhs op rhs` and loop. If it
binds *tighter*, the primary just parsed is really the start of a
higher-precedence subexpression — so recurse with `tokPrec + 1`, letting the
recursive call consume every pair stronger than the current operator, and
use whatever it returns as the real RHS. The `+ 1` is precise: it means
"strictly tighter than me", which both keeps equal-precedence operators in
the caller's loop *and* is exactly what makes them left-associative.

Tracing `a + b * c;`:

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

And the associativity case: in `a - b - c`, the second `'-'` has the *same*
precedence as the first (20, not > 20), so there is no recursion — the loop
merges `(a - b)` first and then `((a - b) - c)`.

### Prototypes, definitions, externs, top-level expressions

What remains is the module-level grammar. First the prototype — a function's
interface, shared by definitions and externs:

**`Chapter2/src/parser.cpp`**
```cpp
/// prototype ::= id '(' id* ')'
std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
    if (curTok != tok_identifier) return logErrorP("Expected function name in prototype");

    std::string fnName = lexer.getIdentifierStr();
    getNextToken();

    if (curTok != '(') return logErrorP("Expected '(' in prototype");

    std::vector<std::string> argNames;
    while (getNextToken() == tok_identifier)
        argNames.push_back(lexer.getIdentifierStr());

    if (curTok != ')') return logErrorP("Expected ')' in prototype");

    getNextToken(); // eat )
    return std::make_unique<PrototypeAST>(fnName, std::move(argNames));
}
```

The parameter list explains the space-separated syntax: the `while` loop
simply keeps accepting identifiers until something else appears — no commas
expected — and that something must then be the `')'`. (This is also where
`logErrorP` earns its existence: this production returns a
`PrototypeAST`, not an `ExprAST`, so it needs the error helper with the
matching return type.)

With prototypes in hand, `def` and `extern` are thin compositions:

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

A `def` is a prototype plus one body expression; an `extern` is a prototype
with no body at all. Externs serve two purposes: declaring library functions
like `sin`/`cos` (whose bodies live in libm), and *forward-declaring*
Kaleidoscope functions before their definition.

Last, top-level expressions — the thing that makes the REPL feel like a
calculator:

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

A bare expression gets wrapped in an anonymous **nullary** (zero-argument)
function, so the rest of the pipeline only ever deals in functions — one
uniform shape instead of a special "loose expression" case. The name
`__anon_expr` is what later chapters look up to JIT-execute the expression
and print its value.

### The driver loop

`mainLoop()` bootstraps the token buffer once, then dispatches on `curTok`
forever. Top-level `';'` is simply eaten — and it exists for a reason: at an
interactive prompt, after `4 + 5` the parser cannot know whether the
expression is finished or about to continue as `4 + 5 * 6`. Typing `;` is
the user saying "done" — `';'` has no precedence-table entry, so
`getTokPrecedence()` returns `-1` and expression parsing stops there;
`mainLoop()` then discards the delimiter:

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

#### Deviation from upstream: token priming moved into `mainLoop()`

Upstream bootstraps the token buffer in `main()`, printing a first prompt
before doing so:

```cpp
// upstream (LangImpl02)
int main() {
  ...
  // Prime the first token.
  fprintf(stderr, "ready> ");
  getNextToken();

  // Run the main "interpreter loop" now.
  MainLoop();
  ...
}
```

That split only exists because upstream's `CurTok` is a file-scope global
that `main()` and `MainLoop()` share. The refactored `main()`
(`src/main.cpp`) just constructs the `Lexer` and `Parser` and calls
`parser.mainLoop()`; the bootstrap `getNextToken()` is the first line of
`mainLoop()` itself, so the token buffer stays entirely the `Parser`'s
business and callers need no "prime first, then loop" protocol. The
trade-off is one visible behavior change at startup: the bootstrap read
blocks *before* the loop prints its first prompt.

|                       | upstream                            | refactored                    |
| --------------------- | ----------------------------------- | ----------------------------- |
| at startup            | prints `ready> `, waits for input   | waits for input, no prompt    |
| first line of input   | typed after a prompt                | typed with no prompt showing  |
| after the first token | identical                           | identical                     |

### The call graph: the whole parser at a glance

With every method now on the table, the parser fits in one picture. The call
graph mirrors the grammar one-to-one — each arrow below is a production
referencing another production:

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

The graph has two tiers. The **driver tier** (top) is straight-line dispatch:
`mainLoop()` looks at the *first token* of each top-level item — `def`,
`extern`, or anything else — and routes it through a `handle*` wrapper to the
matching entry point. Nothing up here recurses; it runs once per top-level
item. The **expression tier** (bottom) is where all the real structure
lives, and it is a cycle machine — three back-edges, each with a distinct
job:

- `parseParenExpr() → parseExpression()` re-enters for **grouping**: an
  expression inside parentheses is a complete, self-contained expression.
- `parseIdentifierExpr() → parseExpression()` re-enters for **call
  arguments**: every argument is a complete expression of its own.
- `parseBinOpRHS() → parseBinOpRHS(tokPrec + 1, ...)` re-enters for
  **precedence**: a tighter-binding operator run is parsed to completion
  before the looser context resumes.

Note that every cycle passes through a token-eating step before it can
repeat — which is why the recursion always terminates: each trip around a
loop consumes at least one token, and the input is finite.

Tracing one full input through the graph — `def foo(x y) x + foo(y, 4.0);`:

```
mainLoop: curTok = tok_def
└▶ handleDefinition ─▶ parseDefinition
    ├▶ parsePrototype              → "foo", params [x, y]
    └▶ parseExpression             → the body
        ├▶ parsePrimary ─▶ parseIdentifierExpr    → VariableExprAST("x")
        └▶ parseBinOpRHS(0, x): eat '+'
            ├▶ parsePrimary ─▶ parseIdentifierExpr   sees '(' → a call
            │    └▶ parseExpression (argument 1)     → VariableExprAST("y")
            │    └▶ parseExpression (argument 2)     → NumberExprAST(4.0)
            │                                        → CallExprAST("foo", ...)
            └▶ ';' has precedence -1 → merge and return
               BinaryExprAST('+', x, call)
```

— which is exactly the tree drawn at the end of §2.1, built by exactly the
`make_unique` calls this section opened with.

That completes the frontend: it validates Kaleidoscope input grammatically
and builds an AST for everything valid, but produces nothing beyond the
`Parsed a ...` reports — turning the tree into executable code starts in
Chapter 3.

## Build and run

Requirements and the other generic build facts (LLVM via Homebrew, CMake ≥
3.20 with Ninja, the deployment-target pin) are in the top-level README's
[Build and run](../README.md#build-and-run). Chapter-specific: **no LLVM is
linked at all** — like upstream's Chapter 2 `toy.cpp`, this directory is a
pure C++ frontend, so its CMake file also skips the LLVM lookup and the
deployment-target pin (LLVM first appears in Chapter3 with codegen; the lit
tests still find FileCheck through `llvm-config`, but that is test tooling,
not a build dependency).

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

Example session — the same input as the Chapter 2 tutorial's, shown as this
binary actually behaves. Because the first token is primed inside
`mainLoop()` (see the deviation subsection in "The driver loop"), no
`ready>` prompt appears until the first line has been typed:

```
$ ./build/toy
def foo(x y) x+foo(y, 4.0);
ready> Parsed a function definition.
ready> def foo(x y) x+y y;
ready> Parsed a function definition.
ready> Parsed a top-level expr
ready> def foo(x y) x+y );
ready> Parsed a function definition.
ready> Error: unknown token when expecting an expression
ready> extern sin(a);
ready> Parsed an extern
ready> ^D
ready> $
```

(The tutorial's own transcript differs in the prompt interleaving — it shows
`Parsed a function definition.` without a leading prompt, because it is
lightly tidied — but the parsed/error lines and their order are identical.
For why piped input instead bunches the prompts up — `ready> ready> Parsed a
function definition.` — see the note in the top-level README's
[Build and run](../README.md#build-and-run).)

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
