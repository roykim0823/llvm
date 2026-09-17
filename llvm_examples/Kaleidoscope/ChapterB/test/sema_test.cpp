//===- sema_test.cpp - Unit tests for diagnostics, recovery, and sema -----===//
//
// What the DiagnosticEngine buys the tests: parse a string, run sema,
// inspect the *collected* diagnostics -- severities, exact locations,
// messages -- without a process boundary, stderr scraping, or a single
// LLVM IR object. (ChapterA could only test error behavior end-to-end
// through lit, one error per run.)
//
//===----------------------------------------------------------------------===//

#include "toy/AST.h"
#include "toy/Diagnostics.h"
#include "toy/Lexer.h"
#include "toy/Parser.h"
#include "toy/Sema.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace toy;

namespace {

/// Parse `source` and resolve it, collecting diagnostics silently.
struct FrontendRun {
  DiagnosticEngine diags{[](const Diagnostic &) {}}; // no printing
  std::unique_ptr<ModuleAST> module;
  Resolutions res;
  bool semaOk = false;

  explicit FrontendRun(const std::string &source) {
    LexerBuffer lexer(source.data(), source.data() + source.size(), "test.k");
    Parser parser(lexer, diags);
    module = parser.parseModule();
    if (!diags.hadError()) {
      res = resolveModule(*module, diags);
      semaOk = !diags.hadError();
    }
  }

  const Diagnostic &diag(unsigned i) const { return diags.diagnostics()[i]; }
};

//===----------------------------------------------------------------------===//
// Clean programs
//===----------------------------------------------------------------------===//

TEST(SemaTest, CleanProgramHasNoDiagnostics) {
  FrontendRun run("extern sin(x);"
                  "def f(a b) if a < b then sin(a) else f(b, a);"
                  "f(1, 2);");
  EXPECT_TRUE(run.semaOk);
  EXPECT_EQ(run.diags.diagnostics().size(), 0u);
}

TEST(SemaTest, ScopingMirrorsIRGen) {
  // var shadowing, the for-loop variable, and params are all in scope
  // exactly where IRGen would bind them.
  FrontendRun run("def f(x) var x = x in (for i = 1, i < x in i) + x;");
  EXPECT_TRUE(run.semaOk) << "shadowing and loop scope should be legal";
}

TEST(SemaTest, RecursiveFunctionSeesItself) {
  FrontendRun run("def fib(x) if x < 3 then 1 else fib(x-1) + fib(x-2);");
  EXPECT_TRUE(run.semaOk);
}

//===----------------------------------------------------------------------===//
// Resolutions: the bindings the backend consumes
//===----------------------------------------------------------------------===//

TEST(ResolutionsTest, ShadowingCreatesDistinctVariables) {
  // The param 'x' and the 'var x' are different variables; the resolver
  // decides which one each use means, so the backend never re-scopes.
  FrontendRun run("def f(x) var x = x in x;");
  ASSERT_TRUE(run.semaOk);
  EXPECT_EQ(run.res.numVariables(), 2u);
}

TEST(ResolutionsTest, EveryDeclarationGetsAVariable) {
  // Params a and b, the loop variable i, and the var-decl c: four ids.
  FrontendRun run("def f(a b) (for i = a, i < b in i) + (var c = 1 in c);");
  ASSERT_TRUE(run.semaOk);
  EXPECT_EQ(run.res.numVariables(), 4u);
}

//===----------------------------------------------------------------------===//
// Semantic errors -- with exact locations
//===----------------------------------------------------------------------===//

TEST(SemaTest, UnknownVariable) {
  FrontendRun run("def f(x) y;");
  ASSERT_FALSE(run.semaOk);
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message, "unknown variable 'y'");
  EXPECT_EQ(run.diag(0).loc.line, 1);
  EXPECT_EQ(run.diag(0).loc.col, 10);
}

TEST(SemaTest, ForVariableOutOfScopeAfterLoop) {
  FrontendRun run("def f(n) (for i = 1, i < n in i) + i;");
  ASSERT_FALSE(run.semaOk);
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message, "unknown variable 'i'");
  EXPECT_EQ(run.diag(0).loc.col, 36) << "the use AFTER the loop, not inside";
}

TEST(SemaTest, VarInitializerSeesOuterBindingOnly) {
  // 'var a = a' where no outer 'a' exists: the initializer is checked
  // before its own name is in scope.
  FrontendRun run("def f(x) var a = a in a;");
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diag(0).message, "unknown variable 'a'");
}

TEST(SemaTest, AssignmentToNonVariable) {
  FrontendRun run("1 = 2;");
  ASSERT_FALSE(run.semaOk);
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message, "destination of '=' must be a variable");
}

TEST(SemaTest, UnknownFunction) {
  FrontendRun run("nope(1);");
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diag(0).message, "unknown function 'nope'");
}

