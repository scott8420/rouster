#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/scrolledwindow.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the window frame,
// the sidebar, each module page, and the menu model. The look is CSS
// (resources/rouster.css); here widgets only get their classes.

namespace rouster {

namespace {
Gtk::Label* label(const std::string& text, const char* css, float x = 0.5f) {
    auto l = Gtk::make_managed<Gtk::Label>(text);
    if (css) l->add_css_class(css);
    l->set_xalign(x);
    l->set_wrap(true);
    l->set_justify(x == 0.5f ? Gtk::Justification::CENTER : Gtk::Justification::LEFT);
    return l;
}
Gtk::Image* hero(const char* icon, int px) {
    auto i = Gtk::make_managed<Gtk::Image>();
    i->set_from_icon_name(icon);
    i->set_pixel_size(px);
    return i;
}
}  // namespace

void Shell::build_shell() {  // zone: window + header + body
    set_title("Rouster");
    set_default_size(980, 660);

    auto header = Gtk::make_managed<Gtk::HeaderBar>();
    m_logo_button.set_icon_name("rouster-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About Rouster");
    m_logo_button.set_action_name("win.about");
    header->pack_start(m_logo_button);
    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    header->pack_end(m_menu_button);
    set_titlebar(*header);

    m_side.add_css_class("sidebar-pane");
    m_side.set_size_request(220, -1);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);
    m_stack.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_stack.set_transition_duration(180);
    m_body.append(m_side);
    m_body.append(m_stack);
    set_child(m_body);
}

// The sidebar: brand, then sections of module rows. Section headers are rows
// that cannot be selected or activated, so the keyboard skips them.
void Shell::build_sidebar() {  // zone: brand + sections + module rows
    auto brand = label("Rouster", "sidebar-brand", 0.0f);
    m_side.append(*brand);

    m_nav.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_nav.set_vexpand(true);
    const auto section = [&](const char* title) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_selectable(false);
        row->set_activatable(false);
        row->set_can_focus(false);
        row->set_child(*label(title, "sidebar-section", 0.0f));
        row->set_css_classes({"section-row"});
        m_nav.append(*row);
    };
    const auto item = [&](const char* page, const char* icon, const char* title) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_name(std::string("nav.") + page);
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto img = Gtk::make_managed<Gtk::Image>();
        img->set_from_icon_name(icon);
        img->set_pixel_size(18);
        box->append(*img);
        box->append(*label(title, nullptr, 0.0f));
        row->set_child(*box);
        m_nav.append(*row);
    };
    section("CLEANUP");
    item("scan", "rouster-scan-symbolic", "Smart Scan");
    section("SAFETY");
    item("preflight", "rouster-preflight-symbolic", "Preflight");
    item("activity", "rouster-activity-symbolic", "Activity");

    m_nav.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        const std::string n = row->get_name();
        if (n.rfind("nav.", 0) == 0) on_nav(n.substr(4));
    });
    m_side.append(m_nav);
    if (auto* first = m_nav.get_row_at_index(1)) m_nav.select_row(*first);
}

