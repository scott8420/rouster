#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"
#include "core/Known.hpp"

#include <algorithm>

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>
#include <giomm/appinfo.h>
#include <glibmm/convert.h>

#include <chrono>
#include <cstring>
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
    if (page == "preflight" || page == "sweep" || page == "dev" || page == "dups" || page == "archive") paint_preflight();   // fresh gate on arrival
    if (page == "projects" && m_pj_states.get_visible_child_name() == "home") paint_projects();   // templates may have been edited in Files
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
    if (!m_door || door_busy() || !m_door->can_undo()) return;
    const std::string label = m_door->undo_label();
    bool shared_only = false;   // a Duplicates share: nothing moved, so nothing is "put back"
    for (auto it = m_door->batches().rbegin(); it != m_door->batches().rend(); ++it) {
        if (it->undone) continue;
        shared_only = !it->records.empty();
        for (auto id : it->records)
            if (const auto* rec = m_door->record(id); rec && rec->verb != core::door::Verb::Dedupe) shared_only = false;
        break;
    }
    const auto r = m_door->undo();
    std::string said = (r.ok ? "Undid \u201c" : "Partly undid \u201c") + label + "\u201d -- " +
                       (shared_only ? "closed; the files were and are identical, so nothing had to move"
                                    : std::to_string(r.done) + " put back");
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
    if (m_ar_states.get_visible_child_name() == "done") {
        m_ar_sub.set_text("Undone -- everything is back where it was. Look again to see it.");
        m_ar_states.set_visible_child("start");
    }
    if (m_du_states.get_visible_child_name() == "done") {
        m_du_sub.set_text("Undone. A Trash step is back where it was; a shared step is closed -- the files were "
                          "and are identical, so there was nothing to move. Look again to see it.");
        m_du_states.set_visible_child("start");
    }
    if (const auto pj = m_pj_states.get_visible_child_name(); pj == "done" || pj == "project" || pj == "home") {
        const std::string open = m_pj_open.path;
        if (m_pj_has_open && !core::organize::load_project(open, m_pj_open)) m_pj_has_open = false;   // undone away
        if (pj != "project" || !m_pj_has_open) on_pj_home();
        else paint_pj_project();
    }
    if (m_dv_states.get_visible_child_name() == "done") {
        m_dv_sub.set_text("Undone -- the build output is back where it was. Look again to see it.");
        m_dv_states.set_visible_child("start");
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
    if (!m_door || door_busy() || m_sw_worker.joinable()) return;
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

// ── Duplicates (s008) ────────────────────────────────────────────────────────
void Shell::on_dups_find() {  // handler: look for duplicates (worker)
    if (m_du_worker.joinable() || door_busy()) return;
    m_du_progress = std::make_shared<core::dups::Progress>();
    m_du_run_big.set_text("0 files");
    m_du_run_caption.set_text(core::dups::stage_name(0));
    if (m_act_du_find)   m_act_du_find->set_enabled(false);
    if (m_act_du_cancel) m_act_du_cancel->set_enabled(true);
    dups_gate();
    m_du_spinner.start();
    m_du_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Dups)) lg->info("find (home '{}')", m_home);

    m_du_worker = std::thread([this, prog = m_du_progress, home = m_home] {
        const auto t0 = std::chrono::steady_clock::now();
        core::dups::Found f = core::dups::find(home, home, *prog);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Dups)) {
            std::size_t folders = 0;
            for (const auto& g : f.groups) folders += g.folder;
            lg->info("found {} groups ({} folders) in {} files, read {}, {} unreadable, reflink {}, {} ms{}{}",
                     f.groups.size(), folders, f.files, core::scan::human_bytes(f.hashed), f.errors,
                     f.reflink ? "yes" : "no", ms, f.cancelled ? " (cancelled)" : "",
                     f.refused.empty() ? "" : " REFUSED: " + f.refused);
        }
        {
            std::lock_guard lk(m_du_mx);
            m_du_found = std::move(f);
        }
        m_dups_finished.emit();
    });
    m_du_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_dups), 120);
}

void Shell::on_dups_cancel() {  // handler: stop looking
    if (m_du_progress) m_du_progress->cancel = true;
    if (auto lg = log::get(log::Area::Dups)) lg->info("cancel requested");
}

void Shell::on_dups_found() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_du_worker.joinable()) m_du_worker.join();
    m_du_tick.disconnect();
    m_du_spinner.stop();
    if (m_act_du_find)   m_act_du_find->set_enabled(true);
    if (m_act_du_cancel) m_act_du_cancel->set_enabled(false);
    {
        std::lock_guard lk(m_du_mx);
        m_du_shown = m_du_found;
    }
    if (!m_du_shown.refused.empty()) {
        m_du_sub.set_text("Not looked: " + m_du_shown.refused);
        m_du_states.set_visible_child("start");
        return;
    }
    m_du_mode = m_du_shown.reflink ? core::dups::Mode::Share : core::dups::Mode::Trash;
    dups_preset();
    paint_dups();
    m_du_states.set_visible_child("results");
}

