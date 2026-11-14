#include "core/About.hpp"

namespace rouster::core::about {

const char* tagline() {
    return "Mops up your Linux home folder -- cleans what Tidy leaves, never loses anything.";
}

const char* description() {
    return "A roustabout does the heavy hauling and the cleanup. Rouster finds what can go and says why: "
           "caches, duplicates, build output, finished projects, system clutter. Organizing is Tidy's work. Everything it "
           "does goes through one door that records it, so anything it did can be taken back.";
}

const std::vector<Group>& credits() {
    static const std::vector<Group> g = {
        {"Made by", {
            {"Scott", "Design, direction and every decision", "", "https://github.com/scott8420", ""},
            {"Claude (Anthropic)", "Pair programmer", "", "https://www.anthropic.com", ""},
        }},
        {"Built with", {
            {"GTK 4 and gtkmm 4", "The interface", "LGPL-2.1-or-later", "https://www.gtkmm.org", ""},
            {"spdlog", "Logging", "MIT", "https://github.com/gabime/spdlog", ""},
            {"nlohmann/json", "The Activity record and settings", "MIT", "https://github.com/nlohmann/json", "json.hpp"},
            {"xxHash", "Fast hashing for Duplicates", "BSD-2-Clause", "https://github.com/Cyan4973/xxHash", "xxhash.h"},
            {"polkit", "Asking for your password before root work", "LGPL-2.0-or-later", "https://github.com/polkit-org/polkit", ""},
        }},
        {"Works with", {
            {"btrfs", "Snapshots and shared blocks", "", "", ""},
            {"Pika Backup", "Rouster reads when it last ran", "", "https://apps.gnome.org/PikaBackup/", ""},
            {"git", "Checks your projects are safely pushed", "", "", ""},
            {"GnuPG", "Encrypts the secrets in a profile backup", "", "", ""},
            {"dconf, dnf, journald, fstrim", "GNOME settings and system housekeeping", "", "", ""},
        }},
        {"Inspired by", {
            {"CleanMyMac X", "The one-scan, one-summary feel", "", "", ""},
        }},
    };
    return g;
}

}  // namespace rouster::core::about