void Shell::build_pages() {  // zone: the module pages
    build_scan_page();
    build_preflight_page();
    build_activity_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Smart Scan -- three states in one stack, CleanMyMac-shaped:
//   start   : the hero, one line of promise, the orb.
//   running : a big spinner and a big live number; Stop.
//   results : the headline number, one card per kind, the chosen card's
//             biggest folders. READ-ONLY: no act button until Sweep (s006).
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_scan_page() {  // zone: Smart Scan
    m_scan_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_scan_states.set_transition_duration(220);
    m_scan_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-scan", 128));
    start->append(*label("Smart Scan", "hero-title"));
    m_scan_sub.add_css_class("hero-sub");
    m_scan_sub.set_wrap(true);
    m_scan_sub.set_justify(Gtk::Justification::CENTER);
    m_scan_sub.set_max_width_chars(52);
    m_scan_sub.set_text("Sorts everything in your home folder into what can go and what must stay. "
                        "Looking changes nothing.");
    start->append(m_scan_sub);
    m_scan_button.set_label("Scan");
    m_scan_button.add_css_class("orb");
    m_scan_button.set_halign(Gtk::Align::CENTER);
    m_scan_button.set_margin_top(18);
    m_scan_button.set_action_name("win.scan");
    start->append(m_scan_button);
    m_scan_states.add(*start, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_spinner.add_css_class("big");
    m_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_spinner);
    m_run_big.add_css_class("hero-big");
    run->append(m_run_big);
    m_run_caption.add_css_class("hero-caption");
    run->append(m_run_caption);
    m_cancel_button.set_label("Stop");
    m_cancel_button.add_css_class("ghost");
    m_cancel_button.set_halign(Gtk::Align::CENTER);
    m_cancel_button.set_margin_top(12);
    m_cancel_button.set_action_name("win.scan-cancel");
    run->append(m_cancel_button);
    m_scan_states.add(*run, "running");

    // results
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(28);

    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-done", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_res_big.add_css_class("hero-big");
    m_res_big.set_xalign(0.0f);
    m_res_caption.add_css_class("hero-caption");
    m_res_caption.set_xalign(0.0f);
    m_res_caption.set_wrap(true);
    words->append(m_res_big);
    words->append(m_res_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Scan again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.scan");
    head->append(*again);
    res->append(*head);

    m_cards.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_cards.set_homogeneous(true);
    m_cards.set_min_children_per_line(2);
    m_cards.set_max_children_per_line(4);
    m_cards.set_column_spacing(12);
    m_cards.set_row_spacing(12);
    m_cards.signal_selected_children_changed().connect([this] {
        const auto sel = m_cards.get_selected_children();
        if (sel.empty()) return;
        const int k = std::stoi(sel.front()->get_name());
        if (k != m_sel_kind) { m_sel_kind = k; paint_detail(); }
    });
    res->append(m_cards);

    m_detail.add_css_class("detail");
    res->append(m_detail);
    m_res_where.add_css_class("dim");
    m_res_where.set_xalign(0.0f);
    m_res_where.set_wrap(true);
    m_res_where.set_ellipsize(Pango::EllipsizeMode::NONE);
    res->append(m_res_where);

    scroll->set_child(*res);
    m_scan_states.add(*scroll, "results");

    m_scan_page.append(m_scan_states);
    m_stack.add(m_scan_page, "scan", "Smart Scan");
}

// ─────────────────────────────────────────────────────────────────────────────
// Preflight (s003) -- the hero, the summary, one card per check.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_preflight_page() {  // zone: Preflight
    m_pf_page.set_margin_start(40);
    m_pf_page.set_margin_end(40);
    m_pf_page.set_margin_top(16);
    m_pf_page.set_margin_bottom(24);
    m_pf_page.append(*hero("rouster-hero-preflight", 96));
    m_pf_page.append(*label("Preflight", "hero-title"));
    m_pf_summary.add_css_class("hero-sub");
    m_pf_summary.set_wrap(true);
    m_pf_summary.set_justify(Gtk::Justification::CENTER);
    m_pf_page.append(m_pf_summary);
    m_pf_rows.set_margin_top(10);
    m_pf_page.append(m_pf_rows);
    m_pf_button.set_label("Check again");
    m_pf_button.add_css_class("ghost");
    m_pf_button.set_halign(Gtk::Align::CENTER);
    m_pf_button.set_action_name("win.preflight");
    m_pf_page.append(m_pf_button);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(m_pf_page);
    m_stack.add(*scroll, "preflight", "Preflight");
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity (s004) -- the manifest, readable; Undo takes the newest batch back.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_activity_page() {  // zone: Activity
    m_act_page.set_margin_start(40);
    m_act_page.set_margin_end(40);
    m_act_page.set_margin_top(16);
    m_act_page.set_margin_bottom(24);
    m_act_page.append(*hero("rouster-hero-activity", 96));
    m_act_page.append(*label("Activity", "hero-title"));
    auto intro = label("Everything Rouster has moved or put in the Trash, newest first. "
                       "Nothing is ever deleted, and any batch can be taken back.", "hero-sub");
    intro->set_max_width_chars(60);
    m_act_page.append(*intro);
    m_act_undo.set_label("Undo");
    m_act_undo.add_css_class("ghost");
    m_act_undo.set_halign(Gtk::Align::CENTER);
    m_act_undo.set_action_name("win.undo");
    m_act_page.append(m_act_undo);
    m_act_said.add_css_class("hero-caption");
    m_act_said.set_wrap(true);
    m_act_page.append(m_act_said);
    m_act_list.set_margin_top(6);
    m_act_page.append(m_act_list);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(m_act_page);
    m_stack.add(*scroll, "activity", "Activity");
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto scan = Gio::Menu::create();
    scan->append("Smart Scan", "win.scan");
    scan->append("Scan a folder…", "win.pick-folder");
    m_recents_menu = Gio::Menu::create();
    scan->append_submenu("Scan again", m_recents_menu);
    menu->append_section(scan);

    auto edit = Gio::Menu::create();
    edit->append("Undo", "win.undo");
    menu->append_section(edit);

    auto diag = Gio::Menu::create();
    diag->append("Keyboard shortcuts", "win.shortcuts");
    diag->append("Dump registry", "win.dump-registry");
    menu->append_section(diag);

    auto app = Gio::Menu::create();
    app->append("About Rouster", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace rouster
