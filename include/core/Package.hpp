#pragma once
#include "latch/Package.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Package -- moved to latch (latch/Package.hpp, 2026-11-10): packages are one
// thing, never split. This shim keeps Rouster's name for it.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::pkg {
using namespace ::latch::pkg;
}  // namespace rouster::core::pkg