TEST(SemaTest, CallBeforeDefinitionIsAnError) {
  // Records are checked in source order (JIT execution semantics).
  FrontendRun run("def caller(x) callee(x); def callee(x) x;");
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diag(0).message, "unknown function 'callee'");
}

TEST(SemaTest, ArityMismatch) {
  FrontendRun run("def f(a b) a + b; f(1);");
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diag(0).message,
            "incorrect number of arguments to 'f': expected 2, got 1");
}

TEST(SemaTest, UnknownOperators) {
  // Only UNARY operators can reach this check from source text: an
  // unregistered binary operator token never parses as a binary op at all,
  // because the parser owns the precedence table (the binary check in sema
  // guards hand-built ASTs).
  FrontendRun run("def f(x) !x; def g(x) $x;");
  ASSERT_FALSE(run.semaOk);
  ASSERT_EQ(run.diags.errorCount(), 2u);
  EXPECT_EQ(run.diag(0).message, "unknown unary operator '!'");
  EXPECT_EQ(run.diag(1).message, "unknown unary operator '$'");
}

TEST(SemaTest, Redefinition) {
  FrontendRun run("def f(x) x; def f(x) x + 1;");
  ASSERT_FALSE(run.semaOk);
  ASSERT_EQ(run.diags.errorCount(), 1u);
  ASSERT_EQ(run.diags.diagnostics().size(), 2u) << "error + note";
  EXPECT_EQ(run.diag(0).message, "function 'f' cannot be redefined");
  EXPECT_EQ(run.diag(1).severity, Diagnostic::Note);
  EXPECT_EQ(run.diag(1).message, "previously defined here");
  EXPECT_EQ(run.diag(1).loc.col, 5) << "note points at the FIRST definition";
}

TEST(SemaTest, ConflictingArityDeclaration) {
  // ChapterA generated IR against the old arity and failed confusingly
  // inside the body; now it is a located error with a note.
  FrontendRun run("extern f(x); def f(a b) a + b;");
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diag(0).message, "conflicting declaration of 'f': 2 "
                                 "parameters, previously declared with 1");
  EXPECT_EQ(run.diag(1).severity, Diagnostic::Note);
}

TEST(SemaTest, ExternAfterDefinitionIsFine) {
  FrontendRun run("def f(x) x; extern f(x); f(1);");
  EXPECT_TRUE(run.semaOk);
}

//===----------------------------------------------------------------------===//
// Warnings
//===----------------------------------------------------------------------===//

TEST(SemaTest, DuplicateParameterWarns) {
  FrontendRun run("def f(x x) x;");
  EXPECT_TRUE(run.semaOk) << "a warning must not fail the compile";
  ASSERT_EQ(run.diags.diagnostics().size(), 1u);
  EXPECT_EQ(run.diag(0).severity, Diagnostic::Warning);
  EXPECT_EQ(run.diag(0).message, "duplicate parameter name 'x' in 'f'");
}

//===----------------------------------------------------------------------===//
// Multiple errors per run
//===----------------------------------------------------------------------===//

TEST(SemaTest, ReportsEveryErrorInOneRun) {
  FrontendRun run("def f(a b) a + b;"
                  "def bad1(x) y;"    // unknown variable
                  "def bad2(x) f(x);" // arity
                  "1 = 2;");          // assignment destination
  ASSERT_FALSE(run.semaOk);
  EXPECT_EQ(run.diags.errorCount(), 3u);
}

//===----------------------------------------------------------------------===//
// Parser recovery (also reported through the engine)
//===----------------------------------------------------------------------===//

TEST(ParserRecoveryTest, ReportsEveryParseErrorInOneRun) {
  // (Note "x + ;" would NOT stop at the ';': like upstream, parseUnary
  // treats any ascii token as a candidate unary operator and consumes it,
  // so the error would land on whatever follows.)
  FrontendRun run("def 123(x) x;\n"  // bad prototype
                  "def ok(x) x;\n"   // parses fine between the bad ones
                  "def g( x;\n");    // unterminated parameter list
  EXPECT_EQ(run.diags.errorCount(), 2u);
  EXPECT_EQ(run.diag(0).message,
            "expected 'function name' in prototype, got a number");
  EXPECT_EQ(run.diag(1).message, "expected ')' to end prototype, got ';'");
}

TEST(ParserRecoveryTest, RecoveredRecordsSurvive) {
  FrontendRun run("def 123(x) x;\n"
                  "def ok(x) x;\n");
  ASSERT_TRUE(run.module);
  // Exactly the clean record made it into the module.
  int count = 0;
  for (auto &record : *run.module) {
    (void)record;
    ++count;
  }
  EXPECT_EQ(count, 1);
}

TEST(ParserRecoveryTest, ErrorsCarryTheTokenSpelling) {
  FrontendRun run("def f(x) (x;"); // ')' expected, ';' found
  ASSERT_EQ(run.diags.errorCount(), 1u);
  EXPECT_EQ(run.diag(0).message,
            "expected ')' to close parenthesized expression, got ';'");
}

} // namespace
