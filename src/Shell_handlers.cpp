#include "Shell.hpp"
#include "core/Helper.hpp"
#include "AboutWindow.hpp"
#include "HelpWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Recents.hpp"
#include "core/Known.hpp"
#include "core/Settings.hpp"

#include <algorithm>

#include <gtkmm/filedialog.h>
#include <giomm/asyncresult.h>
#include <giomm/file.h>
#include <giomm/appinfo.h>
#include <giomm/contenttype.h>
#include <glibmm/convert.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
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
    if (page == "system" && m_sy_states.get_visible_child_name() == "results") { paint_preflight(); system_gate(); }
    if (page == "preflight" || page == "sweep" || page == "dev" || page == "dups") paint_preflight();
    if (page == "activity") paint_activity();
    if (page == "profile" && m_pr_states.get_visible_child_name() == "start") paint_profile_start();
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
    bool root_only = false;     // s013: a root step (a snapshot) is closed, not taken back
    for (auto it = m_door->batches().rbegin(); it != m_door->batches().rend(); ++it) {
        if (it->undone) continue;
        shared_only = root_only = !it->records.empty();
        for (auto id : it->records) {
            const auto* rec = m_door->record(id);
            if (rec && rec->verb != core::door::Verb::Dedupe) shared_only = false;
            if (rec && rec->verb != core::door::Verb::Root) root_only = false;
        }
        break;
    }
    const auto r = m_door->undo();
    std::string said = (r.ok ? "Undid \u201c" : "Partly undid \u201c") + label + "\u201d -- " +
                       (root_only ? std::string("closed; what ran as root isn't taken back (a snapshot stays, "
                                        "a cleaned cache refills, vacuumed logs are gone)")
                        : shared_only ? "closed; the files were and are identical, so nothing had to move"
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
    if (m_pr_states.get_visible_child_name() == "done") {
        m_pr_sub.set_text("Undone -- the backup went to the Trash (its files and folders, one by one). Look again to make another.");
        m_pr_last.set_text("");
        paint_profile_start();
        m_pr_states.set_visible_child("start");
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
        if (auto lg = log::get(log::Area::Sweep); lg && !f.kept_whole.empty())   // s015a
            for (const auto& pk : f.kept_whole) lg->info("kept whole (package): {}", pk);
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

    const auto r = door_run(plan);
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
        if (auto lg = log::get(log::Area::Dups); lg && !f.packages.empty())   // s015a
            for (const auto& pk : f.packages) lg->info("kept whole (package, not entered): {}", pk);
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
    // s016a2 -> s019d: hands-off folders are latch's owned places now (Tidy's list,
    // read at startup) -- Duplicates never walks into one, and the door refuses them.
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
        const auto r = door_run(plan);
        {
            std::lock_guard lk(m_du_act_mx);
            m_du_act_result = r;
        }
        m_dups_acted.emit();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// s013b: System. Look measures as you (a few folders under /var); Run sends
// the checked rows through the door, each a Root step -- pkexec asks once
// (auth_admin_keep), and a refusal skips the rest.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::on_system_look() {  // handler: measure the three (read-only, as you)
    if (m_sy_acting) return;
    m_sy_items = core::sysclean::find();
    if (auto lg = log::get(log::Area::System))
        for (const auto& it : m_sy_items)
            lg->info("find {}: {}{} {}{}", core::helper::text(it.req), it.complete ? "" : ">=", core::scan::human_bytes(it.bytes),
                     it.available ? "" : "(not here) ", it.checked ? "checked" : "unchecked");
    paint_preflight();
    paint_system();
    m_sy_states.set_visible_child("results");
}

void Shell::on_system_run() {  // handler: the checked rows -> a plan -> worker -> the door -> pkexec
    if (!m_door || door_busy() || m_sy_worker.joinable()) return;
    if (m_sy_states.get_visible_child_name() != "results") return;   // preview first: only from the list
    paint_preflight();
    if (m_pf_red) { system_gate(); return; }
    core::door::Plan plan = core::sysclean::plan(m_sy_items);
    if (plan.actions.empty()) { system_gate(); return; }
    plan.previewed = true;   // the cards on screen were the preview
    m_sy_plan = plan;
    m_sy_acting = true;
    if (m_act_undo_action) m_act_undo_action->set_enabled(false);
    m_sy_run_caption.set_text("Asking for your password, then running " + std::to_string(plan.actions.size()) +
                              (plan.actions.size() == 1 ? " step" : " steps") + " as root. A trim can take a minute.");
    m_sy_spinner.start();
    m_sy_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::System)) {
        lg->info("run '{}': {} root steps", plan.label, plan.actions.size());
        for (const auto& a : plan.actions) lg->info("  {}", a.content);
    }
    m_sy_worker = std::thread([this, plan] {
        const auto r = door_run(plan);
        {
            std::lock_guard lk(m_sy_mx);
            m_sy_result = r;
        }
        m_system_acted.emit();
    });
}

