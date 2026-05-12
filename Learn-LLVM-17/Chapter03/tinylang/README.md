# tinylang (Chapter 3)

`tinylang` is a small Modula-2-style compiler frontend built on top of LLVM
support libraries. Chapter 3 introduces the project and implements **the
complete frontend pipeline**: source → tokens → parse + semantic analysis →
typed AST. Code generation arrives in Chapter 4; types like `ARRAY`, `RECORD`
and `POINTER` arrive in Chapter 5.

The Ch03 driver is intentionally a *frontend-only* driver — it reads `.mod`
files, runs them through Lexer → Parser → Sema, and reports diagnostics. There
is no `-o` flag and no IR is emitted.

## Pipeline

```
        +----------+   tokens   +--------+   AST + Decls   +------+
.mod -> |  Lexer   |  -------> | Parser | --------------> | Sema |
        +----------+            +--------+ ^               +------+
              ^                            |   actOn*()        |
              |                            +-------------------+
              | DiagnosticsEngine (errors, notes, warnings)
              v
          SourceMgr  (LLVM's source-buffer & SMLoc machinery)
```

`Driver.cpp` wires `SourceMgr`, `DiagnosticsEngine`, `Lexer`, `Sema`, and
`Parser` together and calls `Parser::parse()` for each input file. The Parser
is *syntax-directed*: every successful production calls a corresponding
`Sema::actOn…` method to create AST nodes, insert declarations into the
current `Scope`, and report semantic errors via `DiagnosticsEngine`.

## Directory layout

```
Chapter03/tinylang/
├── CMakeLists.txt          top-level: finds LLVM, sets up subdirs
├── cmake/                  AddTinylang helpers
├── include/tinylang/       public headers (the API surface)
│   ├── AST/AST.h           AST node hierarchy (Decl, Expr, Stmt)
│   ├── Basic/              cross-cutting infrastructure
│   │   ├── Diagnostic.{h,def}   diagnostic IDs + reporter
│   │   ├── TokenKinds.{h,def}   token enum + spelling lookups
│   │   ├── LLVM.h               re-exports of llvm:: types
│   │   └── Version.h            version string
│   ├── Lexer/              lexer + Token
│   ├── Parser/Parser.h     recursive-descent parser
│   └── Sema/               semantic analyzer + lexical Scope
├── lib/                    implementations mirroring include/tinylang
├── tools/driver/Driver.cpp the `tinylang` executable
├── test/                   gtest unit tests (one per module)
├── example/Gcd.mod         sample input
└── docs/                   Doxygen config (see "Generating Doxygen" below)
```

## What each module does

### Basic — cross-cutting infrastructure

| File | Role |
| --- | --- |
| `LLVM.h` | Brings `llvm::SMLoc`, `StringRef`, `cast/dyn_cast/isa`, etc. into `tinylang::`. Saves typing throughout the rest of the project. |
| `TokenKinds.{h,def}` | Single source of truth for every token. `TokenKinds.def` lists each `TOK`/`PUNCTUATOR`/`KEYWORD`; the header X-macro-expands it into `enum tok::TokenKind` and provides `getTokenName / getPunctuatorSpelling / getKeywordSpelling`. |
| `Diagnostic.{h,def}` | `diag::*` IDs are listed in `Diagnostic.def` (`err_*`, `note_*`, `warn_*`). `DiagnosticsEngine` formats messages via `llvm::formatv`, hands them to `SourceMgr::PrintMessage`, and counts errors. The Parser checks `numErrors()` at the end. |
| `Version.{h,inc.in}` | Generated version string for `--version`. |

### Lexer — characters → `Token`s

| Class | Role |
| --- | --- |
| `Token` | A token: a `(Ptr, Length, Kind)` triple. `is/isNot/isOneOf` predicates; `getLocation()` returns an `SMLoc` pointing into the source buffer; `getIdentifier()` / `getLiteralData()` return the spelling for identifiers and literals. |
| `KeywordFilter` | `StringMap<TokenKind>` populated from `TokenKinds.def` so the lexer can decide whether an identifier is really a keyword. |
| `Lexer` | Owns the current scan pointer and dispatches on the first character to `identifier()`, `number()`, `string()`, `comment()`, or builds punctuators directly via `formToken()`. `next(Token&)` is the main entry; diagnostics for unterminated comments/strings and hex-in-decimal go through `DiagnosticsEngine`. |

### Parser — recursive-descent

`Parser` holds a reference to a `Lexer`, the look-ahead token `Tok`, and a
reference to `Sema` (called `Actions` — the parser doesn't build AST nodes
itself; Sema does, in response to `actOn…` calls).

Helpers (`protected` so tests can call them):

- `advance()` — pull the next token from the lexer.
- `expect(K)` — return `false` if `Tok.is(K)`, else emit `err_expected` and return `true`.
- `consume(K)` — like `expect` but advances on a match.
- `skipUntil(Toks...)` — error recovery: skip tokens until any of `Toks` (or EOF).

The `parseXxx` methods follow the Modula-2 grammar:

