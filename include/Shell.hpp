#pragma once
#include "widgets/Widgets.hpp"
#include "core/Scan.hpp"     // s002: the read-only walker
#include "core/Door.hpp"     // s004: the one door

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
// This class is split across multiple TUs by CATEGORY OF CODE, not by feature
// (CANON: "vocabulary first, file layout second" + "the header is the index").
// The header below IS the index -- every method carries a trailing
// `// category: ...` tag naming the file its body lives in.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: the window frame, header,
//               sidebar+stack, the module pages, the menu model.
//               File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: Gio actions, the recents action group.
//               File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*): what a user action actually does.
//               File: src/Shell_handlers.cpp
//   Helpers  -- reusable sync/orchestration called from zones/bindings/handlers.
//               File: src/Shell_helpers.cpp
//   Glue     -- what stays in Shell.cpp: the ctor/dtor + build_ui().
//
// Finding things
// --------------
//   "Where is the Smart Scan page built?" -> Shell_zones.cpp    (build_scan_page)
//   "Where does a scan start / finish?"   -> Shell_helpers.cpp  (start_scan / paint_report)
//   "What thread does the walking?"       -> Shell_helpers.cpp  (start_scan: m_worker)
//   "Where is the Preflight checklist?"   -> Shell_zones.cpp (page) + Shell_helpers.cpp (paint_preflight)
//   "Where does a write go?"              -> core/Door.* -- nowhere else (the selftest's source gate)
//   "Where are actions registered?"       -> Shell_bindings.cpp (bind_actions)
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
    void build_shell();                          // category: zone: window + sidebar + stack
    void build_pages();                          // category: zone: the module pages
    void build_scan_page();                      // category: zone: Smart Scan (s002)
    void build_preflight_page();                 // category: zone: Preflight (s003)
    void build_activity_page();                  // category: zone: Activity -- what the door did (s004)
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + recents group

    // ── handlers ───────────────────────────────────────────────────────────
    void on_scan();                              // category: handler: Smart Scan the home folder
    void on_scan_cancel();                       // category: handler: stop the running scan
    void on_scan_done();                         // category: handler: the worker finished (Dispatcher, UI thread)
    void on_undo();                              // category: handler: undo the last batch, through the door
    void on_preflight();                         // category: handler: run the checklist again
    void on_pick_folder();                       // category: handler: folder chooser -> scan it
    void on_recent_open(const std::string& p);   // category: handler: scan a recent folder again
    void on_recent_clear();                      // category: handler: empty the recents list
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: open the About window
    void on_shortcuts();                         // category: handler: the keyboard reference

    // ── helpers ────────────────────────────────────────────────────────────
    std::string home_dir() const;                // category: helper: $ROUSTER_HOME, else the real home
    void start_scan(const std::string& root);    // category: helper: spawn the worker
    void stop_scan();                            // category: helper: cancel + join (close, dtor)
    bool tick_progress();                        // category: helper: repaint the live counters
    void paint_report();                         // category: helper: the summary, by kind
    void set_scanning(bool on);                  // category: helper: buttons + spinner follow the state
    void paint_activity();                       // category: helper: the manifest's batches, newest first
    std::string data_dir() const;                // category: helper: where the manifest lives
    void paint_preflight();                      // category: helper: gather + evaluate + repaint the rows
    void note_recent(const std::string& path);   // category: helper: push onto the list + persist
    void rebuild_recents_menu();                 // category: helper: repaint the submenu
    std::string recents_file() const;            // category: helper: the XDG path (UI-side resolution)

    // Titlebar: brand logo (start) + hamburger (end).
    widgets::Button     m_logo_button;
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack (each module is a stack page).
    widgets::Box          m_body;
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // Smart Scan (s002).
    widgets::Box            m_scan_page;      // vertical
    widgets::Label          m_scan_where;     // "Home: /home/scott"
    widgets::Button         m_scan_button;
    widgets::Button         m_cancel_button;
    widgets::Spinner        m_spinner;
    widgets::Label          m_scan_status;    // live counters, then the total
    widgets::ScrolledWindow m_scan_scroll;
    widgets::Box            m_scan_results;   // one Expander per kind, rebuilt per report

    // Preflight (s003).
    widgets::Box    m_pf_page;       // vertical
    widgets::Label  m_pf_summary;
    widgets::Box    m_pf_rows;       // one row per check, rebuilt per run
    widgets::Button m_pf_button;

    // Activity (s004): what went through the door, and the way back.
    std::unique_ptr<core::door::Door> m_door;
    widgets::Box    m_act_page;      // vertical
    widgets::Button m_act_undo;
    widgets::Label  m_act_said;
    widgets::Box    m_act_list;      // one Expander per batch, rebuilt
    Glib::RefPtr<Gio::SimpleAction> m_act_undo_action;

    // The worker. Declaration order matters: the dispatcher outlives the thread
    // (stop_scan joins in the dtor before members go).
    Glib::Dispatcher                            m_scan_finished;
    std::shared_ptr<core::scan::Progress>       m_progress;
    std::thread                                 m_worker;
    std::mutex                                  m_report_mx;
    core::scan::Report                          m_report;     // written by the worker, read after done
    sigc::connection                            m_tick;
    std::string                                 m_home;

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