void Shell::on_system_acted() {  // handler: the helper answered (Dispatcher -> UI thread)
    if (m_sy_worker.joinable()) m_sy_worker.join();
    m_sy_acting = false;
    m_sy_spinner.stop();
    core::door::Result r;
    {
        std::lock_guard lk(m_sy_mx);
        r = m_sy_result;
    }
    std::string lines;
    if (r.batch)
        for (const auto& b : m_door->batches())
            if (b.id == r.batch)
                for (auto id : b.records)
                    if (const auto* rec = m_door->record(id)) {
                        const auto req = core::helper::parse(rec->to);
                        const std::string what = req ? core::helper::op_title(req->op) : rec->to;
                        lines += (lines.empty() ? "" : "\n") + std::string(rec->state == core::door::State::Done ? "✓ " : "✗ ") +
                                 (rec->state == core::door::State::Done ? (rec->note.empty() ? what : rec->note)
                                                                        : what + " -- " + rec->error);
                    }
    if (lines.empty() && !r.messages.empty()) lines = r.messages.front();
    const bool fixture = std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME");
    if (m_sy_done_icon) m_sy_done_icon->set_from_icon_name(r.done ? "rouster-hero-done" : "rouster-hero-system");
    m_sy_done_big.set_text(r.ok ? (fixture ? "Dry run" : "Done") : r.done ? "Partly done" : "Nothing ran");
    m_sy_done_caption.set_text(lines);
    if (auto lg = log::get(log::Area::System)) {
        lg->info("run: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    m_sy_states.set_visible_child("done");
    paint_activity();
}

// ─────────────────────────────────────────────────────────────────────────────
// s013: Take snapshot (Preflight). One Root action through the door; the
// door runs `pkexec rouster-helper run snapshot:5` -- which waits on the
// password prompt, so it runs on a worker and every other door use waits.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::on_snapshot_take() {  // handler: Preflight's Take snapshot -> worker -> the door
    if (!m_door || door_busy() || m_pf_worker.joinable()) return;
    core::door::Plan plan;
    plan.label = "Snapshot of home";
    core::door::Action a;
    a.verb = core::door::Verb::Root;
    a.content = core::helper::text({core::helper::Op::Snapshot, core::helper::kKeepDefault});
    a.reason = core::helper::describe({core::helper::Op::Snapshot, core::helper::kKeepDefault});
    plan.actions.push_back(a);
    plan.previewed = true;   // the snapshot card (and its tooltip) was the preview
    m_pf_acting = true;
    if (m_act_undo_action) m_act_undo_action->set_enabled(false);
    m_pf_said.set_text("Asking for your password -- the snapshot runs as root.");
    m_pf_said.set_visible(true);
    paint_preflight();       // the button reads Waiting… and is off
    if (auto lg = log::get(log::Area::Preflight)) lg->info("snapshot: asking root for {}", a.content);
    m_pf_worker = std::thread([this, plan] {
        const auto r = door_run(plan);
        {
            std::lock_guard lk(m_pf_mx);
            m_pf_result = r;
        }
        m_snapshot_taken.emit();
    });
}

void Shell::on_snapshot_taken() {  // handler: the helper answered (Dispatcher -> UI thread)
    if (m_pf_worker.joinable()) m_pf_worker.join();
    m_pf_acting = false;
    core::door::Result r;
    {
        std::lock_guard lk(m_pf_mx);
        r = m_pf_result;
    }
    std::string note;
    if (r.batch)
        for (const auto& b : m_door->batches())
            if (b.id == r.batch)
                for (auto id : b.records)
                    if (const auto* rec = m_door->record(id)) note = rec->note;
    const bool fixture = std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME");
    if (r.ok) m_pf_said.set_text((fixture ? "Dry run (a fixture never snapshots): " : "Done: ") + note + ".");
    else {
        std::string why = r.messages.empty() ? std::string("the helper failed") : r.messages.front();
        if (why.rfind("failed: root ", 0) == 0 && why.find(" -- ") != std::string::npos) why = why.substr(why.find(" -- ") + 4);
        m_pf_said.set_text("No snapshot: " + why + ".");
    }
    if (auto lg = log::get(log::Area::Preflight)) {
        lg->info("snapshot: {} -- {}", r.ok ? "ok" : "failed", r.ok ? note : (r.messages.empty() ? "" : r.messages.front()));
    }
    paint_preflight();
    paint_activity();
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

// ── Profile backup (s012a) ───────────────────────────────────────────────────
void Shell::on_profile_find() {  // handler: gather the set-up (worker: dconf, dnf, flatpak)
    if (m_pr_worker.joinable() || door_busy()) return;
    m_pr_cancel = false;
    if (m_act_pr_find) m_act_pr_find->set_enabled(false);
    if (m_act_pr_run) m_act_pr_run->set_enabled(false);
    m_pr_spinner.start();
    m_pr_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Profile)) lg->info("find (home '{}')", m_home);
    m_pr_worker = std::thread([this, home = m_home] {
        const auto t0 = std::chrono::steady_clock::now();
        core::profile::Found f = core::profile::find(home, (std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME") ? core::profile::fixture_runner(home, &m_pr_cancel) : core::profile::real_runner(&m_pr_cancel)));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Profile)) {
            lg->info("found {} items ({}), {} earlier backups, {} ms{}", f.items.size(), core::scan::human_bytes(f.bytes),
                     f.bundles.size(), ms, f.refused.empty() ? "" : " REFUSED: " + f.refused);
            for (const auto& n : f.notes) lg->info("  not gathered: {}", n);
        }
        {
            std::lock_guard lk(m_pr_mx);
            m_pr_found = std::move(f);
        }
        m_profile_finished.emit();
    });
}

void Shell::on_profile_found() {  // handler: the worker finished (Dispatcher -> UI thread)
    if (m_pr_worker.joinable()) m_pr_worker.join();
    m_pr_spinner.stop();
    if (m_act_pr_find) m_act_pr_find->set_enabled(true);
    {
        std::lock_guard lk(m_pr_mx);
        m_pr_shown = m_pr_found;
    }
    m_pr_last.set_text(m_pr_shown.bundles.empty() ? std::string()
                       : "Newest backup: ~/" + m_pr_shown.bundles.front() +
                             (m_pr_shown.bundles.size() > 1 ? " (" + std::to_string(m_pr_shown.bundles.size()) + " in all)" : ""));
    if (!m_pr_shown.refused.empty()) {
        m_pr_sub.set_text("Not looked: " + m_pr_shown.refused);
        m_pr_states.set_visible_child("start");
        return;
    }
    if (m_pr_notes && m_pr_notes->get_buffer()->get_text().empty()) {
        std::ifstream in(data_dir() + "/profile-notes.md");
        std::ostringstream ss;
        ss << in.rdbuf();
        m_pr_notes->get_buffer()->set_text(ss.str());
    }
    paint_profile();
    m_pr_states.set_visible_child("results");
}

void Shell::on_profile_run() {  // handler: checked rows -> a plan (one new folder) -> the door
    if (!m_door || door_busy() || m_pr_worker.joinable()) return;
    if (m_pr_states.get_visible_child_name() != "results") return;   // preview first: only from the list
    const bool sec = m_pr_sec_on && m_pr_sec_on->get_active() && !m_pr_shown.secrets.files.empty();
    if (!sec) { profile_backup({}, 0); return; }
    // s012c: the secrets first -- read, tarred and encrypted on a worker; the
    // passphrase leaves the boxes now and is wiped when gpg is done with it.
    std::string a = m_pr_pass1 ? std::string(m_pr_pass1->get_text()) : std::string();
    std::string b = m_pr_pass2 ? std::string(m_pr_pass2->get_text()) : std::string();
    const std::string problem = core::secrets::passphrase_problem(a, b);
    core::secrets::wipe(b);
    if (!problem.empty()) { core::secrets::wipe(a); profile_gate(); return; }
    if (m_pr_pass1) m_pr_pass1->set_text("");
    if (m_pr_pass2) m_pr_pass2->set_text("");
    m_pr_cancel = false;
    m_pr_packing = true;
    profile_gate();
    m_pr_gate.set_text("Encrypting the secrets (gpg)...");
    if (auto lg = log::get(log::Area::Profile))
        lg->info("secrets: encrypting {} files ({})", m_pr_shown.secrets.files.size(),
                 core::scan::human_bytes(m_pr_shown.secrets.bytes));
    m_pr_worker = std::thread([this, home = m_home, found = m_pr_shown.secrets, pass = std::move(a)]() mutable {
        auto pk = core::secrets::pack(home, found, pass, core::secrets::real_gpg(&m_pr_cancel));
        core::secrets::wipe(pass);
        {
            std::lock_guard lk(m_pr_mx);
            m_pr_packed = std::move(pk);
        }
        m_profile_packed.emit();
    });
}

void Shell::on_profile_packed() {  // handler: s012c -- the secrets are encrypted (Dispatcher, UI thread)
    if (m_pr_worker.joinable()) m_pr_worker.join();
    m_pr_packing = false;
    core::secrets::Packed pk;
    {
        std::lock_guard lk(m_pr_mx);
        pk = std::move(m_pr_packed);
        m_pr_packed = {};
    }
    if (auto lg = log::get(log::Area::Profile)) {
        lg->info("secrets: {} files, tar {}, encrypted {}{}", pk.files, core::scan::human_bytes(pk.raw),
                 core::scan::human_bytes(pk.data.size()), pk.error.empty() ? "" : " FAILED: " + pk.error);
        for (const auto& n : pk.notes) lg->info("  secrets: {}", n);
    }
    if (!pk.error.empty()) {
        profile_gate();
        m_pr_gate.set_text("Not backed up -- secrets: " + pk.error + ". Uncheck them to back up the rest.");
        return;
    }
    profile_backup(std::move(pk.data), pk.files);
}

void Shell::profile_backup(std::string secrets, std::size_t secret_files) {  // helper: plan + the door (after any encrypting)
    if (!m_door || door_busy()) return;
    const std::string notes = m_pr_notes ? std::string(m_pr_notes->get_buffer()->get_text()) : std::string();
    if (!core::save_own(data_dir(), "profile-notes.md", notes))
        if (auto lg = log::get(log::Area::Profile)) lg->warn("could not save {}/profile-notes.md", data_dir());
    std::vector<std::string> dropped;
    std::string bundle;
    core::door::Plan plan = core::profile::plan(m_home, m_pr_shown, notes, static_cast<std::int64_t>(std::time(nullptr)),
                                                bundle, dropped, secrets, secret_files);
    core::secrets::wipe(secrets);   // ciphertext, but no reason to keep a copy
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    for (const auto& a : plan.actions)
        if (a.verb != core::door::Verb::MakeDir) { ++files; bytes += a.bytes; }
    if (auto lg = log::get(log::Area::Profile)) {
        lg->info("run '{}': {} actions, {} files, {}", plan.label, plan.actions.size(), files, core::scan::human_bytes(bytes));
        for (const auto& d : dropped) lg->info("  left out: {}", d);
    }
    plan.previewed = true;   // the list on screen was the preview
    const auto r = door_run(plan);
    if (auto lg = log::get(log::Area::Profile)) {
        lg->info("run: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    m_pr_bundle = bundle;
    paint_profile_start();
    const std::string tilde = "~/" + bundle.substr(m_home.size() + 1);
    const std::string left = dropped.empty() ? "" : " " + std::to_string(dropped.size()) + " left out (changed since Look) -- see the log.";
    if (r.ok) {
        m_pr_done_big.set_text("Backed up " + core::scan::human_bytes(bytes));
        m_pr_done_caption.set_text(tilde + " -- " + std::to_string(files) + " files" +
                                   (secret_files ? " (" + std::to_string(secret_files) +
                                                       " secrets inside secrets.tar.gpg, encrypted -- keep the passphrase "
                                                       "somewhere safe; Rouster doesn't)"
                                                 : std::string()) +
                                   ". PROFILE.txt says what each is and how "
                                   "to put it back. Nothing else changed; Undo sends the folder to the Trash." + left);
    } else if (r.done == 0) {
        m_pr_done_big.set_text("Nothing made");
        m_pr_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_pr_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(plan.actions.size()));
        m_pr_done_caption.set_text("steps done; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   ". Undo sends what was made to the Trash.");
    }
    m_pr_states.set_visible_child("done");
    paint_activity();
}

void Shell::on_profile_open() {  // handler: open the bundle in Files
    if (m_pr_bundle.empty()) return;
    try {
        Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(m_pr_bundle));
    } catch (const Glib::Error& e) {
        if (auto lg = log::get(log::Area::Profile)) lg->warn("could not open {}: {}", m_pr_bundle, e.what());
    }
}

void Shell::on_profile_look_back(const std::string& name) {  // handler: a backup's Restore... -> worker: diff against home
    if (m_pr_worker.joinable() || door_busy()) return;
    m_pr_cancel = false;
    m_pr_spinner.start();
    m_pr_states.set_visible_child("running");
    const std::string bundle = m_home + "/" + name;   // s019d: relative to home (new or old root)
    m_pr_sec = {};   // s012c: secrets are opened again for each look
    if (auto lg = log::get(log::Area::Profile)) lg->info("look back at '{}'", bundle);
    m_pr_worker = std::thread([this, home = m_home, bundle] {
        auto r = core::profile::look_back(home, bundle, (std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME") ? core::profile::fixture_runner(home, &m_pr_cancel) : core::profile::real_runner(&m_pr_cancel)));
        if (auto lg = log::get(log::Area::Profile)) {
            std::size_t n[5] = {};
            for (const auto& b : r.files) ++n[static_cast<int>(b.state)];
            lg->info("look back: {} same, {} missing, {} differ, {} protected, {} blocked; {} commands{}", n[0], n[1], n[2],
                     n[3], n[4], r.commands.size(), r.refused.empty() ? "" : " REFUSED: " + r.refused);
        }
        {
            std::lock_guard lk(m_pr_mx);
            m_pr_rs_found = std::move(r);
        }
        m_profile_looked.emit();
    });
}

void Shell::on_profile_looked() {  // handler: the restore look finished (Dispatcher -> UI thread)
    if (m_pr_worker.joinable()) m_pr_worker.join();
    m_pr_spinner.stop();
    {
        std::lock_guard lk(m_pr_mx);
        m_pr_rs = m_pr_rs_found;
    }
    if (!m_pr_rs.refused.empty()) {
        m_pr_sub.set_text("Can't restore: " + m_pr_rs.refused);
        paint_profile_start();
        m_pr_states.set_visible_child("start");
        return;
    }
    paint_preflight();
    m_pr_rs_title.set_text("Restore " + m_pr_rs.name);
    m_pr_rs_sub.set_text("made " + m_pr_rs.made + " on " + m_pr_rs.host + " -- compared with what is in your home now");
    paint_profile_restore();
    m_pr_states.set_visible_child("restore");
}

void Shell::on_profile_open_secrets() {  // handler: s012c -- restore's Open -> worker: decrypt in memory + compare
    if (m_pr_worker.joinable() || !m_pr_rs_pass || m_pr_rs.secrets == 0) return;
    std::string pass = m_pr_rs_pass->get_text();
    if (pass.empty()) return;
    m_pr_rs_pass->set_text("");
    m_pr_rs_pass->set_sensitive(false);
    m_pr_cancel = false;
    if (auto lg = log::get(log::Area::Profile)) lg->info("secrets: opening {}/{}", m_pr_rs.bundle, core::secrets::kFile);
    m_pr_worker = std::thread([this, home = m_home, bundle = m_pr_rs.bundle, pass = std::move(pass)]() mutable {
        auto o = core::secrets::open(home, bundle, pass, core::secrets::real_gpg(&m_pr_cancel));
        core::secrets::wipe(pass);
        {
            std::lock_guard lk(m_pr_mx);
            m_pr_sec_found = std::move(o);
        }
        m_profile_sec_opened.emit();
    });
}

void Shell::on_profile_secrets_opened() {  // handler: s012c -- the secrets were opened (Dispatcher, UI thread)
    if (m_pr_worker.joinable()) m_pr_worker.join();
    {
        std::lock_guard lk(m_pr_mx);
        m_pr_sec = std::move(m_pr_sec_found);
        m_pr_sec_found = {};
    }
    if (auto lg = log::get(log::Area::Profile)) {
        std::size_t n[4] = {};
        for (const auto& r : m_pr_sec.rows) ++n[static_cast<int>(r.state)];
        lg->info("secrets: {} same, {} missing, {} differ, {} blocked; {} lines{}", n[0], n[1], n[2], n[3],
                 m_pr_sec.commands.size(), m_pr_sec.error.empty() ? "" : " -- " + m_pr_sec.error);
    }
    // The focused entry is about to go: drop focus first (else GTK warns, and
    // focus lands on the first selectable line and selects it all).
    unset_focus();
    paint_profile_restore();
    if (m_pr_rs_pass) m_pr_rs_pass->grab_focus();   // a wrong passphrase: ready to type again
    else m_pr_rs_go.grab_focus();
}

void Shell::on_profile_restore() {  // handler: checked rows -> Trash + Copy plan -> the door
    if (!m_door || door_busy() || m_pr_worker.joinable()) return;
    if (m_pr_states.get_visible_child_name() != "restore") return;   // preview first
    paint_preflight();
    if (m_pf_red) { profile_restore_gate(); return; }
    std::vector<std::string> dropped;
    core::door::Plan plan = core::profile::restore_plan(m_home, m_pr_rs, dropped);
    std::size_t files = 0, swaps = 0;
    bool settings = false;   // s012d
    for (const auto& a : plan.actions) {
        if (a.verb == core::door::Verb::Copy) ++files;
        if (a.verb == core::door::Verb::Trash) ++swaps;
        if (a.verb == core::door::Verb::Settings) settings = true;
    }
    if (auto lg = log::get(log::Area::Profile)) {
        lg->info("restore '{}': {} actions, {} files, {} replaced{}", plan.label, plan.actions.size(), files, swaps,
                 settings ? ", GNOME settings (" + std::to_string(m_pr_rs.settings.changes) + " keys)" : std::string());
        for (const auto& d : dropped) lg->info("  left out: {}", d);
    }
    if (!files && !settings) {
        m_pr_rs_gate.set_text(dropped.empty() ? std::string("Nothing to do.") : dropped.front() + " -- go Back and look again.");
        return;
    }
    plan.previewed = true;   // the restore list on screen was the preview
    const auto r = door_run(plan);
    if (auto lg = log::get(log::Area::Profile)) {
        lg->info("restore: {} done, {} failed", r.done, r.failed);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    m_pr_bundle = m_pr_rs.bundle;
    const std::string left = dropped.empty() ? "" : " " + std::to_string(dropped.size()) + " left out (changed since the look) -- see the log.";
    if (r.ok) {
        m_pr_done_big.set_text(files && settings ? "Restored " + std::to_string(files) + (files == 1 ? " file" : " files") + " + settings"
                               : files           ? "Restored " + std::to_string(files) + (files == 1 ? " file" : " files")
                                                 : std::string("Restored GNOME settings"));
        m_pr_done_caption.set_text((swaps ? std::to_string(swaps) + " replaced -- the ones that were there are in the Trash. " : std::string()) +
                                   (settings ? "GNOME settings loaded; what they were is saved. " : std::string()) +
                                   (m_pr_rs.commands.empty() ? std::string() : "The lines to run yourself stay on the restore page (Restore... again). ") +
                                   "Undo takes it all back." + left);
    } else if (r.done == 0) {
        m_pr_done_big.set_text("Nothing restored");
        m_pr_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_pr_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(plan.actions.size()));
        m_pr_done_caption.set_text("steps done; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) + ". Undo puts back what changed.");
    }
    m_pr_states.set_visible_child("done");
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
                lg->info("  {} [{}] {}{} -> {} -- remote {} ({}), {} drift", p.rel,
                         p.kind == core::dev::Kind::Workspace ? "workspace" : "repo",
                         core::dev::status_name(p.repo.status), p.repo.cleaned ? " (source cleaned)" : "",
                         core::dev::group_name(p.group), p.repo.remote, p.repo.host, p.drift.size());
                for (const auto& row : p.rows)
                    lg->info("    {}: {} items, {} bytes{}{}", core::dev::part_name(row.part), row.items.size(), row.bytes,
                             row.offered ? "" : " (not offered)", row.checked ? " (ticked)" : "");
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

// ── Clean (s020b) ────────────────────────────────────────────────────────────
// The ticked rows -> ONE plan -> the preview ("confirm" lists every step, sizes
// and the total) -> the act button -> a worker: every project whose Source is
// ticked is asked again (still Backed up, same commit) -- if any isn't, nothing
// moves -- then the door runs the one step. Undo brings it all back.
namespace {
const core::dev::Project* project_at(const core::dev::Found& f, const std::string& rel) {
    for (const auto& p : f.projects) if (p.rel == rel) return &p;
    return nullptr;
}
}  // namespace

void Shell::on_dev_run() {  // handler: the ticked rows -> a plan -> the preview
    namespace dv = core::dev;
    if (!m_door || door_busy() || m_dv_worker.joinable()) return;
    if (m_dv_states.get_visible_child_name() != "results") return;
    paint_preflight();
    if (m_pf_red) { dev_gate(); return; }

    m_dv_plan = dv::clean_plan(m_home, m_dv_shown.projects);
    if (m_dv_plan.actions.empty()) return;
    m_dv_cf_rel.clear();
    m_dv_cf_push = false;
    std::uint64_t bytes = 0;
    for (const auto& a : m_dv_plan.actions) bytes += a.bytes;

    std::vector<std::string> warn;
    const auto asked = dv::source_ticked(m_dv_shown.projects);
    for (const auto* p : asked) {
        const auto* src = dv::row_of(*p, dv::Part::Source);
        if (src && !src->leaves.empty()) {
            std::string l;
            for (const auto& x : src->leaves) l += (l.empty() ? "" : ", ") + x;
            warn.push_back(p->name + ": git ignores these, so they are NOT on " + p->repo.host +
                           " and git restore won't bring them back -- they go with the source (Undo still does): " + l);
        }
    }
    if (!asked.empty()) {
        std::string names;
        for (const auto* p : asked) names += (names.empty() ? "" : ", ") + p->name;
        warn.push_back("Source of " + names + ": Rouster asks the remote once more before anything moves -- "
                       "if it isn't still Backed up at the same commit, nothing moves.");
    }
    std::vector<std::string> keep = {"docs/", "scripts/", "fixes/", ".git", ".gitignore"};
    for (const auto* p : asked)
        if (const auto* src = dv::row_of(*p, dv::Part::Source))
            for (const auto& x : src->stays) keep.push_back(p->name + "/" + x + " (ignored)");
    const auto n = m_dv_plan.actions.size();
    paint_dev_confirm(m_dv_plan.label + "?",
        core::scan::human_bytes(bytes) + " to the Trash -- " + std::to_string(n) + (n == 1 ? " item" : " items") +
            ", one step. Undo in Activity brings it all back; the space returns when you empty the Trash.",
        "Clean " + core::scan::human_bytes(bytes), keep, warn);
    if (auto lg = log::get(log::Area::Dev))
        lg->info("clean preview: '{}', {} actions, {} bytes, {} asked again", m_dv_plan.label, n, bytes, asked.size());
}

void Shell::on_dev_push(const std::string& rel) {  // handler: s020a -- Push... -> the preview
    const auto* p = project_at(m_dv_shown, rel);
    if (!p || door_busy()) return;
    m_dv_push_plan = core::dev::push_plan(*p);
    if (!m_dv_push_plan.why_not.empty()) { m_dv_gate.set_text("Can't push " + p->name + ": " + m_dv_push_plan.why_not); return; }
    m_dv_plan = {};
    m_dv_cf_rel = rel;
    m_dv_cf_push = true;
    std::vector<std::string> warn = m_dv_push_plan.stays;
    warn.push_back("A push can't be undone from Activity -- it only adds to " + p->repo.remote +
                   "; nothing of yours on this computer changes. Never forced: if the remote moved on, it's refused.");
    paint_dev_confirm("Push " + p->name + "?",
        "Committed work in ~/" + p->rel + (p->app.empty() ? "" : "/" + p->app) + " that isn't on " + p->repo.remote +
            " yet goes there now. Then Rouster asks the remote again.",
        "Push " + p->name, m_dv_push_plan.what, warn);
    if (auto lg = log::get(log::Area::Dev)) lg->info("push preview '{}': {} steps", rel, m_dv_push_plan.steps.size());
}

void Shell::on_dev_confirm() {  // handler: the preview's act button -> worker
    if (m_dv_cf_push) {   // s020a: git push on a worker, then verify again
        if (door_busy() || m_dv_worker.joinable() || m_dv_push_plan.steps.empty()) return;
        if (m_dv_states.get_visible_child_name() != "confirm") return;
        if (m_dv_act_worker.joinable()) m_dv_act_worker.join();
        const auto* p = project_at(m_dv_shown, m_dv_cf_rel);
        if (!p) return;
        m_dv_acting = true;
        if (m_act_dv_find) m_act_dv_find->set_enabled(false);
        m_dv_run_big.set_text("Pushing");
        m_dv_run_caption.set_text("To " + p->repo.remote + " (" + p->repo.host + "), then asking it again");
        if (m_act_dv_cancel) m_act_dv_cancel->set_enabled(false);
        m_dv_spinner.start();
        m_dv_states.set_visible_child("running");
        if (auto lg = log::get(log::Area::Dev)) lg->info("push '{}' -- go", m_dv_cf_rel);
        const std::string repo_abs = m_home + "/" + p->rel + (p->app.empty() ? "" : "/" + p->app);
        m_dv_act_worker = std::thread([this, plan = m_dv_push_plan, repo_abs] {
            auto r = core::dev::push(repo_abs, plan);
            {
                std::lock_guard lk(m_dv_act_mx);
                m_dv_pushed = std::move(r);
            }
            m_dev_acted.emit();
        });
        return;
    }
    if (!m_door || door_busy() || m_dv_worker.joinable() || m_dv_plan.actions.empty()) return;
    if (m_dv_states.get_visible_child_name() != "confirm") return;   // only from the preview
    if (m_dv_act_worker.joinable()) m_dv_act_worker.join();
    paint_preflight();
    if (m_pf_red) {
        m_dv_cf_sub.set_text("Preflight has a red check -- nothing moves until it clears. See Preflight.");
        return;
    }
    // Who is asked again: rel, repo folder, the commit on screen.
    struct Ask { std::string name, abs, head; };
    std::vector<Ask> asks;
    for (const auto* p : core::dev::source_ticked(m_dv_shown.projects))
        asks.push_back({p->name, m_home + "/" + p->repo_rel, p->repo.head});
    m_dv_plan.previewed = true;   // the confirm screen showed every step
    m_dv_acting = true;
    if (m_act_undo_action) m_act_undo_action->set_enabled(false);
    if (m_act_dv_find) m_act_dv_find->set_enabled(false);
    m_dv_run_big.set_text("Cleaning");
    m_dv_run_caption.set_text(asks.empty() ? std::string("Moving to the Trash")
                                           : "Asking the remote once more, then moving to the Trash");
    if (m_act_dv_cancel) m_act_dv_cancel->set_enabled(false);   // a half-run plan is the door's to finish
    m_dv_spinner.start();
    m_dv_states.set_visible_child("running");
    if (auto lg = log::get(log::Area::Dev)) lg->info("clean -- go ({} asked again)", asks.size());

    m_dv_act_worker = std::thread([this, plan = m_dv_plan, asks] {
        core::door::Result r;
        std::string refused;
        for (const auto& a : asks) {
            const auto again = core::dev::verify(a.abs);
            if (again.status != core::dev::Status::Backed) {
                refused = a.name + " is no longer backed up (" + core::dev::status_name(again.status) + ")";
                for (const auto& c : again.checks)
                    if (!c.ok) { refused += ": " + c.detail; break; }
            } else if (again.head != a.head) {
                refused = a.name + " changed since you looked (now at " + again.head.substr(0, 7) + ") -- Look again";
            }
            if (!refused.empty()) break;
        }
        if (refused.empty()) r = door_run(plan);
        {
            std::lock_guard lk(m_dv_act_mx);
            m_dv_act_result = r;
            m_dv_act_refused = refused;
        }
        m_dev_acted.emit();
    });
}

void Shell::on_dev_acted() {  // handler: clean / push finished (Dispatcher -> UI thread)
    if (m_dv_act_worker.joinable()) m_dv_act_worker.join();
    m_dv_acting = false;
    m_dv_spinner.stop();
    if (m_act_dv_find) m_act_dv_find->set_enabled(true);
    if (m_dv_cf_push) {   // s020a
        m_dv_cf_push = false;
        core::dev::Pushed r;
        {
            std::lock_guard lk(m_dv_act_mx);
            r = m_dv_pushed;
        }
        auto* p = const_cast<core::dev::Project*>(project_at(m_dv_shown, m_dv_cf_rel));
        const std::string name = p ? p->name : m_dv_cf_rel;
        if (p) p->repo = r.after;   // the card shows what the remote says now
        std::string said;
        for (const auto& s : r.said) said += (said.empty() ? "" : "; ") + s;
        if (auto lg = log::get(log::Area::Dev))
            lg->info("push '{}': {} -- now {}", m_dv_cf_rel, said, core::dev::status_name(r.after.status));
        if (r.ok && r.after.status == core::dev::Status::Backed) {
            m_dv_done_big.set_text("Pushed");
            m_dv_done_caption.set_text(name + " is on " + r.after.remote + " (" + r.after.host + ") -- asked again, backed up.");
        } else if (r.ok) {
            std::string left;
            for (const auto& c : r.after.checks) if (!c.ok) { left = c.name + ": " + c.detail; break; }
            m_dv_done_big.set_text("Pushed");
            m_dv_done_caption.set_text(name + " was pushed (" + said + "), but it still isn't all on " + r.after.remote +
                                       (left.empty() ? "." : " -- " + left + "."));
        } else {
            m_dv_done_big.set_text("Not pushed");
            m_dv_done_caption.set_text(name + ": " + (said.empty() ? std::string("nothing ran") : said) +
                                       ". Nothing on this computer changed.");
        }
        m_dv_push_plan = {};
        paint_dev();
        m_dv_states.set_visible_child("done");
        return;
    }
    core::door::Result r;
    std::string refused;
    {
        std::lock_guard lk(m_dv_act_mx);
        r = m_dv_act_result;
        refused = m_dv_act_refused;
    }
    std::uint64_t bytes = 0;
    for (const auto& a : m_dv_plan.actions) bytes += a.bytes;
    if (auto lg = log::get(log::Area::Dev)) {
        lg->info("clean '{}': {} done, {} failed{}", m_dv_plan.label, r.done, r.failed,
                 refused.empty() ? "" : " -- NOT RUN: " + refused);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    if (!refused.empty()) {
        m_dv_done_big.set_text("Nothing moved");
        m_dv_done_caption.set_text("Every project was left exactly as it was: " + refused + ".");
    } else if (r.ok) {
        m_dv_done_big.set_text(core::scan::human_bytes(bytes));
        m_dv_done_caption.set_text("went to the Trash -- " + std::to_string(r.done) + (r.done == 1 ? " item" : " items") +
            ". Undo puts it all back now; git restore . brings a cleaned source back any time. "
            "The space returns when you empty the Trash.");
    } else if (r.done == 0) {
        m_dv_done_big.set_text("Nothing moved");
        m_dv_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_dv_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(m_dv_plan.actions.size()));
        m_dv_done_caption.set_text("went to the Trash; " + std::to_string(r.failed) + " could not: " +
            (r.messages.empty() ? std::string("see Activity") : r.messages.front()) + ". Undo takes back what moved.");
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
        if (p.kind == core::dev::Kind::Workspace && !p.repo.cleaned) {
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

void Shell::on_help() {  // handler: s013a2 -- Help, open at the page you're on
    if (!m_help) m_help = std::make_unique<HelpWindow>(*this);
    m_help->show_topic(m_stack.get_visible_child_name());
    m_help->present();
}

void Shell::on_about() {  // handler: open the About window
    // Hide-on-close singleton (CANON: "Lifetime shape is design").
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

}  // namespace rouster
