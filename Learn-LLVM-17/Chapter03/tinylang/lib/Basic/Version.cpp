/// \file
/// \brief Returns the configure-time version string.

#include "tinylang/Basic/Version.h"

std::string tinylang::getTinylangVersion() {
  return TINYLANG_VERSION_STRING;
}
