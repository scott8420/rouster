#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>

#include <filesystem>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace rouster {

void Shell::on_scan() {  // handler: Smart Scan the home folder
    start_scan(m_home);
}

void Shell::on_scan_cancel() {  // handler: stop the running scan
    if (m_progress) m_progress->cancel = true;
    if (auto lg = log::get(log::Area::Scan)) lg->info("cancel requested");
}

void Shell::on_scan_done() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_worker.joinable()) m_worker.join();
    m_tick.disconnect();
    set_scanning(false);
    paint_report();
}

void Shell::on_undo() {  // handler: undo the last batch, through the door
    if (!m_door || !m_door->can_undo()) return;
    const std::string label = m_door->undo_label();
    const auto r = m_door->undo();
    std::string said = (r.ok ? "Undid \u201c" : "Partly undid \u201c") + label + "\u201d -- " +
                       std::to_string(r.done) + " put back";
    if (r.failed) said += ", " + std::to_string(r.failed) + " could not be: " + r.messages.front();
    m_act_said.set_text(said);
    if (auto lg = log::get(log::Area::Door)) {
        lg->info("undo '{}': {} back, {} failed", label, r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    paint_activity();
}

void Shell::on_preflight() {  // handler: run the checklist again
    paint_preflight();
}

void Shell::on_pick_folder() {  // handler: folder chooser -> scan it
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Pick a folder to scan");
    dialog->set_initial_folder(Gio::File::create_for_path(m_home));
    dialog->select_folder(*this,
        [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto dir = dialog->select_folder_finish(result);
                if (!dir) return;
                const std::string folder = dir->get_path();
                if (folder.empty()) return;
                note_recent(folder);
                start_scan(folder);
            } catch (const Glib::Error& e) {
                if (auto lg = log::get(log::Area::Shell))
                    lg->info("pick-folder: dialog dismissed ({})", e.what());
            }
        });
}

void Shell::on_recent_open(const std::string& path) {  // handler: scan a recent again
    if (path.empty()) return;
    if (!std::filesystem::exists(path)) {   // a vanished folder self-prunes
        core::recents_remove(m_recents, path);
        core::save_recents(m_recents_file, m_recents);
        rebuild_recents_menu();
        if (auto lg = log::get(log::Area::Recents)) lg->warn("'{}': gone -- pruned", path);
        return;
    }
    note_recent(path);   // move-to-front
    start_scan(path);
}

void Shell::on_recent_clear() {  // handler: empty the recents list
    m_recents.clear();
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::on_dump_registry() {  // handler: print the live widget tree
    registry::dump();
}

void Shell::on_shortcuts() {  // handler: the keyboard reference
    // Same lifetime stone as About: build once, re-present. It renders from
    // core::shortcut_registry(), the list App wires accels from.
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {  // handler: open the About window
    // Hide-on-close singleton (CANON: "Lifetime shape is design").
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

}  // namespace rouster
