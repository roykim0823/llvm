#include "tinylang/Basic/TokenKinds.h"
#include "llvm/Support/ErrorHandling.h"

using namespace tinylang;

static const char * const TokNames[] = {
#define TOK(ID) #ID,  // # is the stringizing operator
#define KEYWORD(ID, FLAG) #ID,
#include "tinylang/Basic/TokenKinds.def"
  nullptr
};
/*
// Redefines TOK to wrap the ID in quotes (#ID)
// Redefines KEYWORD to wrap the ID in quotes
static const char * const TokNames[] = {
  "unknown",
  "eof",
  "identifier",
  "integer_literal",
  "string_literal",
  "plus",
  "minus",
  "AND",
  "BEGIN",
  // ... (all other tokens and keywords)
  nullptr
};
*/

const char *tok::getTokenName(TokenKind Kind) {
  if (Kind < tok::NUM_TOKENS)
    return TokNames[Kind];
  llvm_unreachable("unknown TokenKind");
  return nullptr;
}

const char *tok::getPunctuatorSpelling(TokenKind Kind) {
  switch (Kind) {
#define PUNCTUATOR(ID, SP) case ID: return SP;
#include "tinylang/Basic/TokenKinds.def"
    default: break;
  }
  return nullptr;
}

const char *tok::getKeywordSpelling(TokenKind Kind) {
  switch (Kind) {
#define KEYWORD(ID, FLAG) case kw_ ## ID: return #ID;
#include "tinylang/Basic/TokenKinds.def"
    default: break;
  }
/*
switch (Kind) {
    case kw_AND: return "AND";
    case kw_BEGIN: return "BEGIN";
    case kw_CONST: return "CONST";
    // ... (Note how kw_ is prepended to the ID)
    default: break;
  }
*/
  return nullptr;
}