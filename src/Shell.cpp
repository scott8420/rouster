#include "Shell.hpp"
#include <algorithm>
#include "core/Package.hpp"   // s017a2: bundles.json
#include "HelpWindow.hpp"      // complete type for ~unique_ptr<HelpWindow>
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
      m_pr_page("shell.profile.page", Gtk::Orientation::VERTICAL),
      m_pr_states("shell.profile.states"),
      m_pr_sub("shell.profile.sub"),
      m_pr_last("shell.profile.last"),
      m_pr_find("shell.profile.find"),
      m_pr_spinner("shell.profile.spinner"),
      m_pr_big("shell.profile.big"),
      m_pr_caption("shell.profile.caption"),
      m_pr_list("shell.profile.list", Gtk::Orientation::VERTICAL, 10),
      m_pr_go("shell.profile.go"),
      m_pr_gate("shell.profile.gate"),
      m_pr_done_big("shell.profile.done_big"),
      m_pr_done_caption("shell.profile.done_caption"),
      m_pr_backups("shell.profile.backups", Gtk::Orientation::VERTICAL, 6),
      m_pr_rs_title("shell.profile.rs_title"),
      m_pr_rs_sub("shell.profile.rs_sub"),
      m_pr_rs_list("shell.profile.rs_list", Gtk::Orientation::VERTICAL, 10),
      m_pr_rs_gate("shell.profile.rs_gate"),
      m_pr_rs_go("shell.profile.rs_go"),
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
Shell::~Shell() { stop_scan(); stop_sweep(); stop_dev(); stop_dups(); stop_profile(); stop_preflight(); stop_system(); }

void Shell::build_ui() {
    m_home = home_dir();
    core::door::install();   // latch: Rouster's name + its root helper's rules, before any door
    // s017a2: Samson's folder, second home and catalog are Protected -- before any walk or door step.
    // s019d: ...and Tidy's Hands off (~/.local/share/tidy/settings.ini) -- ONE list for both apps.
    {
        auto owned = core::scan::samson_places(m_home, config_home());
        const auto tidy = core::tidylink::hands_off(m_home);
        for (const auto& r : tidy) if (std::find(owned.begin(), owned.end(), r) == owned.end()) owned.push_back(r);
        if (auto lg = log::get(log::Area::Shell)) {
            lg->info("hands off (Tidy's): {}", tidy.size());
            for (const auto& r : tidy) lg->info("  ~/{}", r);
        }
        core::scan::set_owned(std::move(owned));
    }
    // s017a2: your own bundle list (bundles.json) joins the built-in package kinds.
    core::pkg::set_extra(core::pkg::load_extra(data_dir()));
    m_door = std::make_unique<core::door::Door>(m_home, data_dir());
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h)   // s012d: a fixture's settings are the fixture's own
        m_door->set_dconf(core::door::fixture_dconf(m_home));
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h)   // s013: a fixture asks root for a dry run only
        m_door->set_root([](const std::vector<std::string>& a) { return core::vcs::child(a, 15 * 60); }, true);
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

    signal_close_request().connect([this] { stop_scan(); stop_sweep(); stop_dev(); stop_dups(); stop_profile(); stop_preflight(); stop_system(); return false; }, false);
    m_scan_finished.connect(sigc::mem_fun(*this, &Shell::on_scan_done));
    m_sweep_finished.connect(sigc::mem_fun(*this, &Shell::on_sweep_found));
    m_dev_finished.connect(sigc::mem_fun(*this, &Shell::on_dev_found));
    m_dev_acted.connect(sigc::mem_fun(*this, &Shell::on_dev_acted));
    m_dups_finished.connect(sigc::mem_fun(*this, &Shell::on_dups_found));
    m_dups_acted.connect(sigc::mem_fun(*this, &Shell::on_dups_acted));
    m_snapshot_taken.connect(sigc::mem_fun(*this, &Shell::on_snapshot_taken));
    m_system_acted.connect(sigc::mem_fun(*this, &Shell::on_system_acted));
    m_profile_finished.connect(sigc::mem_fun(*this, &Shell::on_profile_found));
    m_profile_looked.connect(sigc::mem_fun(*this, &Shell::on_profile_looked));
    m_profile_packed.connect(sigc::mem_fun(*this, &Shell::on_profile_packed));
    m_profile_sec_opened.connect(sigc::mem_fun(*this, &Shell::on_profile_secrets_opened));
    paint_profile_start();
    m_known = core::load_known(data_dir() + "/known.json");

    if (auto lg = log::get(log::Area::Shell)) lg->info("ready -- home is {}", m_home);
}

}  // namespace rouster
