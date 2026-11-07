#include "core/System.hpp"

#include <sys/stat.h>

namespace rouster::core::sysclean {

std::vector<Item> find(const Where& w, int days) {
    std::vector<Item> out;
    {
        Item it;
        it.req = {helper::Op::PkgClean, 0};
        it.title = "Package cache";
        const std::string prog = helper::first_program(w.pkg_programs);
        it.available = !prog.empty();
        it.why = prog.find("dnf") != std::string::npos
                     ? "Downloaded packages and repository data. dnf fetches them again when it needs them."
                     : "Downloaded packages. apt fetches them again when it needs them.";
        for (const auto& d : w.pkg_dirs) {
            const auto u = helper::du(d);
            it.bytes += u.bytes;
            if (u.exists && !u.complete) it.complete = false;
        }
        it.sized = true;
        it.note = it.available ? "Can't be undone -- it refills on the next update." : "Neither dnf nor apt-get is here.";
        it.checked = it.available && it.bytes > 0;
        out.push_back(it);
    }
    {
        Item it;
        it.req = {helper::Op::Journal, days};
        it.title = "System journal";
        it.available = !helper::first_program(w.journal_programs).empty();
        const auto u = helper::du(w.journal);
        it.bytes = u.bytes;
        it.complete = u.complete;
        it.sized = true;
        it.why = "The log of past boots and errors. Only what is older than " + std::to_string(days) +
                 " days goes; the size is the whole journal.";
        it.note = it.available ? "Can't be undone -- old logs are history; unchecked." : "journalctl is not here.";
        out.push_back(it);
    }
    {
        Item it;
        it.req = {helper::Op::Trim, 0};
        it.title = "SSD trim";
        it.available = !helper::first_program(w.trim_programs).empty();
        it.why = "Tells the SSD which blocks are free, so it stays fast. Changes no file.";
        struct stat st{};
        const bool timer = ::stat(w.trim_timer.c_str(), &st) == 0;
        it.note = !it.available ? std::string("fstrim is not here.")
                : timer ? std::string("Your system already trims weekly (fstrim.timer) -- unchecked.")
                        : std::string("Nothing is lost; safe any time.");
        it.checked = it.available && !timer;
        out.push_back(it);
    }
    return out;
}

door::Plan plan(const std::vector<Item>& items) {
    door::Plan p;
    std::string names;
    for (const auto& it : items) {
        if (!it.checked || !it.available) continue;
        door::Action a;
        a.verb = door::Verb::Root;
        a.content = helper::text(it.req);
        a.reason = helper::describe(it.req);
        a.bytes = it.bytes;
        p.actions.push_back(a);
        names += (names.empty() ? "" : ", ") + it.title;
    }
    p.label = "System: " + (names.empty() ? std::string("nothing") : names);
    return p;
}

}  // namespace rouster::core::sysclean
