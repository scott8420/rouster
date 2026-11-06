#pragma once
#include "widgets/Widgets.hpp"
#include "core/Scan.hpp"     // s002: the read-only walker
#include "core/Door.hpp"     // s004: the one door
#include "core/Sweep.hpp"    // s006: the first module that acts
#include "core/Dev.hpp"      // s007: dev projects

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/dispatcher.h>

#include <memory>
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
//   "Where does the light clean act?"    -> Shell_handlers.cpp (on_dev_run -> m_door->run)
//   "Where does a write go?"             -> core/Door.* -- nowhere else (the selftest's source gate)
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster {

class AboutWindow;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: window + header + body
    void build_sidebar();                        // category: zone: brand + sections + module rows
    void build_pages();                          // category: zone: the module pages
    void build_scan_page();                      // category: zone: Smart Scan (s002; look s005)
    void build_sweep_page();                     // category: zone: Sweep (s006)
    void build_dev_page();                       // category: zone: Dev projects (s007)
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
    void on_dev_run();                           // category: handler: the checked build dirs -> a plan -> the door
    void on_preflight();                         // category: handler: run the checklist again
    void on_pick_folder();                       // category: handler: folder chooser -> scan it
    void on_recent_open(const std::string& p);   // category: handler: scan a recent folder again
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window
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
    widgets::Label   m_dv_big;        // the checked build output
    widgets::Label   m_dv_caption;
    widgets::Box     m_dv_list;       // one card per project
    widgets::Button  m_dv_go;         // "Move 1.2 GB of build output to the Trash"
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

    // Preflight (s003).
    bool            m_pf_red = true;  // a red check stops every act button; true until the first run
    widgets::Box    m_pf_page;
    widgets::Label  m_pf_summary;
    widgets::Box    m_pf_rows;
    widgets::Button m_pf_button;

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
    std::unique_ptr<ShortcutsDialog> m_shortcuts;
};

}  // namespace rouster
