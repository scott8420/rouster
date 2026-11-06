#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Settings (s011) -- Rouster's OWN files in its data folder (rules.json), not
// the user's. Written here, not through the door -- the door refuses Rouster's
// records as a target -- and the source gate allowlists this file for that
// reason (like Recents and Known). One function: replace a named file in the
// data folder atomically (temp + rename). `name` must be a plain file name.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core {

bool save_own(const std::string& data_dir, const std::string& name, const std::string& text);

}  // namespace rouster::core
