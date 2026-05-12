/// \file
/// \brief Compile-time version string for the `tinylang` driver.

#ifndef TINYLANG_BASIC_VERSION_H
#define TINYLANG_BASIC_VERSION_H

#include "tinylang/Basic/Version.inc"
#include <string>

namespace tinylang {

/// Returns the tinylang version baked in at configure time.
///
/// The value comes from `TINYLANG_VERSION_STRING` in the top-level
/// `CMakeLists.txt`, written into the generated `Version.inc`.
std::string getTinylangVersion();

} // namespace tinylang

#endif
