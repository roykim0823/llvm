#include <cstdio>

#include "parser.h"
#include "log.h"

using namespace toy;

// Global Variables
/// BinopPrecedence - This holds the precedence for each binary operator that is
/// defined.    
std::map<char, int> BinopPrecedence;
extern std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;  // define in ast.cpp
// ------------------------------

void init_binop() {
    // Install standard binary operators.
    // 1 is lowest precedence.
    BinopPrecedence['='] = 2;
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 20;
    BinopPrecedence['*'] = 40;  
}

Parser::Parser(Lexer& lexer, CodegenContext& ctx) : lexer(lexer), ctx(ctx) {
    init_binop();
}

// Helper to bridge the Lexer to the Parser's CurTok
int Parser::getNextToken() { 
    return curTok = lexer.gettok(); 
}

int Parser::getTokPrecedence() {
    if (!isascii(curTok)) return -1;
    // auto it = binopPrecedence.find(static_cast<char>(curTok));
    // if (it == binopPrecedence.end()) return -1;
    // return it->second;

    // Make sure it's a declared binop.
    int TokPrec = BinopPrecedence[curTok];
    if (TokPrec == 0) return -1;
    return TokPrec;
}


// The routine eats all of the tokens that correspond to the production and returns the lexer buffer 
// This is a fairly standard recursive descent parser structure.
// numberexpr ::= number
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
    auto result = std::make_unique<NumberExprAST>(lexer.getNumVal());
    getNextToken(); 
    return std::move(result);
}

// parenexpr ::= '(' expression ')'
std::unique_ptr<ExprAST> Parser::parseParenExpr() {
    // This eats the ( ) tokens. Once the parser constructs the AST, parentheses are not needed!
    getNextToken(); // eat (
    auto v = parseExpression();     // recursion by calling parseExpression -> parseParenExpr
                                    // This is powerful as it allows nested expressions/recursive grammars
    if (!v) return nullptr;

    if (curTok != ')') return logError("expected ')'");
    getNextToken(); // eat )
    return v;
}

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

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
std::unique_ptr<ExprAST> Parser::parseIfExpr()  {
  getNextToken(); // eat the if.

  // condition.
  auto Cond = parseExpression();
  if (!Cond)
    return nullptr;

  if (curTok != tok_then)
    return logError("expected then");
  getNextToken(); // eat the then

  auto Then = parseExpression();
  if (!Then)
    return nullptr;

  if (curTok != tok_else)
    return logError("expected else");

  getNextToken();

  auto Else = parseExpression();
  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                      std::move(Else));
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
std::unique_ptr<ExprAST> Parser::parseForExpr() {
  getNextToken(); // eat the for.

  if (curTok != tok_identifier)
    return logError("expected identifier after for");

  std::string IdName = lexer.getIdentifierStr();
  getNextToken(); // eat identifier.

  if (curTok != '=')
    return logError("expected '=' after for");
  getNextToken(); // eat '='.

  auto Start = parseExpression();
  if (!Start)
    return nullptr;
  if (curTok != ',')
    return logError("expected ',' after for start value");
  getNextToken();

  auto End = parseExpression();
  if (!End)
    return nullptr;

  // The step value is optional.
  std::unique_ptr<ExprAST> Step;
  if (curTok == ',') {
    getNextToken();
    Step = parseExpression();
    if (!Step)
      return nullptr;
  }

  if (curTok != tok_in)
    return logError("expected 'in' after for");
  getNextToken(); // eat 'in'.

  auto Body = parseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
                                       std::move(Step), std::move(Body));
}

