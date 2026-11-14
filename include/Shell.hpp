#pragma once
#include <array>
#include <set>
#include "widgets/Widgets.hpp"
#include "core/Scan.hpp"     // s002: the read-only walker
#include <map>
#include <functional>
#include "core/Door.hpp"     // s004: the one door
#include "core/TidyLink.hpp" // s019d: Tidy's hands-off + marks
#include "core/Sweep.hpp"    // s006: the first module that acts
#include "core/Dev.hpp"      // s007: dev projects
#include "core/Dups.hpp"     // s008: duplicates
#include "core/Profile.hpp"  // s012: profile backup
#include "core/System.hpp"   // s013b: package cache, journal, trim (root)
#include <gtkmm/stack.h>
#include <gtkmm/spinner.h>

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/dispatcher.h>

#include <gtkmm/dropdown.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/textview.h>
#include <gtkmm/passwordentry.h>
#include <gtkmm/checkbutton.h>
#include <atomic>

#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar.
//
// Split across TUs by CATEGORY OF CODE (CANON: "the header is the index").
// Every method carries a trailing `// category: ...` tag naming its file.
//
// Vocabulary
// ----------
//   Zones    -- construction: the window frame, the sidebar, each module
//               page, the menu model.        File: src/Shell_zones.cpp
//   Bindings -- actions + the recents group. File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*).          File: src/Shell_handlers.cpp
//   Helpers  -- orchestration + painting.    File: src/Shell_helpers.cpp
//   Glue     -- ctor/dtor + build_ui().      File: src/Shell.cpp
//
// The look (s005) lives in resources/rouster.css, loaded by App. Widgets carry
// CSS classes (hero-title, orb, card, ...); colours never live in C++.
//
// Finding things
// --------------
//   "Where is the sidebar built?"        -> Shell_zones.cpp   (build_sidebar)
//   "Where is the Smart Scan page?"      -> Shell_zones.cpp   (build_scan_page: start / running / results)
//   "Where do the result cards come from?" -> Shell_helpers.cpp (paint_report / paint_detail)
//   "What thread does the walking?"      -> Shell_helpers.cpp (start_scan: m_worker)
//   "Where is the Sweep page?"           -> Shell_zones.cpp   (build_sweep_page: start / running / results / done)
//   "Where does Sweep act?"              -> Shell_handlers.cpp (on_sweep_run -> m_door->run)
//   "Where is the Dev projects page?"    -> Shell_zones.cpp   (build_dev_page); cards: Shell_helpers.cpp (paint_dev)
//   "Where does Clean act?"              -> Shell_handlers.cpp (on_dev_run -> preview; on_dev_confirm -> worker -> door)
//   "Where is the Duplicates page?"      -> Shell_zones.cpp   (build_dups_page); groups: Shell_helpers.cpp (paint_dups)
//   "Where does Duplicates act?"         -> Shell_handlers.cpp (on_dups_run -> worker -> m_door->run)
//   "Where is the Profile backup page?"  -> Shell_zones.cpp   (build_profile_page); rows: Shell_helpers.cpp (paint_profile)
//   "Where does a profile backup act?"   -> Shell_handlers.cpp (on_profile_run -> m_door->run)
//   "Where does a write go?"             -> core/Door.* -- nowhere else (the selftest's source gate)
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster {

class AboutWindow;
class HelpWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();
    std::string config_home() const;             // category: helper: ~/.config (a fixture's own) -- Samson's settings                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: window + header + body
    void build_sidebar();                        // category: zone: brand + sections + module rows
    void build_pages();                          // category: zone: the module pages
    void build_scan_page();                      // category: zone: Smart Scan (s002; look s005)
    void build_sweep_page();                     // category: zone: Sweep (s006)
    void build_system_page();                    // category: zone: System (s013b) -- root housekeeping
    void build_dev_page();                       // category: zone: Dev projects (s007)
    void build_dups_page();                      // category: zone: Duplicates (s008)
    void build_profile_page();                   // category: zone: Profile backup (s012a)
    void build_preflight_page();                 // category: zone: Preflight (s003)
    void build_activity_page();                  // category: zone: Activity (s004)
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + recents group

    // ── handlers ───────────────────────────────────────────────────────────
    void on_nav(const std::string& page);        // category: handler: a sidebar row was chosen
    void on_scan();                              // category: handler: Smart Scan the home folder
    void on_scan_cancel();                       // category: handler: stop the running scan
    void on_scan_done();                         // category: handler: the worker finished (Dispatcher, UI thread)
    void on_undo();                              // category: handler: undo the last batch, through the door
    void on_sweep_find();                        // category: handler: look for junk (worker)
    void on_sweep_cancel();                      // category: handler: stop looking
    void on_sweep_found();                       // category: handler: the worker finished (Dispatcher, UI thread)
    void on_sweep_run();                         // category: handler: the checked items -> a plan -> the door
    void on_dev_find();                          // category: handler: find + verify the dev projects (worker)
    void on_dev_cancel();                        // category: handler: stop looking
    void on_dev_found();                         // category: handler: the worker finished (Dispatcher, UI thread)
    void on_dev_run();                           // category: handler: s020b -- the ticked rows -> a plan -> the preview
    void on_dev_push(const std::string& rel);    // category: handler: s020a -- a card's Push... -> the preview
    void on_dev_confirm();                       // category: handler: the preview's act button -> worker (verify again, door)
    void on_dev_acted();                         // category: handler: clean / push finished (Dispatcher, UI thread)
    void on_dev_known(const std::string& rel, const std::string& name, bool add);  // category: handler: promote / forget a lint name
    void on_dups_find();                         // category: handler: look for duplicates (worker)
    void on_dups_cancel();                       // category: handler: stop looking
    void on_dups_found();                        // category: handler: the worker finished (Dispatcher, UI thread)
    void on_dups_mode(core::dups::Mode m);       // category: handler: Share / Trash switch -> re-preset the checks
    void on_dups_keep(std::size_t group, std::size_t member);   // category: handler: make another member the keeper
    void on_dups_run();                          // category: handler: the checked copies -> a plan -> worker -> the door
    void on_dups_acted();                        // category: handler: the door finished (Dispatcher, UI thread)
    void on_system_look();                       // category: handler: s013b -- measure the three (as you)
    void on_system_run();                        // category: handler: the checked rows -> a plan -> worker -> the door -> pkexec
    void on_system_acted();                      // category: handler: the helper answered (Dispatcher, UI thread)
    void paint_system();                         // category: helper: the rows, the warnings
    void system_gate();                          // category: helper: the act button's label + whether it may act
    void stop_system();                          // category: helper: join a run in flight (close, dtor)
    void on_snapshot_take();                     // category: handler: s013 -- Preflight's Take snapshot -> worker -> the door -> pkexec
    void on_snapshot_taken();                    // category: handler: the helper answered (Dispatcher, UI thread)
    void stop_preflight();                       // category: helper: join a snapshot in flight (close, dtor)
    void on_profile_find();                      // category: handler: gather the set-up (worker: dconf, dnf, flatpak)
    void on_profile_found();                     // category: handler: the worker finished (Dispatcher, UI thread)
    void on_profile_run();                       // category: handler: checked rows -> a plan (one new folder) -> the door
    void on_profile_open();                      // category: handler: open the bundle in Files
    void on_profile_look_back(const std::string& name);   // category: handler: a backup's Restore... -> worker: diff against home
    void on_profile_looked();                    // category: handler: the restore look finished (Dispatcher, UI thread)
    void on_profile_packed();                    // category: handler: s012c -- the secrets are encrypted (Dispatcher, UI thread)
    void on_profile_open_secrets();              // category: handler: s012c -- restore's Open -> worker: decrypt in memory + compare
    void on_profile_secrets_opened();            // category: handler: s012c -- the secrets were opened (Dispatcher, UI thread)
    void on_profile_restore();                   // category: handler: checked rows -> Trash + Copy plan -> the door
    void on_preflight();                         // category: handler: run the checklist again
    void on_pick_folder();                       // category: handler: folder chooser -> scan it
    void on_recent_open(const std::string& p);   // category: handler: scan a recent folder again
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window
    void on_help();                              // category: handler: s013a2 -- Help at the current page's topic (F1)
    void on_shortcuts();                         // category: handler: the keyboard reference

    // ── helpers ────────────────────────────────────────────────────────────
    std::string home_dir() const;                // category: helper: $ROUSTER_HOME, else the real home
    std::string data_dir() const;                // category: helper: where the manifest lives
    void start_scan(const std::string& root);    // category: helper: spawn the worker
    void stop_scan();                            // category: helper: cancel + join (close, dtor)
    bool tick_progress();                        // category: helper: the big live counters
    void paint_report();                         // category: helper: headline + one card per kind
    void paint_detail();                         // category: helper: the selected card's biggest folders
    void set_scanning(bool on);                  // category: helper: which scan state shows; actions follow
    void paint_activity();                       // category: helper: the manifest's batches, newest first
    void paint_preflight();                      // category: helper: gather + evaluate + repaint the cards
    void stop_sweep();                           // category: helper: cancel + join (close, dtor)
    bool tick_sweep();                           // category: helper: the live counters while looking
    void paint_sweep();                          // category: helper: groups, rows with checks, the Trash line
    void sweep_gate();                           // category: helper: the act button's label + whether it may act
    void stop_dev();                             // category: helper: cancel + join (close, dtor)
    bool tick_dev();                             // category: helper: the live counters while looking
    void paint_dev();                            // category: helper: one card per project: verdict, checks, lint, build rows
    void dev_gate();                             // category: helper: the light-clean button's label + whether it may act
    void paint_dev_confirm(const std::string& title, const std::string& sub, const std::string& go,
                           const std::vector<std::string>& keep, const std::vector<std::string>& warn);  // category: helper: the clean / push preview
    void stop_dups();                            // category: helper: cancel + join (close, dtor)
    bool tick_dups();                            // category: helper: the live counters while looking
    void dups_preset();                          // category: helper: checks for the mode; nothing unshown stays checked
    void paint_dups();                           // category: helper: mode switch, one card per group, rows with checks
    void dups_gate();                            // category: helper: the act button's label + whether it may act
    void stop_profile();                         // category: helper: cancel + join (close, dtor)
    void paint_profile();                        // category: helper: cards per group, rows with checks, what wasn't gathered
    void profile_backup(std::string secrets, std::size_t secret_files);   // category: helper: plan + the door (after any encrypting)
    void profile_gate();                         // category: helper: the act button's label + whether it may act
    void paint_profile_start();                  // category: helper: the earlier backups, each with Restore...
    void paint_profile_restore();                // category: helper: the restore preview -- files by state, diffs, commands
    void profile_restore_gate();                 // category: helper: the restore button's label + whether it may act                         // category: helper: the act button's label + whether it may act
    core::door::Result door_run(const core::door::Plan& plan);   // category: helper: s019d -- tidylink::guard, then the door (the app's only Door::run)
    bool door_busy() const;                      // category: helper: a clean / duplicates / profile / system run owns the door right now
    void go_to(const std::string& page);         // category: helper: select a module's sidebar row
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                 // category: helper: repaint the submenu
    std::string recents_file() const;            // category: helper: the XDG path (UI-side resolution)

    // Titlebar: brand logo (start) + hamburger (end).
    widgets::Button     m_logo_button;
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack.
    widgets::Box     m_body;          // horizontal
    widgets::Box     m_side;          // vertical, .sidebar-pane
    widgets::ListBox m_nav;           // module rows (+ unselectable section headers)
    widgets::Stack   m_stack;
    std::string      m_page;          // the visible module ("scan", ...) -> window class page-<name>

    // Smart Scan: three states in one stack.
    widgets::Box     m_scan_page;     // vertical
    widgets::Stack   m_scan_states;   // "start" | "running" | "results"
    widgets::Label   m_scan_sub;      // the start screen's one line
    widgets::Button  m_scan_button;   // the orb
    widgets::Spinner m_spinner;
    widgets::Label   m_run_big;       // "3.4 GB"
    widgets::Label   m_run_caption;   // "12,345 files looked at"
    widgets::Button  m_cancel_button;
    widgets::Label   m_res_big;       // "279 KB"
    widgets::Label   m_res_caption;
    widgets::Label   m_res_where;
    widgets::FlowBox m_cards;         // one card per kind
    widgets::Box     m_detail;        // the selected kind's folders
    core::scan::Report m_shown;       // the report on screen (UI thread only)
    int              m_sel_kind = 0;

    // The worker. Declaration order matters: the dispatcher outlives the thread
    // (stop_scan joins in the dtor before members go).
    Glib::Dispatcher                      m_scan_finished;
    std::shared_ptr<core::scan::Progress> m_progress;
    std::thread                           m_worker;
    std::mutex                            m_report_mx;
    core::scan::Report                    m_report;     // written by the worker, read after done
    sigc::connection                      m_tick;
    std::string                           m_home;

    // Sweep (s006): start | running | results | done.
    widgets::Box     m_sw_page;
    widgets::Stack   m_sw_states;
    widgets::Label   m_sw_sub;
    widgets::Button  m_sw_find;       // the orb
    widgets::Spinner m_sw_spinner;
    widgets::Label   m_sw_run_big;
    widgets::Label   m_sw_run_caption;
    widgets::Button  m_sw_cancel;
    widgets::Label   m_sw_big;        // the checked total
    widgets::Label   m_sw_caption;
    widgets::Box     m_sw_list;       // groups + rows
    widgets::Button  m_sw_go;         // "Move 1.2 GB to the Trash"
    widgets::Label   m_sw_gate;       // why it can't, when it can't
    widgets::Label   m_sw_done_big;
    widgets::Label   m_sw_done_caption;
    core::sweep::Found m_sw_shown;    // what the list shows; checks live here (UI thread only)
    Glib::Dispatcher                      m_sweep_finished;
    std::shared_ptr<core::scan::Progress> m_sw_progress;
    std::thread                           m_sw_worker;
    std::mutex                            m_sw_mx;
    core::sweep::Found                    m_sw_found;   // written by the worker, read after done
    sigc::connection                      m_sw_tick;
    Glib::RefPtr<Gio::SimpleAction>       m_act_sw_find, m_act_sw_cancel, m_act_sw_run;

    // Dev projects (s007): start | running | results | done.
    widgets::Box     m_dv_page;
    widgets::Stack   m_dv_states;
    widgets::Label   m_dv_sub;
    widgets::Button  m_dv_find;       // the orb
    widgets::Spinner m_dv_spinner;
    widgets::Label   m_dv_run_big;
    widgets::Label   m_dv_run_caption;
    widgets::Button  m_dv_cancel;
    widgets::Label   m_dv_big;        // the ticked total
    widgets::Label   m_dv_caption;
    widgets::Box     m_dv_list;       // one card per project
    widgets::Button  m_dv_go;         // "Clean 1.2 GB…" -> the preview
    widgets::Label   m_dv_gate;
    widgets::Label   m_dv_done_big;
    widgets::Label   m_dv_done_caption;
    core::dev::Found m_dv_shown;      // what the cards show; checks live here (UI thread only)
    Glib::Dispatcher                      m_dev_finished;
    std::shared_ptr<core::scan::Progress> m_dv_progress;
    std::thread                           m_dv_worker;
    std::mutex                            m_dv_mx;
    core::dev::Found                      m_dv_found;   // written by the worker, read after done
    sigc::connection                      m_dv_tick;
    Glib::RefPtr<Gio::SimpleAction>       m_act_dv_find, m_act_dv_cancel, m_act_dv_run;
    // Clean (s020b) / push (s020a): the preview ("confirm" state), then a worker
    // that owns the door until it is done (Clean asks the remote once more).
    widgets::Label   m_dv_cf_title;
    widgets::Label   m_dv_cf_sub;
    widgets::Box     m_dv_cf_list;    // the plan, step by step -- the preview
    widgets::Button  m_dv_cf_go;
    core::door::Plan m_dv_plan;       // what the preview shows, and exactly what runs
    std::string      m_dv_cf_rel;     // the project it is for
    bool             m_dv_cf_push = false;    // s020a: the preview is a push (git, not the door)
    core::dev::PushPlan m_dv_push_plan;
    core::dev::Pushed   m_dv_pushed;          // worker -> UI (m_dv_act_mx)
    bool             m_dv_acting = false;   // UI thread: the act worker owns m_door
    Glib::Dispatcher m_dev_acted;
    std::thread      m_dv_act_worker;
    std::mutex       m_dv_act_mx;
    core::door::Result m_dv_act_result;   // written by the worker
    std::string        m_dv_act_refused;  // the re-check said no (nothing ran)
    std::vector<std::string> m_known;     // lint names promoted to known-optional (core/Known)

    // Duplicates (s008): start | running | results | done.
    widgets::Box     m_du_page;
    widgets::Stack   m_du_states;
    widgets::Label   m_du_sub;
    widgets::Button  m_du_find;       // the orb
    widgets::Spinner m_du_spinner;
    widgets::Label   m_du_run_big;
    widgets::Label   m_du_run_caption;
    widgets::Button  m_du_cancel;
    widgets::Label   m_du_big;        // what the checked copies would free
    widgets::Label   m_du_caption;
    widgets::Box     m_du_list;       // mode switch + one card per group
    widgets::Button  m_du_go;
    widgets::Label   m_du_gate;
    widgets::Label   m_du_done_big;
    widgets::Label   m_du_done_caption;
    core::dups::Found m_du_shown;     // what the cards show; checks + keepers live here (UI thread only)
    core::dups::Mode  m_du_mode = core::dups::Mode::Share;
    static constexpr std::size_t kDupsShown = 150;   // cards painted; groups past it are never checked
    Glib::Dispatcher                       m_dups_finished;
    std::shared_ptr<core::dups::Progress>  m_du_progress;
    std::thread                            m_du_worker;
    std::mutex                             m_du_mx;
    core::dups::Found                      m_du_found;   // written by the worker, read after done
    sigc::connection                       m_du_tick;
    Glib::RefPtr<Gio::SimpleAction>        m_act_du_find, m_act_du_cancel, m_act_du_run;
    bool               m_du_acting = false;   // UI thread: the act worker owns m_door
    Glib::Dispatcher   m_dups_acted;
    std::thread        m_du_act_worker;
    std::mutex         m_du_act_mx;
    core::door::Result m_du_act_result;
    std::uint64_t      m_du_act_bytes = 0;
    std::size_t        m_du_act_total = 0;
    std::vector<std::string> m_du_dropped;   // left out of the plan, and why

    // Profile backup (s012a): start | running | results | done.
    widgets::Box     m_pr_page;
    widgets::Stack   m_pr_states;
    widgets::Label   m_pr_sub;
    widgets::Label   m_pr_last;        // the newest earlier backup, on the start page
    widgets::Button  m_pr_find;
    widgets::Spinner m_pr_spinner;
    widgets::Label   m_pr_big;
    widgets::Label   m_pr_caption;
    widgets::Box     m_pr_list;
    widgets::Button  m_pr_go;
    widgets::Label   m_pr_gate;
    widgets::Label   m_pr_done_big;
    widgets::Label   m_pr_done_caption;
    Gtk::TextView*   m_pr_notes = nullptr;        // free-form notes (Rouster's own profile-notes.md)
    core::profile::Found m_pr_shown;              // checks live here (UI thread only)
    std::string      m_pr_bundle;                 // the last bundle made
    static constexpr std::size_t kProfileShown = 400;   // rows painted per group; unpainted rows are never checked
    Glib::Dispatcher                  m_profile_finished;
    std::thread                       m_pr_worker;
    std::mutex                        m_pr_mx;
    std::atomic<bool>                 m_pr_cancel{false};
    core::profile::Found              m_pr_found;
    Glib::RefPtr<Gio::SimpleAction>   m_act_pr_find, m_act_pr_run;
    // Restore (s012b): the "restore" state.
    widgets::Box     m_pr_backups;                // start page: earlier backups (painted)
    widgets::Label   m_pr_rs_title;
    widgets::Label   m_pr_rs_sub;
    widgets::Box     m_pr_rs_list;
    widgets::Label   m_pr_rs_gate;
    widgets::Button  m_pr_rs_go;
    core::profile::Restore m_pr_rs;               // checks live here (UI thread only)
    core::profile::Restore m_pr_rs_found;         // written by the worker
    Glib::Dispatcher m_profile_looked;
    // Secrets (s012c): opt-in on the backup, opened on the restore.
    Gtk::CheckButton*    m_pr_sec_on = nullptr;     // painted with the results; off by default
    Gtk::PasswordEntry*  m_pr_pass1 = nullptr;
    Gtk::PasswordEntry*  m_pr_pass2 = nullptr;
    Gtk::Label*          m_pr_sec_state = nullptr;  // the passphrase line under the boxes
    Gtk::PasswordEntry*  m_pr_rs_pass = nullptr;    // restore: the passphrase to open with
    bool                 m_pr_packing = false;      // the worker is encrypting (the act button waits)
    core::secrets::Packed m_pr_packed;              // worker -> UI (m_pr_mx)
    core::secrets::Opened m_pr_sec;                 // restore: what the secrets hold vs home (UI thread)
    core::secrets::Opened m_pr_sec_found;           // worker -> UI (m_pr_mx)
    Glib::Dispatcher     m_profile_packed;
    Glib::Dispatcher     m_profile_sec_opened;

    // Preflight (s003).
    bool            m_pf_red = true;  // a red check stops every act button; true until the first run
    widgets::Box    m_pf_page;
    widgets::Label  m_pf_summary;
    widgets::Box    m_pf_rows;
    widgets::Button m_pf_button;
    // s013b: System -- root housekeeping (plain Gtk members, like m_pf_said).
    Gtk::Box           m_sy_page{Gtk::Orientation::VERTICAL, 0};
    Gtk::Stack         m_sy_states;
    Gtk::Box           m_sy_list{Gtk::Orientation::VERTICAL, 10};
    Gtk::Label         m_sy_sub, m_sy_caption, m_sy_gate, m_sy_run_caption, m_sy_done_big, m_sy_done_caption;
    Gtk::Button        m_sy_go;
    Gtk::Image*        m_sy_done_icon = nullptr;   // the check -- or the System disc when nothing ran
    Gtk::Spinner       m_sy_spinner;
    std::vector<core::sysclean::Item> m_sy_items;
    bool               m_sy_acting = false;
    std::thread        m_sy_worker;
    std::mutex         m_sy_mx;
    core::door::Result m_sy_result;
    core::door::Plan   m_sy_plan;
    Glib::Dispatcher   m_system_acted;
    // s013: Take snapshot -- the door's Root verb on a worker (pkexec waits on the password).
    Gtk::Label         m_pf_said;
    bool               m_pf_acting = false;   // UI thread: the snapshot worker owns m_door
    std::thread        m_pf_worker;
    std::mutex         m_pf_mx;
    core::door::Result m_pf_result;
    Glib::Dispatcher   m_snapshot_taken;

    // Activity (s004): what went through the door, and the way back.
    std::unique_ptr<core::door::Door> m_door;
    widgets::Box    m_act_page;
    widgets::Button m_act_undo;
    widgets::Label  m_act_said;
    widgets::Box    m_act_list;
    Glib::RefPtr<Gio::SimpleAction> m_act_undo_action;

    // Recents: the folders scanned, newest first.
    std::vector<std::string>        m_recents;
    std::string                     m_recents_file;
    Glib::RefPtr<Gio::Menu>         m_recents_menu;
    Glib::RefPtr<Gio::SimpleAction> m_recents_clear;
    Glib::RefPtr<Gio::SimpleAction> m_act_scan, m_act_cancel, m_act_pick;

    std::unique_ptr<AboutWindow>     m_about;
    std::unique_ptr<HelpWindow>      m_help;    // s013a2: F1, opens at the page you're on
    std::unique_ptr<ShortcutsDialog> m_shortcuts;
};

}  // namespace rouster
