#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Known -- the names Scott promoted to "known-optional" in the template lint
// (s007b, REQUIREMENTS §4.5: "one click promotes it"). A JSON array of names in
// Rouster's own data folder, next to the manifest. Rouster's own setting, not a
// user file: it is written here, not through the door, and the source gate
// allowlists this file for that reason (like Recents). Forget takes a name back.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core {

std::vector<std::string> load_known(const std::string& file);   // missing / bad file -> empty
bool save_known(const std::string& file, const std::vector<std::string>& names);   // sorted, unique

}  // namespace rouster::core
