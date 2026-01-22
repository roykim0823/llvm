#ifndef PARSER_H
#define PARSER_H

#include "AST.h"
#include "Lexer.h"
#include "llvm/Support/raw_ostream.h"

class Parser {
  Lexer &Lex;
  Token Tok;
  bool HasError;

  void error() {
    llvm::errs() << "Unexpected: " << Tok.getText() << "\n";
    HasError = true;
  }

  void advance() { Lex.next(Tok); }

  // Expect the current token to be of the given kind.
  // If so, return false. Otherwise, report an error and return true.  
  bool expect(Token::TokenKind Kind) {  
    if (!Tok.is(Kind)) {
      error();
      return true;
    }
    return false;
  }

  bool consume(Token::TokenKind Kind) {
    if (expect(Kind))
      return true;
    advance();
    return false;
  }

  // For each non-terminal of the grammar,
  // a method to parse the rule is declared
  AST *parseCalc();
  Expr *parseExpr();
  Expr *parseTerm();
  Expr *parseFactor();

public:
  Parser(Lexer &Lex) : Lex(Lex), HasError(false) {
    advance();
  }
  AST *parse();  // main entry point
  bool hasError() { return HasError; }
};

#endif