/// varexpr ::= 'var' identifier ('=' expression)?
//                    (',' identifier ('=' expression)?)* 'in' expression
std::unique_ptr<ExprAST> Parser::parseVarExpr() {
  getNextToken(); // eat the var.

  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

  // At least one variable name is required.
  if (curTok != tok_identifier)
    return logError("expected identifier after var");

  while (true) {
    std::string Name = lexer.getIdentifierStr();
    getNextToken(); // eat identifier.

    // Read the optional initializer.
    std::unique_ptr<ExprAST> Init = nullptr;
    if (curTok == '=') {
      getNextToken(); // eat the '='.

      Init = parseExpression();
      if (!Init)
        return nullptr;
    }

    VarNames.push_back(std::make_pair(Name, std::move(Init)));

    // End of var list, exit loop.
    if (curTok != ',')
      break;
    getNextToken(); // eat the ','.

    if (curTok != tok_identifier)
      return logError("expected identifier list after var");
  }

  // At this point, we have to have 'in'.
  if (curTok != tok_in)
    return logError("expected 'in' keyword after 'var'");
  getNextToken(); // eat 'in'.

  auto Body = parseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<VarExprAST>(std::move(VarNames), std::move(Body));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
///   ::= ifexpr
///   ::= forexpr
///   ::= varexpr
std::unique_ptr<ExprAST> Parser::parsePrimary() {
    switch (curTok) {
    case tok_identifier: return parseIdentifierExpr();
    case tok_number:     return parseNumberExpr();
    case '(':            return parseParenExpr();
    case tok_if:         return parseIfExpr();
    case tok_for:        return parseForExpr();
    case tok_var:        return parseVarExpr();
    default:             return logError("unknown token when expecting an expression");
    }
}

/// unary
///   ::= primary
///   ::= '!' unary
std::unique_ptr<ExprAST> Parser::parseUnary() {
  // If the current token is not an operator, it must be a primary expr.
  if (!isascii(curTok) || curTok == '(' || curTok == ',')
    return parsePrimary();

  // If this is a unary operator, read it.
  int Opc = curTok;
  getNextToken();
  if (auto Operand = parseUnary())
    return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
  return nullptr;
}


// binoprhs ::= ('+' primary)*
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        // If this is a binop, find its precedence.
        int tokPrec = getTokPrecedence();

        // If this is a binop that binds at least as tightly as the current binop,
        // consume it, otherwise we are done.
        if (tokPrec < exprPrec) return lhs;

        // Okay, we know this is a binop.
        int binOp = curTok;
        getNextToken(); // eat binop

        // Parse the unary expression after the binary operator.
        auto rhs = parseUnary();
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

// expression
//   ::= primary binoprhs
std::unique_ptr<ExprAST> Parser::parseExpression() {
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
  std::string FnName;

  unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
  unsigned BinaryPrecedence = 30;

  switch (curTok) {
  default:
    return logErrorP("Expected function name in prototype");
  case tok_identifier:
    FnName = lexer.getIdentifierStr();
    Kind = 0;
    getNextToken();
    break;
  case tok_unary:
    getNextToken();
    if (!isascii(curTok))
      return logErrorP("Expected unary operator");
    FnName = "unary";
    FnName += (char)curTok;
    Kind = 1;
    getNextToken();
    break;
  case tok_binary:
    getNextToken();
    if (!isascii(curTok))
      return logErrorP("Expected binary operator");
    FnName = "binary";
    FnName += (char)curTok;
    Kind = 2;
    getNextToken();

    // Read the precedence if present.
    if (curTok == tok_number) {
      if (lexer.getNumVal() < 1 || lexer.getNumVal() > 100)
        return logErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = (unsigned)lexer.getNumVal();
      getNextToken();
    }
    break;
  }

  if (curTok != '(')
    return logErrorP("Expected '(' in prototype");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(lexer.getIdentifierStr());
  if (curTok != ')')
    return logErrorP("Expected ')' in prototype");

  // success.
  getNextToken(); // eat ')'.

  // Verify right number of names for operator.
  if (Kind && ArgNames.size() != Kind)
    return logErrorP("Invalid number of operands for operator");

  return std::make_unique<PrototypeAST>(FnName, ArgNames, Kind != 0,
                                         BinaryPrecedence);
}

// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> Parser::parseDefinition() {
    getNextToken(); // eat def
    auto proto = parsePrototype();
    if (!proto) return nullptr;

    if (auto e = parseExpression())
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    return nullptr;
}

// top-level expression ::= expression
std::unique_ptr<FunctionAST> Parser::parseTopLevelExpr() {
    if (auto e = parseExpression()) {
        auto proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
    }
    return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> Parser::parseExtern() {
    getNextToken(); // eat extern
    return parsePrototype();
}

void Parser::handleDefinition() {
  if (auto FnAST = parseDefinition()) {
    if (auto *FnIR = FnAST->codegen(ctx)) {
      fprintf(stderr, "Read function definition:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

void Parser::handleExtern() {
  if (auto ProtoAST = parseExtern()) {
    if (auto *FnIR = ProtoAST->codegen(ctx)) {
      fprintf(stderr, "Read extern:\n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

void Parser::handleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = parseTopLevelExpr()) {
    FnAST->codegen(ctx);
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

void Parser::mainLoop() {
    while (true) {        
        switch (curTok) {
        case tok_eof: return;
        case ';':     getNextToken(); break;  // ignore top-level semicolons.
        case tok_def: handleDefinition(); break;
        case tok_extern: handleExtern(); break;
        default:      handleTopLevelExpression(); break;
        }
        
    }
    // Print out all of the generated code.
    ctx.theModule->print(llvm::errs(), nullptr);

}