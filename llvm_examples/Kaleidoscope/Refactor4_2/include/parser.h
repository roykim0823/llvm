#ifndef PARSER_H
#define PARSER_H

#include <map>
#include <memory>

#include "ast.h"
#include "ir_gen_ctx.h"
#include "lexer.h"

namespace toy {

class Parser {
public:
    Parser(Lexer& lexer, IRGenContext& ctx) : lexer(lexer), ctx(ctx) {
        binopPrecedence['<'] = 10;
        binopPrecedence['+'] = 20;
        binopPrecedence['-'] = 20;
        binopPrecedence['*'] = 40;
    }

    void mainLoop();
    int getNextToken();  // Reada another token from the lexer and updates curTok
    int getTokPrecedence();

    std::unique_ptr<ExprAST> parseExpression();
    std::unique_ptr<ExprAST> parseNumberExpr();
    std::unique_ptr<ExprAST> parseParenExpr();
    std::unique_ptr<ExprAST> parseIdentifierExpr();
    std::unique_ptr<ExprAST> parsePrimary();  // simple warpper for numberexpr/identifierexpr/parenexpr
    std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs);  // called by parseExpression
    std::unique_ptr<PrototypeAST> parsePrototype();
    std::unique_ptr<FunctionAST> parseDefinition();
    std::unique_ptr<FunctionAST> parseTopLevelExpr();  // simple wrapper for top-level-expression
    std::unique_ptr<PrototypeAST> parseExtern();

    // Top-level parsing and JIT driver
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();

  private:
    Lexer& lexer;
    IRGenContext& ctx;
    /// CurTok/getNextToken - Provide a simple token buffer.
    int curTok;  // Current token the parser is looking at

    /// BinopPrecedence - This holds the precedence for each binary operator that is
    /// defined.
    std::map<char, int> binopPrecedence;
};

} // end namespace toy

#endif