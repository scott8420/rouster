#pragma once
#include "latch/Vcs.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Git -- moved to latch (latch/Vcs.hpp, 2026-11-10): git and any other program
// as a child; feed() through pipes. This shim keeps Rouster's name for it.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::vcs {
using namespace ::latch::vcs;
}  // namespace rouster::core::vcs
