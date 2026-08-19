//===- Parser.h - Kaleidoscope Language Parser ----------------------------===//
//
// Recursive descent parser in the style of the MLIR Toy tutorial, header-only.
// It produces a well-formed AST from a stream of Tokens supplied by the
// Lexer. No semantic checks or symbol resolution are performed here (an
// undeclared variable parses successfully); those happen in codegen.
//
// The one grammar-affecting piece of state the parser owns is the binary
// operator precedence table: a 'def binary| 5 (a b) ...' registers '|' at
// parse time (before its body is parsed), so subsequent input -- including
// the operator's own recursive uses -- parses with the right precedence.
// Codegen never mutates the grammar.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_PARSER_H
#define TOY_PARSER_H

#include "toy/AST.h"
#include "toy/Lexer.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toy {

/// This is a simple recursive parser for the Kaleidoscope language. It
/// produces a well formed AST from a stream of Token supplied by the Lexer.
class Parser {
public:
  /// Create a Parser for the supplied lexer.
  Parser(Lexer &lexer) : lexer(lexer) {
    // Install the builtin binary operators (1 is lowest precedence).
    binopPrecedence['='] = 2;
    binopPrecedence['<'] = 10;
    binopPrecedence['+'] = 20;
    binopPrecedence['-'] = 20;
    binopPrecedence['*'] = 40;
  }

  /// Parse a full module: a list of function definitions, extern
  /// declarations, and top-level expressions (wrapped into anonymous
  /// functions), in source order.
  std::unique_ptr<ModuleAST> parseModule() {
    lexer.getNextToken(); // prime the lexer

    std::vector<std::unique_ptr<RecordAST>> records;
    while (true) {
      std::unique_ptr<RecordAST> record;
      switch (lexer.getCurToken()) {
      case tok_eof:
        return std::make_unique<ModuleAST>(std::move(records));
      case tok_semicolon:
        lexer.consume(tok_semicolon); // ignore top-level semicolons
        continue;
      case tok_def:
        record = parseDefinition();
        break;
      case tok_extern:
        record = parseExtern();
        break;
      default:
        record = parseTopLevelExpr();
        break;
      }
      if (!record)
        return nullptr;
      records.push_back(std::move(record));
    }
  }

private:
  Lexer &lexer;

  /// The precedence for each binary operator that is defined. Owned by the
  /// parser: user-defined operators are registered here at parse time.
  std::map<char, int> binopPrecedence;

  /// Counter used to give each anonymous top-level expression a unique
  /// function name within the module.
  int anonCount = 0;

  /// Get the precedence of the pending binary operator token, or -1 if the
  /// token is not a binary operator.
  int getTokPrecedence() {
    if (!isascii(lexer.getCurToken()))
      return -1;
    auto it = binopPrecedence.find(static_cast<char>(lexer.getCurToken()));
    if (it == binopPrecedence.end())
      return -1;
    return it->second;
  }

  /// numberexpr ::= number
  std::unique_ptr<ExprAST> parseNumberExpr() {
    auto loc = lexer.getLastLocation();
    auto result =
        std::make_unique<NumberExprAST>(std::move(loc), lexer.getValue());
    lexer.consume(tok_number);
    return result;
  }

  /// parenexpr ::= '(' expression ')'
  std::unique_ptr<ExprAST> parseParenExpr() {
    lexer.consume(Token('('));
    auto v = parseExpression();
    if (!v)
      return nullptr;
    if (lexer.getCurToken() != ')')
      return parseError<ExprAST>(")", "to close parenthesized expression");
    lexer.consume(Token(')'));
    return v;
  }