void Shell::on_dups_mode(core::dups::Mode m) {  // handler: Share / Trash switch
    if (m == m_du_mode) return;
    if (m == core::dups::Mode::Share && !m_du_shown.reflink) return;
    m_du_mode = m;
    dups_preset();
    paint_dups();
}

void Shell::on_dups_keep(std::size_t gi, std::size_t mi) {  // handler: make another member the keeper
    if (gi >= m_du_shown.groups.size() || mi >= m_du_shown.groups[gi].members.size()) return;
    auto& g = m_du_shown.groups[gi];
    g.keep = mi;
    g.keep_why = "you chose";
    for (std::size_t i = 0; i < g.members.size(); ++i)
        g.members[i].checked = m_du_mode == core::dups::Mode::Share && i != g.keep && !g.shared(i);
    if (auto lg = log::get(log::Area::Dups)) lg->info("keep '{}'", g.members[mi].rel);
    paint_dups();
}

void Shell::on_dups_run() {  // handler: the checked copies -> a plan -> worker -> the door
    if (!m_door || door_busy() || m_du_worker.joinable()) return;
    if (m_du_states.get_visible_child_name() != "results") return;   // preview first: only from the list
    paint_preflight();
    if (m_pf_red) { dups_gate(); return; }
    m_du_dropped.clear();
    core::door::Plan plan = core::dups::plan(m_home, m_du_shown, m_du_mode, m_du_dropped);
    if (plan.actions.empty()) {
        m_du_gate.set_text(m_du_dropped.empty() ? std::string("Nothing to do.") : m_du_dropped.front() + " -- Look again.");
        return;
    }
    plan.previewed = true;   // the cards on screen were the preview
    m_du_act_bytes = 0;
    for (const auto& a : plan.actions) m_du_act_bytes += a.bytes;
    m_du_act_total = plan.actions.size();
    m_du_acting = true;
    if (m_act_undo_action) m_act_undo_action->set_enabled(false);
    if (m_act_du_find) m_act_du_find->set_enabled(false);
    if (m_act_du_cancel) m_act_du_cancel->set_enabled(false);   // a half-run plan is the door's to finish
    m_du_run_big.set_text(core::scan::human_bytes(m_du_act_bytes));
    m_du_run_caption.set_text(m_du_mode == core::dups::Mode::Share
        ? "Sharing blocks -- the filesystem compares every byte as it goes"
        : "Moving copies to the Trash");
    m_du_spinner.start();
    m_du_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Dups)) {
        lg->info("run '{}': {} actions, {}", plan.label, plan.actions.size(), core::scan::human_bytes(m_du_act_bytes));
        for (const auto& d : m_du_dropped) lg->info("  left out: {}", d);
    }
    m_du_act_worker = std::thread([this, plan] {
        const auto r = m_door->run(plan);
        {
            std::lock_guard lk(m_du_act_mx);
            m_du_act_result = r;
        }
        m_dups_acted.emit();
    });
}

void Shell::on_dups_acted() {  // handler: the door finished (Dispatcher -> UI thread)
    if (m_du_act_worker.joinable()) m_du_act_worker.join();
    m_du_acting = false;
    m_du_spinner.stop();
    if (m_act_du_find) m_act_du_find->set_enabled(true);
    core::door::Result r;
    {
        std::lock_guard lk(m_du_act_mx);
        r = m_du_act_result;
    }
    if (auto lg = log::get(log::Area::Dups)) {
        lg->info("run: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    const bool share = m_du_mode == core::dups::Mode::Share;
    std::string left;
    if (!m_du_dropped.empty())
        left = " " + std::to_string(m_du_dropped.size()) + " left out (changed since Look) -- see the log.";
    if (r.ok) {
        m_du_done_big.set_text(core::scan::human_bytes(m_du_act_bytes));
        m_du_done_caption.set_text(share
            ? "of copies now share their keepers' blocks -- " + std::to_string(r.done) + (r.done == 1 ? " file" : " files") +
              ". Every file is still where it was, with the same contents. The space is free now, unless a "
              "snapshot still holds the old blocks." + left
            : "went to the Trash -- " + std::to_string(r.done) + (r.done == 1 ? " copy. Undo puts it back." : " copies. Undo puts them back.") +
              " The space returns when you empty the Trash." + left);
    } else if (r.done == 0) {
        m_du_done_big.set_text(share ? "Nothing shared" : "Nothing moved");
        m_du_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_du_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(m_du_act_total));
        m_du_done_caption.set_text(std::string(share ? "were shared" : "went to the Trash") + "; " +
                                   std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   (share ? ". Those files are untouched." : ". Undo puts back what moved."));
    }
    m_du_states.set_visible_child("done");
    dups_gate();
    paint_activity();
}

// ── Archive (s009) ───────────────────────────────────────────────────────────
void Shell::on_archive_find() {  // handler: look for old files (worker)
    if (m_ar_worker.joinable() || door_busy()) return;
    m_ar_progress = std::make_shared<core::scan::Progress>();
    m_ar_run_big.set_text("0 files");
    m_ar_run_caption.set_text("Looking for files untouched in " +
                              std::string(m_ar_months % 12 ? std::to_string(m_ar_months) + " months"
                                                           : m_ar_months == 12 ? "a year" : std::to_string(m_ar_months / 12) + " years"));
    if (m_act_ar_find)   m_act_ar_find->set_enabled(false);
    if (m_act_ar_cancel) m_act_ar_cancel->set_enabled(true);
    archive_gate();
    m_ar_spinner.start();
    m_ar_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Archive)) lg->info("find (home '{}', {} months)", m_home, m_ar_months);

    m_ar_worker = std::thread([this, prog = m_ar_progress, home = m_home, months = m_ar_months] {
        const auto t0 = std::chrono::steady_clock::now();
        core::archive::Found f = core::archive::find(home, static_cast<std::int64_t>(std::time(nullptr)), months, *prog);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Archive))
            lg->info("found {} items ({}) in {} files, {} unreadable, {} ms{}{}", f.items.size(),
                     core::scan::human_bytes(f.old_bytes), f.files, f.errors, ms, f.cancelled ? " (cancelled)" : "",
                     f.refused.empty() ? "" : " REFUSED: " + f.refused);
        {
            std::lock_guard lk(m_ar_mx);
            m_ar_found = std::move(f);
        }
        m_archive_finished.emit();
    });
    m_ar_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_archive), 120);
}

