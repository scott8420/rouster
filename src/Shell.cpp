#include "Shell.hpp"
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto
#include "Log.hpp"
#include "core/Known.hpp"

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
      m_sw_page("shell.sweep.page", Gtk::Orientation::VERTICAL),
      m_sw_states("shell.sweep.states"),
      m_sw_sub("shell.sweep.sub"),
      m_sw_find("shell.sweep.find"),
      m_sw_spinner("shell.sweep.spinner"),
      m_sw_run_big("shell.sweep.run_big"),
      m_sw_run_caption("shell.sweep.run_caption"),
      m_sw_cancel("shell.sweep.cancel"),
      m_sw_big("shell.sweep.big"),
      m_sw_caption("shell.sweep.caption"),
      m_sw_list("shell.sweep.list", Gtk::Orientation::VERTICAL, 8),
      m_sw_go("shell.sweep.go"),
      m_sw_gate("shell.sweep.gate"),
      m_sw_done_big("shell.sweep.done_big"),
      m_sw_done_caption("shell.sweep.done_caption"),
      m_dv_page("shell.dev.page", Gtk::Orientation::VERTICAL),
      m_dv_states("shell.dev.states"),
      m_dv_sub("shell.dev.sub"),
      m_dv_find("shell.dev.find"),
      m_dv_spinner("shell.dev.spinner"),
      m_dv_run_big("shell.dev.run_big"),
      m_dv_run_caption("shell.dev.run_caption"),
      m_dv_cancel("shell.dev.cancel"),
      m_dv_big("shell.dev.big"),
      m_dv_caption("shell.dev.caption"),
      m_dv_list("shell.dev.list", Gtk::Orientation::VERTICAL, 12),
      m_dv_go("shell.dev.go"),
      m_dv_gate("shell.dev.gate"),
      m_dv_done_big("shell.dev.done_big"),
      m_dv_done_caption("shell.dev.done_caption"),
      m_dv_cf_title("shell.dev.confirm_title"),
      m_dv_cf_sub("shell.dev.confirm_sub"),
      m_dv_cf_list("shell.dev.confirm_list", Gtk::Orientation::VERTICAL, 6),
      m_dv_cf_go("shell.dev.confirm_go"),
      m_du_page("shell.dups.page", Gtk::Orientation::VERTICAL),
      m_du_states("shell.dups.states"),
      m_du_sub("shell.dups.sub"),
      m_du_find("shell.dups.find"),
      m_du_spinner("shell.dups.spinner"),
      m_du_run_big("shell.dups.run_big"),
      m_du_run_caption("shell.dups.run_caption"),
      m_du_cancel("shell.dups.cancel"),
      m_du_big("shell.dups.big"),
      m_du_caption("shell.dups.caption"),
      m_du_list("shell.dups.list", Gtk::Orientation::VERTICAL, 10),
      m_du_go("shell.dups.go"),
      m_du_gate("shell.dups.gate"),
      m_du_done_big("shell.dups.done_big"),
      m_du_done_caption("shell.dups.done_caption"),
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
Shell::~Shell() { stop_scan(); stop_sweep(); stop_dev(); stop_dups(); }

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

    signal_close_request().connect([this] { stop_scan(); stop_sweep(); stop_dev(); stop_dups(); return false; }, false);
    m_scan_finished.connect(sigc::mem_fun(*this, &Shell::on_scan_done));
    m_sweep_finished.connect(sigc::mem_fun(*this, &Shell::on_sweep_found));
    m_dev_finished.connect(sigc::mem_fun(*this, &Shell::on_dev_found));
    m_dev_acted.connect(sigc::mem_fun(*this, &Shell::on_dev_acted));
    m_dups_finished.connect(sigc::mem_fun(*this, &Shell::on_dups_found));
    m_dups_acted.connect(sigc::mem_fun(*this, &Shell::on_dups_acted));
    m_known = core::load_known(data_dir() + "/known.json");

    if (auto lg = log::get(log::Area::Shell)) lg->info("ready -- home is {}", m_home);
}

}  // namespace rouster
