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
    if (page == "preflight" || page == "sweep" || page == "dev" || page == "dups") paint_preflight();   // fresh gate on arrival
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
    if (m_du_states.get_visible_child_name() == "done") {
        m_du_sub.set_text("Undone. A Trash step is back where it was; a shared step is closed -- the files were "
                          "and are identical, so there was nothing to move. Look again to see it.");
        m_du_states.set_visible_child("start");
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