  /// identifierexpr
  ///   ::= identifier
  ///   ::= identifier '(' expression (',' expression)* ')'
  std::unique_ptr<ExprAST> parseIdentifierExpr() {
    std::string name(lexer.getIdentifier());
    auto loc = lexer.getLastLocation();
    lexer.consume(tok_identifier);

    if (lexer.getCurToken() != '(') // Simple variable ref.
      return std::make_unique<VariableExprAST>(std::move(loc), name);

    // This is a function call.
    lexer.consume(Token('('));
    ExprASTList args;
    if (lexer.getCurToken() != ')') {
      while (true) {
        if (auto arg = parseExpression())
          args.push_back(std::move(arg));
        else
          return nullptr;

        if (lexer.getCurToken() == ')')
          break;
        if (lexer.getCurToken() != ',')
          return parseError<ExprAST>(", or )", "in argument list");
        lexer.consume(tok_comma);
      }
    }
    lexer.consume(Token(')'));
    return std::make_unique<CallExprAST>(std::move(loc), name,
                                         std::move(args));
  }

  /// ifexpr ::= 'if' expression 'then' expression 'else' expression
  std::unique_ptr<ExprAST> parseIfExpr() {
    auto loc = lexer.getLastLocation();
    lexer.consume(tok_if);

    auto cond = parseExpression();
    if (!cond)
      return nullptr;

    if (lexer.getCurToken() != tok_then)
      return parseError<ExprAST>("then", "in if expression");
    lexer.consume(tok_then);

    auto thenExpr = parseExpression();
    if (!thenExpr)
      return nullptr;

    if (lexer.getCurToken() != tok_else)
      return parseError<ExprAST>("else", "in if expression");
    lexer.consume(tok_else);

    auto elseExpr = parseExpression();
    if (!elseExpr)
      return nullptr;

    return std::make_unique<IfExprAST>(std::move(loc), std::move(cond),
                                       std::move(thenExpr),
                                       std::move(elseExpr));
  }

  /// forexpr
  ///   ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
  std::unique_ptr<ExprAST> parseForExpr() {
    auto loc = lexer.getLastLocation();
    lexer.consume(tok_for);

    if (lexer.getCurToken() != tok_identifier)
      return parseError<ExprAST>("identifier", "after 'for'");
    std::string varName(lexer.getIdentifier());
    lexer.consume(tok_identifier);

    if (lexer.getCurToken() != '=')
      return parseError<ExprAST>("=", "after 'for' variable");
    lexer.consume(Token('='));

    auto start = parseExpression();
    if (!start)
      return nullptr;
    if (lexer.getCurToken() != ',')
      return parseError<ExprAST>(",", "after 'for' start value");
    lexer.consume(tok_comma);

    auto end = parseExpression();
    if (!end)
      return nullptr;

    // The step value is optional.
    std::unique_ptr<ExprAST> step;
    if (lexer.getCurToken() == ',') {
      lexer.consume(tok_comma);
      step = parseExpression();
      if (!step)
        return nullptr;
    }

    if (lexer.getCurToken() != tok_in)
      return parseError<ExprAST>("in", "after 'for' header");
    lexer.consume(tok_in);

    auto body = parseExpression();
    if (!body)
      return nullptr;

    return std::make_unique<ForExprAST>(std::move(loc), varName,
                                        std::move(start), std::move(end),
                                        std::move(step), std::move(body));
  }

  /// varexpr ::= 'var' identifier ('=' expression)?
  ///                   (',' identifier ('=' expression)?)* 'in' expression
  std::unique_ptr<ExprAST> parseVarExpr() {
    auto loc = lexer.getLastLocation();
    lexer.consume(tok_var);

    if (lexer.getCurToken() != tok_identifier)
      return parseError<ExprAST>("identifier", "after 'var'");

    std::vector<VarExprAST::VarDecl> varNames;
    while (true) {
      std::string name(lexer.getIdentifier());
      lexer.consume(tok_identifier);

      // Read the optional initializer.
      std::unique_ptr<ExprAST> init;
      if (lexer.getCurToken() == '=') {
        lexer.consume(Token('='));
        init = parseExpression();
        if (!init)
          return nullptr;
      }
      varNames.emplace_back(std::move(name), std::move(init));

      if (lexer.getCurToken() != ',')
        break;
      lexer.consume(tok_comma);
      if (lexer.getCurToken() != tok_identifier)
        return parseError<ExprAST>("identifier", "after ',' in var list");
    }

    if (lexer.getCurToken() != tok_in)
      return parseError<ExprAST>("in", "after 'var' declarations");
    lexer.consume(tok_in);

    auto body = parseExpression();
    if (!body)
      return nullptr;

    return std::make_unique<VarExprAST>(std::move(loc), std::move(varNames),
                                        std::move(body));
  }

