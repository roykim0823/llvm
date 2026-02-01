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

    // /// BinopPrecedence - This holds the precedence for each binary operator that is
    // /// defined.    
    // std::map<char, int> binopPrecedence;  // it is used in AST, so move to log

    /// LogError* - These are little helper functions for error handling.
    //std::unique_ptr<ExprAST> logError(const char* str);
    //std::unique_ptr<PrototypeAST> logErrorP(const char* str);

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

    // Top-level parsing and JIT driver
    void handleDefinition();
    void handleExtern();
    void handleTopLevelExpression();
};

} // end namespace toy

#endif