void Shell::on_archive_cancel() {  // handler: stop looking
    if (m_ar_progress) m_ar_progress->cancel = true;
    if (auto lg = log::get(log::Area::Archive)) lg->info("cancel requested");
}

void Shell::on_archive_found() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_ar_worker.joinable()) m_ar_worker.join();
    m_ar_tick.disconnect();
    m_ar_spinner.stop();
    if (m_act_ar_find)   m_act_ar_find->set_enabled(true);
    if (m_act_ar_cancel) m_act_ar_cancel->set_enabled(false);
    {
        std::lock_guard lk(m_ar_mx);
        m_ar_shown = m_ar_found;
    }
    if (!m_ar_shown.refused.empty()) {
        m_ar_sub.set_text("Not looked: " + m_ar_shown.refused);
        m_ar_states.set_visible_child("start");
        return;
    }
    m_ar_stored = m_door ? core::archive::stored(*m_door, m_home) : std::vector<core::archive::Stored>{};
    m_ar_view_stored = false;
    paint_archive();
    m_ar_states.set_visible_child("results");
}

void Shell::on_archive_view(bool stored) {  // handler: Old files / In the archive
    if (stored == m_ar_view_stored) return;
    m_ar_view_stored = stored;
    if (stored && m_door) m_ar_stored = core::archive::stored(*m_door, m_home);   // fresh, from the manifest
    paint_archive();
}

void Shell::on_archive_months(int months) {  // handler: the "untouched for" choice -> look again
    if (months == m_ar_months) return;
    m_ar_months = months;
    if (auto lg = log::get(log::Area::Archive)) lg->info("untouched for {} months", months);
    on_archive_find();
}

