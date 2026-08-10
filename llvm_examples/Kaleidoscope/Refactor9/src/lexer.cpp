#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "lexer.h"
#include "log.h"

using namespace toy;

SourceLocation CurLoc;  // Temp Global variable

/// gettok - Return the next token from standard input.
int Lexer::gettok() {

    while (isspace(lastChar))  // Skip any whitespace
        lastChar = dbgMgr.advance();

    CurLoc = dbgMgr.lexLoc;

    if (isalpha(lastChar)) {  // identifier: [a-zA-Z][a-zA-Z0-9]*
        identifierStr = lastChar;
        while (isalnum((lastChar = dbgMgr.advance())))
            identifierStr += lastChar;

        if (identifierStr == "def") return tok_def;
        if (identifierStr == "extern") return tok_extern;
        if (identifierStr == "if") return tok_if;
        if (identifierStr == "then") return tok_then;
        if (identifierStr == "else") return tok_else;
        if (identifierStr == "for") return tok_for;
        if (identifierStr == "in") return tok_in;
        if (identifierStr == "binary") return tok_binary;
        if (identifierStr == "unary") return tok_unary;
        if (identifierStr == "var") return tok_var;
        return tok_identifier;
    }

    if (isdigit(lastChar) || lastChar == '.') {  // Number: [0-9.]+
        std::string numStr;
        if (lastChar == '.') {
            // If we see a dot, it must be followed by a digit to be a valid number.
            int nextChar = dbgMgr.advance();
            if (!isdigit(nextChar)) {
                // Not a valid number, return the dot as a token.
                lastChar = nextChar; // Update lastChar to the next character for future calls.
                return '.';
            }
            numStr += "0."; // Prepend a zero for numbers like ".5"
            lastChar = nextChar; // Update lastChar to the digit after the dot for future calls.
        }

        do {
            numStr += lastChar;
            lastChar = dbgMgr.advance();
        } while (isdigit(lastChar) || lastChar == '.');

        numVal = strtod(numStr.c_str(), nullptr);
        return tok_number;
    }

    if (lastChar == '#') {
        // Comment until end of line.
        do lastChar = dbgMgr.advance();
        while (lastChar != EOF && lastChar != '\n' && lastChar != '\r');

        if (lastChar != EOF) return gettok();
    }

    // Check for end of file.  Don't eat the EOF.
    if (lastChar == EOF) return tok_eof;

    // Otherwise, just return the character as its ascii value.
    int thisChar = lastChar;
    lastChar = dbgMgr.advance();
    return thisChar;
}