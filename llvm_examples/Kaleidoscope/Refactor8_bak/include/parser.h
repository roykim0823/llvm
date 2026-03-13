#ifndef PARSER_H
#define PARSER_H

#include <map>
#include <memory>

#include "ast.h"
#include "codegen_ctx.h"
#include "lexer.h"


namespace toy {

class Parser {
public:
    Parser(Lexer& lexer, CodegenContext& ctx);

    void mainLoop();
    int getNextToken();  // Reada another token from the lexer and updates curTok

private:
    Lexer& lexer;
    CodegenContext& ctx;

    /// CurTok/getNextToken - Provide a simple token buffer.    
    int curTok;  // Current token the parser is looking at

    int getTokPrecedence();

    std::unique_ptr<ExprAST> parseExpression();
    std::unique_ptr<ExprAST> parseNumberExpr();
    std::unique_ptr<ExprAST> parseParenExpr();
    std::unique_ptr<ExprAST> parseIdentifierExpr();
    std::unique_ptr<ExprAST> parseIfExpr();
    std::unique_ptr<ExprAST> parseForExpr();
    std::unique_ptr<ExprAST> parsePrimary();
    std::unique_ptr<ExprAST> parseUnary();
    std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs);
    std::unique_ptr<ExprAST> parseVarExpr();
    std::unique_ptr<PrototypeAST> parsePrototype();
    std::unique_ptr<FunctionAST> parseDefinition();
    std::unique_ptr<FunctionAST> parseTopLevelExpr();
    std::unique_ptr<PrototypeAST> parseExtern();

    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();
};

} // end namespace toy

#endif