void Shell::on_archive_run() {  // handler: checked rows -> a plan -> the door
    if (!m_door || door_busy() || m_ar_worker.joinable()) return;
    if (m_ar_states.get_visible_child_name() != "results") return;   // preview first: only from the list
    paint_preflight();
    if (m_pf_red) { archive_gate(); return; }
    std::vector<std::string> dropped;
    core::door::Plan plan = m_ar_view_stored ? core::archive::put_back(m_home, m_ar_stored, dropped)
                                             : core::archive::plan(m_home, m_ar_shown.items, dropped);
    std::size_t moves = 0;
    std::uint64_t bytes = 0;
    for (const auto& a : plan.actions)
        if (a.verb == core::door::Verb::Move) { ++moves; bytes += a.bytes; }
    if (auto lg = log::get(log::Area::Archive)) {
        lg->info("run '{}': {} actions", plan.label, plan.actions.size());
        for (const auto& d : dropped) lg->info("  left out: {}", d);
    }
    if (!moves) {
        m_ar_gate.set_text(dropped.empty() ? std::string("Nothing to do.") : dropped.front() + " -- Look again.");
        return;
    }
    plan.previewed = true;   // the list on screen was the preview
    const auto r = m_door->run(plan);
    if (auto lg = log::get(log::Area::Archive)) {
        lg->info("run: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    const std::string n = std::to_string(moves) + (moves == 1 ? " item" : " items");
    const std::string left = dropped.empty() ? "" : " " + std::to_string(dropped.size()) + " left out -- see the log.";
    if (r.ok) {
        m_ar_done_big.set_text(core::scan::human_bytes(bytes));
        m_ar_done_caption.set_text(m_ar_view_stored
            ? "went back where it came from -- " + n + "." + left
            : "is in ~/Archive now -- " + n + ", each under the year it was last changed, with its folders kept. "
              "Undo takes the step back; Put back works any time later." + left);
    } else if (r.done == 0) {
        m_ar_done_big.set_text("Nothing moved");
        m_ar_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_ar_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(plan.actions.size()));
        m_ar_done_caption.set_text("steps done; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   ". Undo puts back what moved.");
    }
    m_ar_states.set_visible_child("done");
    archive_gate();
    paint_activity();
}

// ── Projects (s010) ──────────────────────────────────────────────────────────
void Shell::on_pj_home() {  // handler: back to templates + projects (re-read)
    paint_projects();
    m_pj_states.set_visible_child("home");
}

void Shell::on_pj_new(std::size_t tmpl) {  // handler: a template's New project... -> the form
    if (tmpl >= m_pj_templates.size()) return;
    m_pj_tmpl = tmpl;
    m_pj_new_title.set_text("New " + m_pj_templates[tmpl].name);
    m_pj_id.set_text("");
    m_pj_title.set_text("");
    on_pj_form();
    m_pj_states.set_visible_child("new");
    m_pj_id.grab_focus();
}

void Shell::on_pj_form() {  // handler: id / title typed -> the plan again
    namespace og = core::organize;
    if (m_pj_tmpl >= m_pj_templates.size()) return;
    const auto& t = m_pj_templates[m_pj_tmpl];
    const og::Values v{m_pj_id.get_text(), m_pj_title.get_text(), static_cast<std::int64_t>(std::time(nullptr))};
    std::vector<std::string> problems;
    m_pj_plan = og::new_project(m_home, t, v, problems);
    m_pj_plan_gated = false;
    const std::string dir = og::project_dir(m_home, t, v);
    m_pj_new_where.set_text("~/" + t.root + "/" + og::fill(t.folder, v) + "    ·    " +
                            (t.rules.pattern.empty() ? std::string("file names kept as they are") : t.rules.pattern));
    while (auto* c = m_pj_new_list.get_first_child()) m_pj_new_list.remove(*c);
    if (!problems.empty()) {
        m_pj_new_gate.set_text(problems.front() + ".");
        m_pj_new_go.set_label("Create");
        m_pj_new_go.set_sensitive(false);
        return;
    }
    paint_pj_plan(m_pj_new_list, m_pj_plan);
    m_pj_new_go.set_label("Create " + std::filesystem::path(dir).filename().string());
    m_pj_new_gate.set_text("One step: " + std::to_string(m_pj_plan.actions.size()) +
                           " things made. Undo sends what was made to the Trash.");
    m_pj_new_go.set_sensitive(!door_busy());
}

void Shell::on_pj_create() {  // handler: the form's plan -> the door
    namespace og = core::organize;
    if (!m_door || door_busy() || m_pj_plan.actions.empty()) return;
    if (m_pj_states.get_visible_child_name() != "new") return;   // preview first: only from the form
    m_pj_plan.previewed = true;   // the list on screen was the preview
    const auto r = m_door->run(m_pj_plan);
    if (auto lg = log::get(log::Area::Organize)) {
        lg->info("run '{}': {} done, {} failed", m_pj_plan.label, r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    std::string dir;
    for (const auto& a : m_pj_plan.actions)
        if (a.to.size() > 13 && a.to.ends_with(std::string("/") + og::kMeta)) dir = a.to.substr(0, a.to.size() - std::strlen(og::kMeta) - 1);
    if (r.ok && og::load_project(dir, m_pj_open)) {
        m_pj_has_open = true;
        m_pj_queue.clear();
        m_pj_back = "project";
    } else {
        m_pj_back = "home";
    }
    const std::string name = std::filesystem::path(dir).filename().string();
    pj_done(r, name, "Ready at ~/" + (dir.size() > m_home.size() ? dir.substr(m_home.size() + 1) : dir) +
                     ". Drop files on it to file them by its pattern.", m_pj_back);
}

void Shell::on_pj_starters() {  // handler: Install starter templates... -> the preview
    std::vector<std::string> skipped;
    m_pj_plan = core::organize::starters(m_home, skipped);
    m_pj_plan_gated = false;
    m_pj_back = "home";
    m_pj_pv_title.set_text("Starter templates");
    m_pj_pv_sub.set_text(std::string("Plain folders in ~/") + core::organize::kTemplates +
                         " -- open them in Files and make them yours. Design job and Book name files "
                         "{id}_{title}_{yyyymmdd-hhmm}_{part}.{ext}; edit a template's .rouster.json to change that." +
                         (skipped.empty() ? std::string() : "  Already there, left alone: " + std::to_string(skipped.size()) + "."));
    while (auto* c = m_pj_pv_list.get_first_child()) m_pj_pv_list.remove(*c);
    paint_pj_plan(m_pj_pv_list, m_pj_plan);
    m_pj_pv_go.set_label("Install");
    m_pj_pv_go.set_sensitive(!m_pj_plan.actions.empty() && !door_busy());
    m_pj_pv_gate.set_text(m_pj_plan.actions.empty() ? "All three are there already." : "One step. Undo sends them to the Trash.");
    m_pj_states.set_visible_child("preview");
}

void Shell::on_pj_version(const std::string& rel) {  // handler: a row's New version -> the preview
    if (!m_pj_has_open) return;
    std::string why;
    m_pj_plan = core::organize::new_version(m_pj_open, rel, static_cast<std::int64_t>(std::time(nullptr)), why);
    m_pj_plan_gated = true;
    m_pj_back = "project";
    m_pj_pv_title.set_text("New version");
    m_pj_pv_sub.set_text(rel);
    while (auto* c = m_pj_pv_list.get_first_child()) m_pj_pv_list.remove(*c);
    if (!why.empty()) {
        m_pj_pv_gate.set_text("No new version: " + why + ".");
        m_pj_pv_go.set_label("Make the version");
        m_pj_pv_go.set_sensitive(false);
    } else {
        paint_pj_plan(m_pj_pv_list, m_pj_plan);
        m_pj_pv_go.set_label("Make the version");
        paint_preflight();
        m_pj_pv_gate.set_text(m_pf_red ? "Preflight has a red check. This waits until it clears -- see Preflight."
                                       : "The copy is the new current file; the old one goes into " +
                                             m_pj_open.rules.versions + "/. One step; Undo takes it back.");
        m_pj_pv_go.set_sensitive(!m_pf_red && !door_busy());
    }
    m_pj_states.set_visible_child("preview");
}

void Shell::on_pj_confirm() {  // handler: the preview's act button -> the door
    if (!m_door || door_busy() || m_pj_plan.actions.empty()) return;
    if (m_pj_states.get_visible_child_name() != "preview") return;
    if (m_pj_plan_gated) { paint_preflight(); if (m_pf_red) return; }
    m_pj_plan.previewed = true;   // the list on screen was the preview
    const auto r = m_door->run(m_pj_plan);
    if (auto lg = log::get(log::Area::Organize)) {
        lg->info("run '{}': {} done, {} failed", m_pj_plan.label, r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    pj_done(r, m_pj_plan_gated ? "New version" : "Templates installed",
            m_pj_plan_gated ? "The copy is the current file now; the old one is in " + m_pj_open.rules.versions + "/."
                            : std::string("Open them in Files, ~/") + core::organize::kTemplates + ", and make them yours.", m_pj_back);
}

void Shell::on_pj_open(const std::string& dir) {  // handler: open a project
    core::organize::Project p;
    if (!core::organize::load_project(dir, p)) {
        paint_projects();
        m_pj_states.set_visible_child("home");
        if (auto lg = log::get(log::Area::Organize)) lg->warn("not a project (no {}): {}", core::organize::kMeta, dir);
        return;
    }
    if (!m_pj_has_open || m_pj_open.path != p.path) m_pj_queue.clear();
    m_pj_open = p;
    m_pj_has_open = true;
    paint_pj_project();
    m_pj_states.set_visible_child("project");
}

void Shell::on_pj_pick_project() {  // handler: folder chooser -> open a project
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open a project folder");
    dialog->set_initial_folder(Gio::File::create_for_path(m_home));
    dialog->select_folder(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto dir = dialog->select_folder_finish(result);
            if (dir && !dir->get_path().empty()) on_pj_open(dir->get_path());
        } catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Organize)) lg->info("open project: dialog dismissed ({})", e.what());
        }
    });
}

void Shell::on_pj_add_files() {  // handler: file chooser -> the queue
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Files to file into the project");
    dialog->set_initial_folder(Gio::File::create_for_path(std::filesystem::exists(m_home + "/Downloads") ? m_home + "/Downloads" : m_home));
    dialog->open_multiple(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            std::vector<std::string> paths;
            for (const auto& f : dialog->open_multiple_finish(result))
                if (f && !f->get_path().empty()) paths.push_back(f->get_path());
            on_pj_dropped(paths);
        } catch (const Glib::Error& e) {
            if (auto lg = log::get(log::Area::Organize)) lg->info("add files: dialog dismissed ({})", e.what());
        }
    });
}

