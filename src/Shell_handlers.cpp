#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>

#include <chrono>
#include <ctime>
#include <filesystem>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action actually does.
// Each is small and single-concern; orchestration it shares lives in helpers.

namespace rouster {

void Shell::on_nav(const std::string& page) {  // handler: a sidebar row was chosen
    if (!m_page.empty()) remove_css_class("page-" + m_page);
    m_page = page;
    add_css_class("page-" + page);   // the window's gradient follows the module (rouster.css)
    m_stack.set_visible_child(page);
    if (page == "preflight" || page == "sweep") paint_preflight();   // fresh gate on arrival
    if (page == "activity") paint_activity();
}

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
    // A finished sweep that was just taken back: the done screen would lie.
    if (m_sw_states.get_visible_child_name() == "done") {
        m_sw_sub.set_text("Undone -- everything is back where it was. Look again to see it.");
        m_sw_states.set_visible_child("start");
    }
}

// ── Sweep (s006) ─────────────────────────────────────────────────────────────
void Shell::on_sweep_find() {  // handler: look for junk (worker)
    if (m_sw_worker.joinable()) return;
    m_sw_progress = std::make_shared<core::scan::Progress>();
    m_sw_run_big.set_text("0 B");
    m_sw_run_caption.set_text("Looking through caches and logs");
    if (m_act_sw_find)   m_act_sw_find->set_enabled(false);
    if (m_act_sw_cancel) m_act_sw_cancel->set_enabled(true);
    sweep_gate();
    m_sw_spinner.start();
    m_sw_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Sweep)) lg->info("find (home '{}')", m_home);

    m_sw_worker = std::thread([this, prog = m_sw_progress, home = m_home] {
        const auto t0 = std::chrono::steady_clock::now();
        core::sweep::Found f = core::sweep::find(home, *prog, static_cast<std::int64_t>(std::time(nullptr)));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Sweep))
            lg->info("found {} items, Trash {} in {} files, {} unreadable, {} ms{}{}", f.items.size(),
                     core::scan::human_bytes(f.trash_bytes), f.trash_files, f.errors, ms,
                     f.cancelled ? " (cancelled)" : "", f.refused.empty() ? "" : " REFUSED: " + f.refused);
        {
            std::lock_guard lk(m_sw_mx);
            m_sw_found = std::move(f);
        }
        m_sweep_finished.emit();
    });
    m_sw_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_sweep), 120);
}

void Shell::on_sweep_cancel() {  // handler: stop looking
    if (m_sw_progress) m_sw_progress->cancel = true;
    if (auto lg = log::get(log::Area::Sweep)) lg->info("cancel requested");
}

void Shell::on_sweep_found() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_sw_worker.joinable()) m_sw_worker.join();
    m_sw_tick.disconnect();
    m_sw_spinner.stop();
    if (m_act_sw_find)   m_act_sw_find->set_enabled(true);
    if (m_act_sw_cancel) m_act_sw_cancel->set_enabled(false);
    {
        std::lock_guard lk(m_sw_mx);
        m_sw_shown = m_sw_found;
    }
    if (!m_sw_shown.refused.empty()) {
        m_sw_sub.set_text("Not looked: " + m_sw_shown.refused);
        m_sw_states.set_visible_child("start");
        return;
    }
    paint_sweep();
    m_sw_states.set_visible_child("results");
}

void Shell::on_sweep_run() {  // handler: the checked items -> a plan -> the door
    if (!m_door || m_sw_worker.joinable()) return;
    // Preview first: only from the results list, which is the preview.
    if (m_sw_states.get_visible_child_name() != "results") return;
    paint_preflight();                     // the gate is judged now, not at startup
    if (m_pf_red) { sweep_gate(); return; }

    core::door::Plan plan = core::sweep::plan(m_home, m_sw_shown.items);
    if (plan.actions.empty()) return;
    plan.previewed = true;                 // the list on screen was the preview
    std::uint64_t bytes = 0;
    for (const auto& a : plan.actions) bytes += a.bytes;

    const auto r = m_door->run(plan);
    if (auto lg = log::get(log::Area::Sweep)) {
        lg->info("run '{}': {} done, {} failed", plan.label, r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    if (r.ok) {
        m_sw_done_big.set_text(core::scan::human_bytes(bytes));
        m_sw_done_caption.set_text("went to the Trash -- " + std::to_string(r.done) +
            (r.done == 1 ? " item" : " items") + ". Undo puts everything back. "
            "The space returns when you empty the Trash; Rouster leaves that step to you.");
    } else if (r.done == 0) {
        m_sw_done_big.set_text("Nothing moved");
        m_sw_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.")
                                                      : r.messages.front());
    } else {
        m_sw_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(plan.actions.size()));
        m_sw_done_caption.set_text("went to the Trash; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   ". Undo puts back what moved.");
    }
    m_sw_states.set_visible_child("done");
    sweep_gate();
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