```
parseCompilationUnit   ::= MODULE id ; { import } block id .
parseImport            ::= [ FROM id ] IMPORT id { , id } ;
parseBlock             ::= { declaration } [ BEGIN statementSequence ] END
parseDeclaration       ::= CONST … | VAR … | PROCEDURE …
parseStatement         ::= assignment | call | IF … | WHILE … | RETURN …
parseExpression        ::= simpleExpr [ relop simpleExpr ]
parseSimpleExpression  ::= [+|-] term { addop term }
parseTerm              ::= factor { mulop factor }
parseFactor            ::= int | string | ( expr ) | NOT factor | qualident [ ( expList ) ]
```

Each `parseXxx` returns `true` on error (the caller usually calls `skipUntil`
to resync). On success it calls the corresponding `Sema::actOn…` to attach an
AST node to its out-parameter (`Decl *&`, `Expr *&`, etc.).

### Sema — symbol tables, types, AST construction

| Class | Role |
| --- | --- |
| `Scope` | A `StringMap<Decl *>` plus a parent pointer. `insert(D)` fails on a duplicate; `lookup(name)` walks the parent chain. |
| `EnterDeclScope` | RAII helper: ctor calls `Sema::enterScope`, dtor calls `leaveScope`. Used in `parseCompilationUnit` and `parseProcedureDeclaration` so nested scopes are popped automatically. |
| `Sema` | The action object. Tracks `CurrentScope`, `CurrentDecl` (the enclosing module/procedure for new declarations), and a reference to `DiagnosticsEngine`. Pre-populates the global scope with `INTEGER`, `BOOLEAN`, `TRUE`, `FALSE` in `initialize()`. |

Every grammar rule has a matching `actOn…` method (see the table in
`include/tinylang/Sema/Sema.h`). Roughly:

- **declarations**: `actOnModuleDeclaration`, `actOnConstantDeclaration`,
  `actOnVariableDeclaration`, `actOnFormalParameterDeclaration`,
  `actOnProcedureDeclaration` (two overloads: heading-only and final),
  `actOnProcedureHeading`.
- **statements**: `actOnAssignment`, `actOnProcCall`, `actOnIfStatement`,
  `actOnWhileStatement`, `actOnReturnStatement`.
- **expressions**: `actOnExpression` (relops),
  `actOnSimpleExpression` (`+/-/OR`), `actOnTerm` (`*//DIV/MOD/AND`),
  `actOnPrefixExpression` (`-`, `NOT`), `actOnIntegerLiteral`,
  `actOnVariable`, `actOnFunctionCall`.