void Shell::on_pj_dropped(const std::vector<std::string>& paths) {  // handler: files dropped / chosen -> the queue
    if (!m_pj_has_open || paths.empty()) return;
    std::vector<std::string> all;
    std::vector<std::pair<std::string, bool>> checks;
    for (const auto& q : m_pj_queue) { all.push_back(q.from); checks.emplace_back(q.from, q.checked); }
    for (const auto& p : paths)
        if (std::find(all.begin(), all.end(), p) == all.end()) all.push_back(p);
    m_pj_queue = core::organize::propose(m_home, m_pj_open, all);   // looks, changes nothing
    for (auto& q : m_pj_queue)
        for (const auto& [from, c] : checks)
            if (q.from == from && !q.to.empty()) q.checked = c;
    if (auto lg = log::get(log::Area::Organize)) {
        lg->info("queue: {} files for {}", m_pj_queue.size(), m_pj_open.path);
        for (const auto& q : m_pj_queue) lg->info("  {} -> {}", q.from, q.to.empty() ? "(" + q.note + ")" : q.to);
    }
    paint_pj_project();
    m_pj_states.set_visible_child("project");
}

void Shell::on_pj_file() {  // handler: the checked queue -> a plan -> the door
    if (!m_door || door_busy() || !m_pj_has_open) return;
    if (m_pj_states.get_visible_child_name() != "project") return;   // preview first: only from the queue
    paint_preflight();
    if (m_pf_red) { pj_gate(); return; }
    std::vector<std::string> dropped;
    core::door::Plan plan = core::organize::file_plan(m_pj_open, m_pj_queue, dropped);
    std::size_t moves = 0;
    for (const auto& a : plan.actions) moves += a.verb == core::door::Verb::Move;
    if (auto lg = log::get(log::Area::Organize)) {
        lg->info("run '{}': {} actions", plan.label, plan.actions.size());
        for (const auto& d : dropped) lg->info("  left out: {}", d);
    }
    if (!moves) {
        m_pj_gate.set_text(dropped.empty() ? std::string("Nothing to file.") : dropped.front() + " -- drop it again.");
        return;
    }
    plan.previewed = true;   // the queue on screen was the preview
    const auto r = m_door->run(plan);
    if (auto lg = log::get(log::Area::Organize)) {
        lg->info("run: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    m_pj_queue.clear();
    pj_done(r, "Filed " + std::to_string(moves) + (moves == 1 ? " file" : " files"),
            "Named by the project's pattern, each in the folder its type goes to." +
               (dropped.empty() ? std::string() : " " + std::to_string(dropped.size()) + " left out: " + dropped.front() + "."),
            "project");
}

void Shell::on_pj_show(const std::string& path) {  // handler: open a folder in Files
    try {
        Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(path));
    } catch (const Glib::Error& e) {
        if (auto lg = log::get(log::Area::Organize)) lg->warn("could not open {}: {}", path, e.what());
    }
}

// ── Dev projects (s007) ──────────────────────────────────────────────────────
void Shell::on_dev_find() {  // handler: find + verify the dev projects (worker)
    if (m_dv_worker.joinable()) return;
    m_dv_progress = std::make_shared<core::scan::Progress>();
    m_dv_run_big.set_text("0 projects");
    m_dv_run_caption.set_text("Looking under ~/Develop and asking each remote");
    if (m_act_dv_find)   m_act_dv_find->set_enabled(false);
    if (m_act_dv_cancel) m_act_dv_cancel->set_enabled(true);
    dev_gate();
    m_dv_spinner.start();
    m_dv_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Dev)) lg->info("find (home '{}')", m_home);

    m_dv_worker = std::thread([this, prog = m_dv_progress, home = m_home, known = m_known] {
        const auto t0 = std::chrono::steady_clock::now();
        core::dev::Found f = core::dev::find(home, *prog, known);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Dev)) {
            lg->info("found {} projects in {} roots, {} unreadable, {} ms{}{}", f.projects.size(), f.roots.size(),
                     f.errors, ms, f.cancelled ? " (cancelled)" : "", f.refused.empty() ? "" : " REFUSED: " + f.refused);
            for (const auto& p : f.projects) {
                lg->info("  {} [{}] {} -- remote {} ({}), {} build dirs, {} drift", p.rel,
                         p.kind == core::dev::Kind::Workspace ? "workspace" : "repo",
                         core::dev::status_name(p.repo.status), p.repo.remote, p.repo.host, p.builds.size(), p.drift.size());
                for (const auto& c : p.repo.checks)
                    if (!c.ok) lg->info("    {}: {}", c.name, c.detail);
                if (!p.repo.error.empty()) lg->info("    error: {}", p.repo.error);
            }
        }
        {
            std::lock_guard lk(m_dv_mx);
            m_dv_found = std::move(f);
        }
        m_dev_finished.emit();
    });
    m_dv_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_dev), 120);
}

