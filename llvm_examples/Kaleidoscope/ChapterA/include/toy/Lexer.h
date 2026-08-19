//===- Lexer.h - Lexer for the Kaleidoscope language ----------------------===//
//
// Buffer-based lexer in the style of the MLIR Toy tutorial: the abstract
// Lexer owns all tokenization logic and delegates line acquisition to a
// subclass via readNextLine(), so it can lex from a memory buffer, a file,
// or (in a future subclass) an interactive prompt -- and tests can feed it
// a plain string instead of redirecting stdin.
//
//===----------------------------------------------------------------------===//

#ifndef TOY_LEXER_H
#define TOY_LEXER_H

#include "llvm/ADT/StringRef.h"

#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>

namespace toy {

/// Structure definition of a location in a file.
/// The filename is shared so every AST node can copy a Location cheaply.
struct Location {
  std::shared_ptr<std::string> file; ///< filename.
  int line;                          ///< line number.
  int col;                           ///< column number.
};

/// List of tokens returned by the lexer. Single-character punctuation and
/// operators are returned as their ASCII value; named tokens are negative.
enum Token : int {
  tok_semicolon = ';',
  tok_paren_open = '(',
  tok_paren_close = ')',
  tok_comma = ',',

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

  // user-defined operators
  tok_binary = -11,
  tok_unary = -12,

  // mutable variable definition
  tok_var = -13,
};

/// The Lexer is an abstract base class providing all the facilities that the
/// Parser expects. It goes through the stream one token at a time and keeps
/// track of the location in the input for debugging purposes.
/// It relies on a subclass to provide a `readNextLine()` method. The subclass
/// can proceed by reading the next line from the standard input or from a
/// memory mapped file.
class Lexer {
public:
  /// Create a lexer for the given filename. The filename is kept only for
  /// debugging purposes (attaching a location to a Token).
  Lexer(std::string filename)
      : lastLocation(
            {std::make_shared<std::string>(std::move(filename)), 0, 0}) {}
  virtual ~Lexer() = default;

  /// Look at the current token in the stream.
  Token getCurToken() { return curTok; }

  /// Move to the next token in the stream and return it.
  Token getNextToken() { return curTok = getTok(); }

  /// Move to the next token in the stream, asserting on the current token
  /// matching the expectation.
  void consume(Token tok) {
    assert(tok == curTok && "consume Token mismatch expectation");
    getNextToken();
  }

  /// Return the current identifier (prereq: getCurToken() == tok_identifier).
  llvm::StringRef getIdentifier() {
    assert(curTok == tok_identifier);
    return identifierStr;
  }

  /// Return the current number (prereq: getCurToken() == tok_number).
  double getValue() {
    assert(curTok == tok_number);
    return numVal;
  }

  /// Return the location for the beginning of the current token.
  Location getLastLocation() { return lastLocation; }

  /// Return the current line in the file.
  int getLine() { return curLineNum; }

  /// Return the current column in the file.
  int getCol() { return curCol; }

private:
  /// Delegate to a derived class fetching the next line. Returns an empty
  /// string to signal end of file (EOF). Lines are expected to always finish
  /// with "\n".
  virtual llvm::StringRef readNextLine() = 0;

  /// Return the next character from the stream. This manages the buffer for
  /// the current line and requests the next line buffer to the derived class
  /// as needed.
  int getNextChar() {
    // The current line buffer should not be empty unless it is the end of file.
    if (curLineBuffer.empty())
      return EOF;
    ++curCol;
    auto nextchar = curLineBuffer.front();
    curLineBuffer = curLineBuffer.drop_front();
    if (curLineBuffer.empty())
      curLineBuffer = readNextLine();
    if (nextchar == '\n') {
      ++curLineNum;
      curCol = 0;
    }
    return nextchar;
  }

