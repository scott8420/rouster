#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Help (s013a2) -- what Rouster does and how to use it, as DATA. GTK-free, so
// the selftest can hold it to the app: every sidebar page has a topic (F1
// opens the one you're on), no topic is empty, ids are unique, search finds.
// HelpWindow only paints it.
//
// Text is plain. A paragraph starting "- " is painted as a bullet; one
// starting "$ " as a command line (monospace, selectable).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::help {

struct Section {
    std::string heading;               // "" = no heading
    std::vector<std::string> paras;
};

struct Topic {
    std::string id;                    // a sidebar page id ("sweep") or a general one ("safety")
    std::string title;                 // "Sweep"
    std::string group;                 // "Start here", "Clean up", "Organize", "Safety"
    std::string lead;                  // one line under the title
    std::vector<Section> sections;
};

const std::vector<Topic>& topics();    // in reading order
const Topic* find(const std::string& id);
// Topics whose text holds every word of `query` (case-insensitive), in order.
std::vector<const Topic*> search(const std::string& query);

}  // namespace rouster::core::help