void Shell::on_dev_cancel() {  // handler: stop looking
    if (m_dv_progress) m_dv_progress->cancel = true;
    if (auto lg = log::get(log::Area::Dev)) lg->info("cancel requested");
}

void Shell::on_dev_found() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_dv_worker.joinable()) m_dv_worker.join();
    m_dv_tick.disconnect();
    m_dv_spinner.stop();
    if (m_act_dv_find)   m_act_dv_find->set_enabled(true);
    if (m_act_dv_cancel) m_act_dv_cancel->set_enabled(false);
    {
        std::lock_guard lk(m_dv_mx);
        m_dv_shown = m_dv_found;
    }
    if (!m_dv_shown.refused.empty()) {
        m_dv_sub.set_text("Not looked: " + m_dv_shown.refused);
        m_dv_states.set_visible_child("start");
        return;
    }
    paint_dev();
    m_dv_states.set_visible_child("results");
}

void Shell::on_dev_run() {  // handler: the checked build dirs -> a plan -> the door
    if (!m_door || door_busy() || m_dv_worker.joinable()) return;
    if (m_dv_states.get_visible_child_name() != "results") return;   // the cards are the preview
    paint_preflight();
    if (m_pf_red) { dev_gate(); return; }

    core::door::Plan plan = core::dev::plan(m_home, m_dv_shown.projects);
    if (plan.actions.empty()) return;
    plan.previewed = true;
    std::uint64_t bytes = 0;
    for (const auto& a : plan.actions) bytes += a.bytes;

    const auto r = m_door->run(plan);
    if (auto lg = log::get(log::Area::Dev)) {
        lg->info("run '{}': {} done, {} failed", plan.label, r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    if (r.ok) {
        m_dv_done_big.set_text(core::scan::human_bytes(bytes));
        m_dv_done_caption.set_text("of build output went to the Trash -- " + std::to_string(r.done) +
            (r.done == 1 ? " folder" : " folders") + ". The next build makes it again; Undo puts it back now. "
            "The space returns when you empty the Trash.");
    } else if (r.done == 0) {
        m_dv_done_big.set_text("Nothing moved");
        m_dv_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_dv_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(plan.actions.size()));
        m_dv_done_caption.set_text("went to the Trash; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   ". Undo puts back what moved.");
    }
    m_dv_states.set_visible_child("done");
    dev_gate();
    paint_activity();
}

// ── Retire / restore (s007b) ─────────────────────────────────────────────────
// Card -> the preview (the "confirm" state lists every step of the plan) ->
// the act button -> a worker: retire asks the remote ONCE MORE (the verdict on
// screen may be minutes old), then the door runs; restore's clone is the
// network's time. While the worker owns the door, every other door use waits.
namespace {
const core::dev::Project* project_at(const core::dev::Found& f, const std::string& rel) {
    for (const auto& p : f.projects) if (p.rel == rel) return &p;
    return nullptr;
}
}  // namespace

void Shell::on_dev_retire(const std::string& rel) {  // handler: Retire... -> the preview
    const auto* p = project_at(m_dv_shown, rel);
    if (!p || door_busy()) return;
    std::string why;
    core::dev::RetireView view;
    m_dv_plan = core::dev::retire_plan(m_home, *p, static_cast<std::int64_t>(std::time(nullptr)), why, &view);
    if (m_dv_plan.actions.empty()) { m_dv_gate.set_text("Can't retire " + p->name + ": " + why); return; }
    m_dv_cf_rel = rel;
    m_dv_cf_retire = true;
    std::vector<std::string> warn;
    if (!view.ignored.empty()) {
        std::string ig;
        for (const auto& i : view.ignored) ig += (ig.empty() ? "" : ", ") + i;
        warn.push_back("Git ignores these, so they are NOT on " + p->repo.host +
                       ", and they leave with the working copy (to the Trash, until you empty it): " + ig);
    }
    if (p->fixes_files) warn.push_back("fixes/ stays exactly where it is -- Rouster never touches it.");
    paint_dev_confirm("Retire " + p->name + "?",
        "Everything in " + p->app + "/ is on " + p->repo.host + " (" + p->repo.remote + ") at " +
            p->repo.head.substr(0, 7) + (p->repo.branch.empty() ? "" : " on " + p->repo.branch) +
            ". Rouster asks the remote once more before anything moves.",
        "Retire " + p->name, view.keep, warn);
    if (auto lg = log::get(log::Area::Dev)) lg->info("retire preview '{}': {} steps", rel, m_dv_plan.actions.size());
}

void Shell::on_dev_restore(const std::string& rel) {  // handler: Restore... -> the preview
    const auto* p = project_at(m_dv_shown, rel);
    if (!p || door_busy()) return;
    std::string why;
    m_dv_plan = core::dev::restore_plan(m_home, *p, why);
    if (m_dv_plan.actions.empty()) { m_dv_gate.set_text("Can't restore " + p->name + ": " + why); return; }
    m_dv_cf_rel = rel;
    m_dv_cf_retire = false;
    paint_dev_confirm("Restore " + p->name + "?",
        "Clones " + p->restore.url + " into ~/" + p->rel + "/" + p->restore.app + " and checks that commit " +
            p->restore.commit.substr(0, 7) + " is in its history. Build output and ignored files don't come back -- "
            "build again after. (If the old copy is still in the Trash, Undo in Activity is the other way back.)",
        "Restore " + p->name, {}, {});
    if (auto lg = log::get(log::Area::Dev)) lg->info("restore preview '{}': {}", rel, p->restore.url);
}

void Shell::on_dev_confirm() {  // handler: the preview's act button -> worker
    if (!m_door || door_busy() || m_dv_worker.joinable() || m_dv_plan.actions.empty()) return;
    if (m_dv_states.get_visible_child_name() != "confirm") return;   // only from the preview
    if (m_dv_act_worker.joinable()) m_dv_act_worker.join();
    paint_preflight();
    if (m_pf_red) {
        m_dv_cf_sub.set_text("Preflight has a red check -- nothing moves until it clears. See Preflight.");
        return;
    }
    const auto* p = project_at(m_dv_shown, m_dv_cf_rel);
    if (!p) return;
    m_dv_plan.previewed = true;   // the confirm screen showed every step
    m_dv_acting = true;
    if (m_act_undo_action) m_act_undo_action->set_enabled(false);
    if (m_act_dv_find) m_act_dv_find->set_enabled(false);
    m_dv_run_big.set_text(m_dv_cf_retire ? "Retiring" : "Restoring");
    m_dv_run_caption.set_text(m_dv_cf_retire ? "Asking " + p->repo.remote + " once more, then moving"
                                             : "Cloning from " + p->repo.host);
    if (m_act_dv_cancel) m_act_dv_cancel->set_enabled(false);   // a half-run plan is the door's to finish
    m_dv_spinner.start();
    m_dv_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Dev)) lg->info("{} '{}' -- go", m_dv_cf_retire ? "retire" : "restore", m_dv_cf_rel);

    const std::string repo_abs = m_home + "/" + p->rel + "/" + p->app;
    m_dv_act_worker = std::thread([this, plan = m_dv_plan, retire = m_dv_cf_retire, repo_abs, head = p->repo.head] {
        core::door::Result r;
        std::string refused;
        if (retire) {
            const auto again = core::dev::verify(repo_abs);
            if (again.status != core::dev::Status::Backed) {
                refused = std::string("it is no longer backed up (") + core::dev::status_name(again.status) + ")";
                for (const auto& c : again.checks)
                    if (!c.ok) { refused += ": " + c.detail; break; }
            } else if (again.head != head) {
                refused = "it changed since you looked (now at " + again.head.substr(0, 7) + ") -- Look again";
            }
        }
        if (refused.empty()) r = m_door->run(plan);
        {
            std::lock_guard lk(m_dv_act_mx);
            m_dv_act_result = r;
            m_dv_act_refused = refused;
        }
        m_dev_acted.emit();
    });
}