  /// primary
  ///   ::= identifierexpr
  ///   ::= numberexpr
  ///   ::= parenexpr
  ///   ::= ifexpr
  ///   ::= forexpr
  ///   ::= varexpr
  std::unique_ptr<ExprAST> parsePrimary() {
    switch (lexer.getCurToken()) {
    case tok_identifier:
      return parseIdentifierExpr();
    case tok_number:
      return parseNumberExpr();
    case '(':
      return parseParenExpr();
    case tok_if:
      return parseIfExpr();
    case tok_for:
      return parseForExpr();
    case tok_var:
      return parseVarExpr();
    default:
      return parseError<ExprAST>("expression",
                                 "when expecting a primary expression");
    }
  }

  /// unary
  ///   ::= primary
  ///   ::= unary_operator unary
  std::unique_ptr<ExprAST> parseUnary() {
    // If the current token is not an operator, it must be a primary expr.
    if (!isascii(lexer.getCurToken()) || lexer.getCurToken() == '(' ||
        lexer.getCurToken() == ',')
      return parsePrimary();

    // If this is a unary operator, read it.
    auto loc = lexer.getLastLocation();
    char opc = static_cast<char>(lexer.getCurToken());
    lexer.getNextToken();
    if (auto operand = parseUnary())
      return std::make_unique<UnaryExprAST>(std::move(loc), opc,
                                            std::move(operand));
    return nullptr;
  }

