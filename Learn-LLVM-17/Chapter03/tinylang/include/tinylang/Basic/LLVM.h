/// \file
/// \brief Forward declarations and `using` aliases for the subset of LLVM
/// types referenced throughout tinylang.
///
/// Including this header lets the rest of the project write `SMLoc`,
/// `StringRef`, `cast`, `dyn_cast`, etc. unqualified, without dragging the
/// full `llvm::` namespace into the implementation.

#ifndef TINYLANG_BASIC_LLVM_H
#define TINYLANG_BASIC_LLVM_H

#include "llvm/Support/Casting.h"

namespace llvm {
class SMLoc;
class SourceMgr;
template <typename T, typename A> class StringMap;
class StringRef;
class raw_ostream;
} // namespace llvm

namespace tinylang {
using llvm::cast;            ///< Checked LLVM-style RTTI cast (asserts on failure).
using llvm::cast_or_null;    ///< Like \ref cast but tolerates `nullptr`.
using llvm::dyn_cast;        ///< Returns `nullptr` if the dynamic type doesn't match.
using llvm::dyn_cast_or_null;///< Like \ref dyn_cast but tolerates a `nullptr` input.
using llvm::isa;             ///< True if a pointer's dynamic type matches.

using llvm::raw_ostream;
using llvm::SMLoc;
using llvm::SourceMgr;
using llvm::StringMap;
using llvm::StringRef;
} // namespace tinylang

#endif
