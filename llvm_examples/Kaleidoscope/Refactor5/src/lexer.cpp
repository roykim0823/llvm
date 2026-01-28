#include <cstdio>
#include <cctype>

#include "lexer.h"

using namespace toy;
/// gettok - Return the next token from standard input.
int Lexer::gettok() {

    while (isspace(lastChar))  // Skip any whitespace
        lastChar = getchar();

    if (isalpha(lastChar)) {  // identifier: [a-zA-Z][a-zA-Z0-9]*
        identifierStr = lastChar;
        while (isalnum((lastChar = getchar())))
            identifierStr += lastChar;

        if (identifierStr == "def") return tok_def;
        if (identifierStr == "extern") return tok_extern;
        if (identifierStr == "if") return tok_if;
        if (identifierStr == "then") return tok_then;
        if (identifierStr == "else") return tok_else;
        if (identifierStr == "for") return tok_for;
        if (identifierStr == "in") return tok_in;
        return tok_identifier;
    }

    if (isdigit(lastChar) || lastChar == '.') {  // Number: [0-9.]+
        std::string numStr;
        do {
            numStr += lastChar;
            lastChar = getchar();
        } while (isdigit(lastChar) || lastChar == '.');

        numVal = strtod(numStr.c_str(), nullptr);
        return tok_number;
    }

    if (lastChar == '#') {
        // Comment until end of line.
        do lastChar = getchar();
        while (lastChar != EOF && lastChar != '\n' && lastChar != '\r');
        
        if (lastChar != EOF) return gettok();
    }

    // Check for end of file.  Don't eat the EOF.
    if (lastChar == EOF) return tok_eof;

    // Otherwise, just return the character as its ascii value.
    int thisChar = lastChar;
    lastChar = getchar();
    return thisChar;
}