  /// binoprhs ::= (binary_operator unary)*
  /// Operator-precedence climbing: consume operator/operand pairs as long as
  /// the pending operator binds at least as tightly as exprPrec.
  std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec,
                                         std::unique_ptr<ExprAST> lhs) {
    while (true) {
      int tokPrec = getTokPrecedence();

      // If this binop does not bind at least as tightly as the current one,
      // we are done.
      if (tokPrec < exprPrec)
        return lhs;

      char binOp = static_cast<char>(lexer.getCurToken());
      auto loc = lexer.getLastLocation();
      lexer.getNextToken(); // eat binop

      auto rhs = parseUnary();
      if (!rhs)
        return nullptr;

      // If binOp binds less tightly with rhs than the operator after rhs,
      // let the pending operator take rhs as its lhs.
      int nextPrec = getTokPrecedence();
      if (tokPrec < nextPrec) {
        rhs = parseBinOpRHS(tokPrec + 1, std::move(rhs));
        if (!rhs)
          return nullptr;
      }

      lhs = std::make_unique<BinaryExprAST>(std::move(loc), binOp,
                                            std::move(lhs), std::move(rhs));
    }
  }

  /// expression ::= unary binoprhs
  std::unique_ptr<ExprAST> parseExpression() {
    auto lhs = parseUnary();
    if (!lhs)
      return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
  }

  /// prototype
  ///   ::= identifier '(' identifier* ')'
  ///   ::= 'binary' LETTER number? '(' identifier identifier ')'
  ///   ::= 'unary' LETTER '(' identifier ')'
  std::unique_ptr<PrototypeAST> parsePrototype() {
    auto loc = lexer.getLastLocation();
    std::string fnName;

    unsigned kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
    unsigned binaryPrecedence = 30;

    switch (lexer.getCurToken()) {
    default:
      return parseError<PrototypeAST>("function name", "in prototype");
    case tok_identifier:
      fnName = std::string(lexer.getIdentifier());
      lexer.consume(tok_identifier);
      break;
    case tok_unary:
      lexer.consume(tok_unary);
      if (!isascii(lexer.getCurToken()))
        return parseError<PrototypeAST>("unary operator character",
                                        "after 'unary'");
      fnName = "unary";
      fnName += static_cast<char>(lexer.getCurToken());
      kind = 1;
      lexer.getNextToken();
      break;
    case tok_binary:
      lexer.consume(tok_binary);
      if (!isascii(lexer.getCurToken()))
        return parseError<PrototypeAST>("binary operator character",
                                        "after 'binary'");
      fnName = "binary";
      fnName += static_cast<char>(lexer.getCurToken());
      kind = 2;
      lexer.getNextToken();

      // Read the precedence if present.
      if (lexer.getCurToken() == tok_number) {
        double prec = lexer.getValue();
        if (prec < 1 || prec > 100)
          return parseError<PrototypeAST>("precedence in 1..100",
                                          "in operator prototype");
        binaryPrecedence = static_cast<unsigned>(prec);
        lexer.consume(tok_number);
      }
      break;
    }

    if (lexer.getCurToken() != '(')
      return parseError<PrototypeAST>("(", "in prototype");

    std::vector<std::string> argNames;
    while (lexer.getNextToken() == tok_identifier)
      argNames.push_back(std::string(lexer.getIdentifier()));
    if (lexer.getCurToken() != ')')
      return parseError<PrototypeAST>(")", "to end prototype");
    lexer.consume(Token(')'));

    // Verify right number of names for operator.
    if (kind && argNames.size() != kind)
      return parseError<PrototypeAST>("correct number of operands",
                                      "for operator prototype");

    return std::make_unique<PrototypeAST>(std::move(loc), fnName,
                                          std::move(argNames), kind != 0,
                                          binaryPrecedence);
  }

  /// definition ::= 'def' prototype expression
  std::unique_ptr<RecordAST> parseDefinition() {
    lexer.consume(tok_def);
    auto proto = parsePrototype();
    if (!proto)
      return nullptr;

    // A user-defined binary operator becomes part of the grammar as soon as
    // its prototype is parsed -- before the body, which may use it
    // recursively, and before any subsequent input.
    bool installedOp = false;
    if (proto->isBinaryOp()) {
      binopPrecedence[proto->getOperatorName()] =
          proto->getBinaryPrecedence();
      installedOp = true;
    }

    auto body = parseExpression();
    if (!body) {
      // The operator was never really defined; unregister it so later input
      // does not parse against a function that will not exist.
      if (installedOp)
        binopPrecedence.erase(proto->getOperatorName());
      return nullptr;
    }
    return std::make_unique<FunctionAST>(std::move(proto), std::move(body));
  }

  /// external ::= 'extern' prototype
  std::unique_ptr<RecordAST> parseExtern() {
    lexer.consume(tok_extern);
    auto proto = parsePrototype();
    if (!proto)
      return nullptr;
    return std::make_unique<ExternAST>(std::move(proto));
  }

  /// toplevelexpr ::= expression
  /// Wrapped into an anonymous function with a unique name so several
  /// top-level expressions can coexist in one module.
  std::unique_ptr<RecordAST> parseTopLevelExpr() {
    auto loc = lexer.getLastLocation();
    auto expr = parseExpression();
    if (!expr)
      return nullptr;

    std::string name = "__anon_expr" + std::to_string(anonCount++);
    auto proto = std::make_unique<PrototypeAST>(std::move(loc), name,
                                                std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(proto), std::move(expr),
                                         /*topLevelExpr=*/true);
  }

  /// Helper function to signal errors while parsing. It takes an argument
  /// indicating the expected token and another for more context. Location is
  /// retrieved from the lexer and printed with the message. The explicit
  /// template parameter is the return type of the caller, so the call site
  /// reads as a drop-in `return parseError<ExprAST>(...)`.
  template <typename R, typename T, typename U = const char *>
  std::unique_ptr<R> parseError(T &&expected, U &&context = "") {
    auto curToken = lexer.getCurToken();
    llvm::errs() << "Parse error (" << lexer.getLastLocation().line << ", "
                 << lexer.getLastLocation().col << "): expected '" << expected
                 << "' " << context << " but has Token " << curToken;
    if (isprint(curToken))
      llvm::errs() << " '" << (char)curToken << "'";
    llvm::errs() << "\n";
    return nullptr;
  }
};

} // namespace toy

#endif // TOY_PARSER_H
