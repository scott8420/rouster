#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto
#include "Log.hpp"

// Shell.cpp -- GLUE. The ctor/dtor and the ordered build_ui() pass live here;
// every other method body lives in a category file (see the index in Shell.hpp).

namespace rouster {

Shell::Shell()
    : m_logo_button("shell.logo_button"),
      m_menu_button("shell.menu_button"),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_sidebar("shell.sidebar"),
      m_stack("shell.stack"),
      m_scan_page("shell.scan.page", Gtk::Orientation::VERTICAL, 10),
      m_scan_where("shell.scan.where"),
      m_scan_button("shell.scan.scan"),
      m_cancel_button("shell.scan.cancel"),
      m_spinner("shell.scan.spinner"),
      m_scan_status("shell.scan.status"),
      m_scan_scroll("shell.scan.scroll"),
      m_scan_results("shell.scan.results", Gtk::Orientation::VERTICAL, 6),
      m_pf_page("shell.preflight.page", Gtk::Orientation::VERTICAL, 10),
      m_pf_summary("shell.preflight.summary"),
      m_pf_rows("shell.preflight.rows", Gtk::Orientation::VERTICAL, 4),
      m_pf_button("shell.preflight.again"),
      m_act_page("shell.activity.page", Gtk::Orientation::VERTICAL, 10),
      m_act_undo("shell.activity.undo"),
      m_act_said("shell.activity.said"),
      m_act_list("shell.activity.list", Gtk::Orientation::VERTICAL, 4) {
    set_name("shell.window");
}

// The worker must be gone before the members it touches (m_report, the
// dispatcher). close-request does this too; the dtor is the backstop.
Shell::~Shell() { stop_scan(); }

void Shell::build_ui() {
    m_home = home_dir();
    m_door = std::make_unique<core::door::Door>(m_home, data_dir());
    m_door->load();
    build_shell();     // zone
    build_pages();     // zone
    m_recents_file = recents_file();
    bind_actions();    // bindings (loads recents)
    rebuild_recents_menu();
    set_scanning(false);
    paint_preflight();
    paint_activity();

    signal_close_request().connect([this] { stop_scan(); return false; }, false);
    m_scan_finished.connect(sigc::mem_fun(*this, &Shell::on_scan_done));

    if (auto lg = log::get(log::Area::Shell)) lg->info("ready -- home is {}", m_home);
}

}  // namespace rouster