- **name resolution**: `actOnQualIdentPart` — looks a name up in the current
  scope (or a module's scope, for `Mod.x` references).

`Sema` performs type-compatibility checks (`isOperatorForType`), enforces that
IF/WHILE conditions are `BOOLEAN`, that RETURN values match the function's
return type, and that VAR parameters get variable arguments. Errors are
reported but Sema still returns a node where possible so parsing can continue.

### AST — node hierarchy

All AST nodes use LLVM-style RTTI: a private `Kind` enum + `classof(const
T*)` so `llvm::isa / dyn_cast` work.

**Declarations** (`include/tinylang/AST/AST.h`, base `class Decl`):

```
Decl
 ├── ModuleDeclaration         MODULE … END Name.
 ├── ConstantDeclaration       CONST name = expr
 ├── TypeDeclaration           predeclared INTEGER / BOOLEAN (Ch3)
 ├── VariableDeclaration       VAR name : type
 ├── FormalParameterDeclaration  ( [VAR] name : type )
 └── ProcedureDeclaration      PROCEDURE name(params) [: ret] ; body END name
```

**Expressions** (base `class Expr`, carries `Type` + `IsConstant` flag):

```
Expr
 ├── InfixExpression       a op b
 ├── PrefixExpression      op a
 ├── IntegerLiteral        APSInt value
 ├── BooleanLiteral        TRUE / FALSE
 ├── VariableAccess        reference to a var or param decl
 ├── ConstantAccess        reference to a CONST decl
 └── FunctionCallExpr      f(args)  (must return a value)
```

`OperatorInfo` bundles `(SMLoc, TokenKind)` for an operator, plus an
`IsUnspecified` bit (so a default-constructed instance is meaningful).

**Statements** (base `class Stmt`):

```
Stmt
 ├── AssignmentStatement    var := expr
 ├── ProcedureCallStatement p(args)  (no return value used)
 ├── IfStatement            IF cond THEN … [ELSE …] END
 ├── WhileStatement         WHILE cond DO … END
 └── ReturnStatement        RETURN [expr]
```

Supporting types: `Ident` (location + name pair) and the `using` aliases
`DeclList`, `StmtList`, `ExprList`, `FormalParamList`, `IdentList` —
all `std::vector` of pointers / pairs.

## Build & run

```bash
# from Chapter03/tinylang
./build.sh          # configures and builds via CMake/Ninja
./run.sh            # runs ./build/tools/driver/tinylang ./example/Gcd.mod
./test.sh           # ctest in ./build/test
```

`Driver.cpp` accepts one or more `.mod` files as positional arguments. Output
is just the version banner and any diagnostics — successful parses are silent.

## Tests

Unit tests live under `test/` and are built into individual GoogleTest
binaries that `ctest` discovers. GoogleTest and GoogleMock are pulled in via
`FetchContent` (see `test/CMakeLists.txt`); no system install is required.

| File | Targets | What it covers |
| --- | --- | --- |
| `Diagnostic_test.cpp` | `DiagnosticsEngine` | Iterates every entry of `Diagnostic.def` (via the same X-macro the engine uses) and verifies that reporting it does/doesn't bump the error count for its declared severity. |
| `TokenKinds_test.cpp` | `tok::get*` helpers | One row per token kind from `TokenKinds.def`; checks name, punctuator-spelling, and keyword-spelling lookups. |
| `Lexer_test.cpp` | `Lexer`, `KeywordFilter` | Parametric token-recognition table (identifiers, every keyword, every punctuator — including the `:` vs `:=` / `<` vs `<=` / `>` vs `>=` lookahead pairs), plus targeted cases for whitespace, nested comments, empty-input EOF, an unknown character, multi-token sequences, source-location preservation, and the three lexical-error diagnostics. |
| `Parser_test.cpp` | `Parser` (with mocked `Sema`) | Subclasses `Sema` with gmock so the test can assert *which* `actOn…` methods the parser invokes for a given input. Drives `parseCompilationUnit` and `parseImport` over both well-formed and malformed snippets. Relies on `Sema`'s virtual entry points and `ModuleDeclaration`'s default ctor. |
| `Scope_test.cpp` | `Scope` | Scripts of insert/lookup actions across a chain of nested scopes; verifies shadowing, parent walks, deep nesting, and case sensitivity. |
| `Sema_test.cpp` | `Sema` | Calls `actOn…` methods directly and asserts on the produced AST kind plus diagnostics. Covers: term type-checking (`actOnTerm`), additive type-checking (`actOnSimpleExpression`), relational and prefix expressions, integer-literal radix decoding, IF/WHILE condition typing, assignment type compatibility, duplicate symbols (`actOnConstantDeclaration`), undeclared-name resolution (`actOnQualIdentPart`), and the Ch3 `actOnImport` stub. |

Run them with `./test.sh` (= `ctest` inside `build/test`), or invoke a single
binary for finer-grained output, e.g.:

```bash
./build/test/Lexer_test --gtest_filter='LexerTest.HandlesNestedComments'
```

Test files **are** picked up by Doxygen: the `test/` directory is part of the
`INPUT` list in `docs/Doxyfile` so the file-level `/// \file` summaries show
up under the "Files" tab next to the rest of the source. The gtest helper
types (`MockSema`, `LexerParam`, `ScopeAction`, …) are kept out of the class
index via `EXCLUDE_SYMBOLS` so the API surface stays clean.

## Generating Doxygen documentation

A pre-configured `docs/Doxyfile` is included. It scans `include/` and `lib/`,
recognises both `///` and `/** */` comments, and writes HTML to
`docs/html/`.

```bash
# from Chapter03/tinylang
brew install doxygen          # macOS; or: apt install doxygen graphviz
doxygen docs/Doxyfile         # builds docs/html and docs/latex
open docs/html/index.html     # macOS
```

If `dot` (graphviz) is installed, class hierarchy and collaboration graphs
are produced; otherwise Doxygen falls back to text.

### Optional CMake integration

The top-level `CMakeLists.txt` is **not** modified by default. If you want a
`make docs` target, add this snippet to it:

```cmake
find_package(Doxygen)
if(DOXYGEN_FOUND)
  add_custom_target(docs
    COMMAND Doxygen::doxygen ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Doxygen documentation"
    VERBATIM)
endif()
```

Then `cmake --build build --target docs` builds the docs out-of-tree.

### Regenerating a fresh Doxyfile

The shipped `docs/Doxyfile` was created from the Doxygen 1.10 defaults and
then customised (see `INPUT`, `RECURSIVE`, `EXTRACT_ALL`, `GENERATE_HTML`,
`HAVE_DOT` near the top). To regenerate from scratch:

```bash
cd docs
doxygen -g Doxyfile.new      # writes a default Doxyfile.new alongside ours
diff Doxyfile Doxyfile.new   # see what we changed
```

## Doxygen comment style used here

Headers use `///` for single-line summaries and `/** … */` for multi-paragraph
descriptions. Recognised tags:

- `\file`   — top-of-file purpose summary.
- `\brief`  — one-line description (also derived from the first sentence of a
              `///` block when `JAVADOC_AUTOBRIEF = YES`).
- `\param`  — describes one function parameter.
- `\returns` / `\return` — describes the return value.
- `\note`, `\warning`, `\see` — cross-references and call-outs.

Doxygen also picks up `///<` for *trailing* comments on enum values or
fields — see `tok::TokenKind` in `Basic/TokenKinds.def` for examples once
documented, or `Decl::DK_*` in `AST/AST.h`.
