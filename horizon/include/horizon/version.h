// Placeholder API that proves the library builds and links. Replaced by storage in Phase 3.
#pragma once  // include this header at most once per .cpp (replaces #ifndef guards)

#include <string>

// Everything in Horizon lives in this namespace; callers write horizon::version().
namespace horizon {

// Returns the library version. std::string owns its memory, so the caller never frees it.
std::string version();

}  // namespace horizon
