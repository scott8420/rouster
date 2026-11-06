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
      m_side("shell.side", Gtk::Orientation::VERTICAL),
      m_nav("shell.nav"),
      m_stack("shell.stack"),
      m_scan_page("shell.scan.page", Gtk::Orientation::VERTICAL),
      m_scan_states("shell.scan.states"),
      m_scan_sub("shell.scan.sub"),
      m_scan_button("shell.scan.scan"),
      m_spinner("shell.scan.spinner"),
      m_run_big("shell.scan.run_big"),
      m_run_caption("shell.scan.run_caption"),
      m_cancel_button("shell.scan.cancel"),
      m_res_big("shell.scan.res_big"),
      m_res_caption("shell.scan.res_caption"),
      m_res_where("shell.scan.res_where"),
      m_cards("shell.scan.cards"),
      m_detail("shell.scan.detail", Gtk::Orientation::VERTICAL, 4),
      m_pf_page("shell.preflight.page", Gtk::Orientation::VERTICAL, 10),
      m_pf_summary("shell.preflight.summary"),
      m_pf_rows("shell.preflight.rows", Gtk::Orientation::VERTICAL, 10),
      m_pf_button("shell.preflight.again"),
      m_act_page("shell.activity.page", Gtk::Orientation::VERTICAL, 10),
      m_act_undo("shell.activity.undo"),
      m_act_said("shell.activity.said"),
      m_act_list("shell.activity.list", Gtk::Orientation::VERTICAL, 10) {
    set_name("shell.window");
    add_css_class("rouster");
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
    build_sidebar();   // zone (after the pages: it selects the first one)
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
