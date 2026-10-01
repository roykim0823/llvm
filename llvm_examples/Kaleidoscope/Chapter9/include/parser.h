#ifndef PARSER_H
#define PARSER_H

#include <map>
#include <memory>

#include "ast.h"
#include "lexer.h"

namespace toy {

class Parser {
public:
    Parser(Lexer& lexer) : lexer(lexer) {
        binopPrecedence['='] = 2;  // Mutable Variable: assignment binds loosest
        binopPrecedence['<'] = 10;
        binopPrecedence['+'] = 20;
        binopPrecedence['-'] = 20;
        binopPrecedence['*'] = 40;
    }

    int getCurToken() const { return curTok; }  // The token the parser is looking at
    int getNextToken();  // Reads another token from the lexer and updates curTok
    int getTokPrecedence();

    std::unique_ptr<ExprAST> parseExpression();
    std::unique_ptr<ExprAST> parseNumberExpr();
    std::unique_ptr<ExprAST> parseParenExpr();
    std::unique_ptr<ExprAST> parseIdentifierExpr();
    std::unique_ptr<ExprAST> parseUnary();  // for user-defined operators
    std::unique_ptr<ExprAST> parseIfExpr();
    std::unique_ptr<ExprAST> parseForExpr();
    std::unique_ptr<ExprAST> parseVarExpr();  // to parse mutable variable
    std::unique_ptr<ExprAST> parsePrimary();  // simple wrapper for numberexpr/identifierexpr/parenexpr
    std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs);  // called by parseExpression
    std::unique_ptr<PrototypeAST> parsePrototype();
    std::unique_ptr<FunctionAST> parseDefinition();
    std::unique_ptr<FunctionAST> parseTopLevelExpr();  // simple wrapper for top-level-expression
    std::unique_ptr<PrototypeAST> parseExtern();

  private:
    Lexer& lexer;
    /// CurTok/getNextToken - Provide a simple token buffer.
    int curTok;  // Current token the parser is looking at

    /// BinopPrecedence - This holds the precedence for each binary operator that is
    /// defined. The four builtins are installed by the constructor; from Chapter 6
    /// on, parseDefinition() installs user-defined operators here as soon as their
    /// prototype is parsed, so the rest of the input parses with them in place.
    std::map<char, int> binopPrecedence;
};

} // end namespace toy

#endif
