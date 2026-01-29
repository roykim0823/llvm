#ifndef LOG_H
#define LOG_H   

#include <memory>
#include "ast.h"

// LogError* - These are little helper functions for error handling.
namespace toy {

std::unique_ptr<ExprAST> logError(const char* str);
std::unique_ptr<PrototypeAST> logErrorP(const char* str);
llvm::Value *logErrorV(const char *str);
}
#endif