void Shell::on_dev_acted() {  // handler: retire / restore finished (Dispatcher -> UI thread)
    if (m_dv_act_worker.joinable()) m_dv_act_worker.join();
    m_dv_acting = false;
    m_dv_spinner.stop();
    if (m_act_dv_find) m_act_dv_find->set_enabled(true);
    core::door::Result r;
    std::string refused;
    {
        std::lock_guard lk(m_dv_act_mx);
        r = m_dv_act_result;
        refused = m_dv_act_refused;
    }
    const auto* p = project_at(m_dv_shown, m_dv_cf_rel);
    const std::string name = p ? p->name : m_dv_cf_rel;
    if (auto lg = log::get(log::Area::Dev)) {
        lg->info("{} '{}': {} done, {} failed{}", m_dv_cf_retire ? "retire" : "restore", m_dv_cf_rel, r.done, r.failed,
                 refused.empty() ? "" : " -- NOT RUN: " + refused);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    if (!refused.empty()) {
        m_dv_done_big.set_text("Not retired");
        m_dv_done_caption.set_text(name + " was left exactly as it was: " + refused + ".");
    } else if (r.ok && m_dv_cf_retire) {
        std::uint64_t bytes = 0;
        for (const auto& a : m_dv_plan.actions) bytes += a.bytes;
        const auto n = m_dv_plan.actions.size() - 2;
        m_dv_done_big.set_text("Retired");
        m_dv_done_caption.set_text(name + "'s working copy" + (n ? " and " + std::to_string(n) + " old backups" : std::string()) +
            " went to the Trash (" + core::scan::human_bytes(bytes) + "); RESTORE.txt is in its place. "
            "Restore brings it back from " + (p ? p->repo.host : std::string("its remote")) +
            " in one click; Undo puts everything back now.");
    } else if (r.ok) {
        m_dv_done_big.set_text("Restored");
        m_dv_done_caption.set_text(name + " is back" + (p ? " from " + p->repo.host + " at " + p->restore.commit.substr(0, 7) : std::string()) +
            ". RESTORE.txt went to the Trash. Build it again with build.sh. Undo sends the clone to the Trash.");
    } else if (r.done == 0) {
        m_dv_done_big.set_text("Nothing moved");
        m_dv_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_dv_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(m_dv_plan.actions.size()));
        m_dv_done_caption.set_text("steps happened; " + std::to_string(r.failed) + " could not: " +
            (r.messages.empty() ? std::string("see Activity") : r.messages.front()) + ". Undo takes back what happened.");
    }
    m_dv_plan = {};
    m_dv_states.set_visible_child("done");
    paint_activity();
}

void Shell::on_dev_known(const std::string& rel, const std::string& name, bool add) {  // handler: promote / forget
    auto it = std::find(m_known.begin(), m_known.end(), name);
    if (add && it == m_known.end()) m_known.push_back(name);
    if (!add && it != m_known.end()) m_known.erase(it);
    core::save_known(data_dir() + "/known.json", m_known);
    if (auto lg = log::get(log::Area::Dev)) lg->info("{} '{}' (from {})", add ? "known-optional" : "forgot", name, rel);
    // Re-lint every workspace on screen: a name is known everywhere.
    for (auto& p : m_dv_shown.projects)
        if (p.kind == core::dev::Kind::Workspace && !p.retired) {
            p.drift = core::dev::lint(m_home + "/" + p.rel, p.app, m_known);
        }
    paint_dev();
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