  /// Return the next token from the input stream.
  Token getTok() {
    // Skip any whitespace.
    while (isspace(lastChar))
      lastChar = Token(getNextChar());

    // Save the current location before reading the token characters.
    lastLocation.line = curLineNum;
    lastLocation.col = curCol;

    // Identifier or keyword: [a-zA-Z][a-zA-Z0-9_]*
    if (isalpha(lastChar)) {
      identifierStr = (char)lastChar;
      while (isalnum((lastChar = Token(getNextChar()))) || lastChar == '_')
        identifierStr += (char)lastChar;

      if (identifierStr == "def")
        return tok_def;
      if (identifierStr == "extern")
        return tok_extern;
      if (identifierStr == "if")
        return tok_if;
      if (identifierStr == "then")
        return tok_then;
      if (identifierStr == "else")
        return tok_else;
      if (identifierStr == "for")
        return tok_for;
      if (identifierStr == "in")
        return tok_in;
      if (identifierStr == "binary")
        return tok_binary;
      if (identifierStr == "unary")
        return tok_unary;
      if (identifierStr == "var")
        return tok_var;
      return tok_identifier;
    }

    // Number: [0-9]+('.'[0-9]*)? or '.'[0-9]+
    if (isdigit(lastChar) || lastChar == '.') {
      std::string numStr;
      if (lastChar == '.') {
        // A leading dot must be followed by a digit to be a number,
        // otherwise it is returned as a plain '.' token.
        int nextChar = getNextChar();
        if (!isdigit(nextChar)) {
          lastChar = Token(nextChar);
          return Token('.');
        }
        numStr = "0.";
        lastChar = Token(nextChar);
      }
      // Consume digits, and at most one decimal point.
      bool seenDot = numStr == "0.";
      do {
        if (lastChar == '.') {
          if (seenDot)
            break; // second '.' terminates the number ("3.14.15" -> 3.14, '.', 15)
          seenDot = true;
        }
        numStr += (char)lastChar;
        lastChar = Token(getNextChar());
      } while (isdigit(lastChar) || lastChar == '.');

      numVal = strtod(numStr.c_str(), nullptr);
      return tok_number;
    }

    // Comment until end of line.
    if (lastChar == '#') {
      do {
        lastChar = Token(getNextChar());
      } while (lastChar != EOF && lastChar != '\n' && lastChar != '\r');

      if (lastChar != EOF)
        return getTok();
    }

    // Check for end of file. Don't eat the EOF.
    if (lastChar == EOF)
      return tok_eof;

    // Otherwise, just return the character as its ascii value.
    Token thisChar = Token(lastChar);
    lastChar = Token(getNextChar());
    return thisChar;
  }

  /// The last token read from the input.
  Token curTok = tok_eof;

  /// Location for `curTok`.
  Location lastLocation;

  /// If the current Token is an identifier, this string contains the value.
  std::string identifierStr;

  /// If the current Token is a number, this contains the value.
  double numVal = 0.0;

  /// The last value returned by getNextChar(). We need to keep it around as we
  /// always need to read ahead one character to decide when to end a token and
  /// we can't put it back in the stream after reading from it.
  Token lastChar = Token(' ');

  /// Keep track of the current line number in the input stream.
  int curLineNum = 0;

  /// Keep track of the current column number in the input stream.
  int curCol = 0;

  /// Buffer supplied by the derived class on calls to `readNextLine()`.
  llvm::StringRef curLineBuffer = "\n";
};

/// A lexer implementation operating on a buffer of memory.
class LexerBuffer final : public Lexer {
public:
  LexerBuffer(const char *begin, const char *end, std::string filename)
      : Lexer(std::move(filename)), current(begin), end(end) {}

private:
  /// Provide one line at a time to the Lexer, return an empty string when
  /// reaching the end of the buffer.
  llvm::StringRef readNextLine() override {
    auto *begin = current;
    while (current <= end && *current && *current != '\n')
      ++current;
    if (current <= end && *current)
      ++current;
    llvm::StringRef result{begin, static_cast<size_t>(current - begin)};
    return result;
  }

  const char *current, *end;
};

} // namespace toy

#endif // TOY_LEXER_H
