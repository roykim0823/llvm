#include "Parser.h"

std::unique_ptr<AST> Parser::parse() {
  auto Res = parseCalc();
  expect(Token::eoi);
  return Res;
}

// calc ::= 'with' ident (',' ident)* ':')? expr;
std::unique_ptr<AST>  Parser::parseCalc() {
  std::unique_ptr<Expr> E;  // Early declaration due to goto usage
  llvm::SmallVector<llvm::StringRef, 8> Vars;
  if (Tok.is(Token::KW_with)) {
    advance();
    if (expect(Token::ident))
      goto _error;
    Vars.push_back(Tok.getText());
    advance();
    while (Tok.is(Token::comma)) {
      advance();
      if (expect(Token::ident))
        goto _error;
      Vars.push_back(Tok.getText());
      advance();
    }
    if (consume(Token::colon))
      goto _error;
  }

  // Parse the expression part
  E = parseExpr();
  if (expect(Token::eoi))
    goto _error;
  if (Vars.empty())
    return E;  // unique_ptr<Expr> implicitly converts to unique_ptr<AST>
  else
    return std::make_unique<WithDecl>(Vars, std::move(E));  // return the created AST WithDecl(Vars, E) node

_error:
  while (Tok.getKind() != Token::eoi)
    advance();
  return nullptr;
}

// expr : term (( "+" | "-" ) term)* ;
std::unique_ptr<Expr> Parser::parseExpr() {
  auto Left = parseTerm();
  // the use of isOneOf() to simplify the check for several tokens
  while (Tok.isOneOf(Token::plus, Token::minus)) {
    BinaryOp::Operator Op = Tok.is(Token::plus)
                                ? BinaryOp::Plus
                                : BinaryOp::Minus;
    advance();
    auto Right = parseTerm();
    Left = std::make_unique<BinaryOp>(Op, std::move(Left), std::move(Right));
  }
  return Left;
}

// term : factor ( ( "*" | "/" ) factor )* ;
std::unique_ptr<Expr> Parser::parseTerm() {
  auto Left = parseFactor();
  while (Tok.isOneOf(Token::star, Token::slash)) {
    BinaryOp::Operator Op =
        Tok.is(Token::star) ? BinaryOp::Mul : BinaryOp::Div;
    advance();
    auto Right = parseFactor();
    Left = std::make_unique<BinaryOp>(Op, std::move(Left), std::move(Right));
  }
  return Left;
}

// factor : ident | number | "(" expr ")" ;
std::unique_ptr<Expr> Parser::parseFactor() {
  std::unique_ptr<Expr> Res = nullptr;
  switch (Tok.getKind()) {
  case Token::number:
    Res = std::make_unique<Factor>(Factor::Number, Tok.getText());
    advance(); break;
  case Token::ident:
    Res = std::make_unique<Factor>(Factor::Ident, Tok.getText());
    advance(); break;
  case Token::l_paren:
    advance();
    Res = parseExpr();
    if (!consume(Token::r_paren)) break;
  default:
    if (!Res)
      error();
    // The guard prevents a second error message on a syntax error
    // from the parenthesis's expression
    while (!Tok.isOneOf(Token::r_paren, Token::star,
                        Token::plus, Token::minus,
                        Token::slash, Token::eoi))
      advance();
  }
  return Res;
}
