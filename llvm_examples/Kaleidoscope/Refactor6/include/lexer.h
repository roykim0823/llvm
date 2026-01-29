#ifndef LEXER_H
#define LEXER_H

#include <string>

namespace toy {
//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//
// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
// Each token returned by the lexer includes a token code and potentially some 
// metadata.
enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,

  // control
  tok_if = -6,
  tok_then = -7,
  tok_else = -8,
  tok_for = -9,
  tok_in = -10,

  // operators
  tok_binary = -11,
  tok_unary = -12
};

class Lexer {
    public:
    int gettok();
    double getNumVal() const { return numVal; }
    std::string getIdentifierStr() const { return identifierStr; }
    

    private:
    std::string identifierStr; // Filled in if tok_identifier
    double numVal;             // Filled in if tok_number
    int lastChar = ' ';  // Used by gettok
};

} // end namespace toy
#endif // LEXER_H