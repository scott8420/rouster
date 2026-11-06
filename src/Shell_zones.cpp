#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/droptarget.h>
#include <gdk/gdk.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/textview.h>

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
    item("sweep", "rouster-sweep-symbolic", "Sweep");
    item("dups", "rouster-dups-symbolic", "Duplicates");
    item("dev", "rouster-dev-symbolic", "Dev projects");
    section("ORGANIZE");
    item("projects", "rouster-projects-symbolic", "Projects");
    item("rules", "rouster-rules-symbolic", "Rules");
    item("archive", "rouster-archive-symbolic", "Archive");
    section("SAFETY");
    item("preflight", "rouster-preflight-symbolic", "Preflight");
    item("profile", "rouster-profile-symbolic", "Profile backup");
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
    build_sweep_page();
    build_dups_page();
    build_archive_page();
    build_projects_page();
    build_rules_page();
    build_dev_page();
    build_profile_page();
    build_preflight_page();
    build_activity_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Smart Scan -- three states in one stack, CleanMyMac-shaped:
//   start   : the hero, one line of promise, the orb.
//   running : a big spinner and a big live number; Stop.
//   results : the headline number, one card per kind, the chosen card's
//             biggest folders. READ-ONLY: acting is Sweep's page (s006),
//             one click away ("Sweep...").
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
    auto to_sweep = Gtk::make_managed<Gtk::Button>("Sweep…");
    to_sweep->add_css_class("ghost");
    to_sweep->set_valign(Gtk::Align::CENTER);
    to_sweep->set_tooltip_text("Choose what to clear, on the Sweep page");
    to_sweep->set_action_name("win.go-sweep");
    head->append(*to_sweep);
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
// Sweep (s006) -- the first page that acts. Four states:
//   start   : hero, one line, the orb ("Look").
//   running : spinner + live counters; Stop.
//   results : the checked total, then each group with its rows (a check, the
//             size, the path); the Trash measured at the foot; the act button.
//             This list IS the preview (RULES: preview first).
//   done    : what went to the Trash, Undo, and the way to Activity.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_sweep_page() {  // zone: Sweep
    m_sw_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_sw_states.set_transition_duration(220);
    m_sw_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-sweep", 128));
    start->append(*label("Sweep", "hero-title"));
    m_sw_sub.add_css_class("hero-sub");
    m_sw_sub.set_wrap(true);
    m_sw_sub.set_justify(Gtk::Justification::CENTER);
    m_sw_sub.set_max_width_chars(52);
    m_sw_sub.set_text("Caches, thumbnails and old logs -- things your apps rebuild on their own. "
                      "You choose; they go to the Trash, and Undo brings them back.");
    start->append(m_sw_sub);
    m_sw_find.set_label("Look");
    m_sw_find.add_css_class("orb");
    m_sw_find.set_halign(Gtk::Align::CENTER);
    m_sw_find.set_margin_top(18);
    m_sw_find.set_action_name("win.sweep-find");
    start->append(m_sw_find);
    m_sw_states.add(*start, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_sw_spinner.add_css_class("big");
    m_sw_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_sw_spinner);
    m_sw_run_big.add_css_class("hero-big");
    run->append(m_sw_run_big);
    m_sw_run_caption.add_css_class("hero-caption");
    run->append(m_sw_run_caption);
    m_sw_cancel.set_label("Stop");
    m_sw_cancel.add_css_class("ghost");
    m_sw_cancel.set_halign(Gtk::Align::CENTER);
    m_sw_cancel.set_margin_top(12);
    m_sw_cancel.set_action_name("win.sweep-cancel");
    run->append(m_sw_cancel);
    m_sw_states.add(*run, "running");

    // results
    auto res_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(16);
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-sweep", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_sw_big.add_css_class("hero-big");
    m_sw_big.set_xalign(0.0f);
    m_sw_caption.add_css_class("hero-caption");
    m_sw_caption.set_xalign(0.0f);
    m_sw_caption.set_wrap(true);
    words->append(m_sw_big);
    words->append(m_sw_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Look again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.sweep-find");
    head->append(*again);
    res->append(*head);
    res->append(m_sw_list);
    scroll->set_child(*res);
    res_page->append(*scroll);

    // The act bar stays put under the list.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    bar->add_css_class("act-bar");
    m_sw_gate.add_css_class("dim");
    m_sw_gate.set_xalign(0.0f);
    m_sw_gate.set_hexpand(true);
    m_sw_gate.set_wrap(true);
    bar->append(m_sw_gate);
    m_sw_go.add_css_class("act");
    m_sw_go.set_action_name("win.sweep-run");
    bar->append(m_sw_go);
    res_page->append(*bar);
    m_sw_states.add(*res_page, "results");

    // done
    auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    done->set_valign(Gtk::Align::CENTER);
    done->set_halign(Gtk::Align::CENTER);
    done->set_margin(40);
    done->append(*hero("rouster-hero-done", 112));
    m_sw_done_big.add_css_class("hero-big");
    done->append(m_sw_done_big);
    m_sw_done_caption.add_css_class("hero-sub");
    m_sw_done_caption.set_wrap(true);
    m_sw_done_caption.set_justify(Gtk::Justification::CENTER);
    m_sw_done_caption.set_max_width_chars(58);
    done->append(m_sw_done_caption);
    auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    btns->set_halign(Gtk::Align::CENTER);
    btns->set_margin_top(12);
    auto undo = Gtk::make_managed<Gtk::Button>("Undo");
    undo->add_css_class("ghost");
    undo->set_action_name("win.undo");
    auto act = Gtk::make_managed<Gtk::Button>("Activity");
    act->add_css_class("ghost");
    act->signal_clicked().connect([this] { go_to("activity"); });
    auto more = Gtk::make_managed<Gtk::Button>("Look again");
    more->add_css_class("ghost");
    more->set_action_name("win.sweep-find");
    btns->append(*undo);
    btns->append(*act);
    btns->append(*more);
    done->append(*btns);
    m_sw_states.add(*done, "done");

    m_sw_page.append(m_sw_states);
    m_stack.add(m_sw_page, "sweep", "Sweep");
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicates (s008) -- the Sweep shape. Four states:
//   start   : hero, one line, the orb ("Look").
//   running : spinner, the stage (walk / ends / whole / folders), bytes read; Stop.
//   results : what the checked copies would free; the mode switch (Share
//             blocks / Trash a copy); one card per group: the keeper (and
//             why), each copy with a check and "Keep this". THE PREVIEW.
//   done    : what was shared or trashed, Undo, Activity.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_dups_page() {  // zone: Duplicates
    m_du_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_du_states.set_transition_duration(220);
    m_du_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-dups", 128));
    start->append(*label("Duplicates", "hero-title"));
    m_du_sub.add_css_class("hero-sub");
    m_du_sub.set_wrap(true);
    m_du_sub.set_justify(Gtk::Justification::CENTER);
    m_du_sub.set_max_width_chars(52);
    m_du_sub.set_text("Files and whole folders that are copies of each other -- the double-dumped backup, "
                      "the download saved twice. On btrfs the copies share their blocks and every path stays put; "
                      "or send a copy to the Trash. Nothing is chosen for you that can't be undone.");
    start->append(m_du_sub);
    m_du_find.set_label("Look");
    m_du_find.add_css_class("orb");
    m_du_find.set_halign(Gtk::Align::CENTER);
    m_du_find.set_margin_top(18);
    m_du_find.set_action_name("win.dups-find");
    start->append(m_du_find);
    m_du_states.add(*start, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_du_spinner.add_css_class("big");
    m_du_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_du_spinner);
    m_du_run_big.add_css_class("hero-big");
    run->append(m_du_run_big);
    m_du_run_caption.add_css_class("hero-caption");
    run->append(m_du_run_caption);
    m_du_cancel.set_label("Stop");
    m_du_cancel.add_css_class("ghost");
    m_du_cancel.set_halign(Gtk::Align::CENTER);
    m_du_cancel.set_margin_top(12);
    m_du_cancel.set_action_name("win.dups-cancel");
    run->append(m_du_cancel);
    m_du_states.add(*run, "running");

    // results
    auto res_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(16);
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-dups", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_du_big.add_css_class("hero-big");
    m_du_big.set_xalign(0.0f);
    m_du_caption.add_css_class("hero-caption");
    m_du_caption.set_xalign(0.0f);
    m_du_caption.set_wrap(true);
    words->append(m_du_big);
    words->append(m_du_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Look again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.dups-find");
    head->append(*again);
    res->append(*head);
    res->append(m_du_list);
    scroll->set_child(*res);
    res_page->append(*scroll);

    // The act bar stays put under the list.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    bar->add_css_class("act-bar");
    m_du_gate.add_css_class("dim");
    m_du_gate.set_xalign(0.0f);
    m_du_gate.set_hexpand(true);
    m_du_gate.set_wrap(true);
    bar->append(m_du_gate);
    m_du_go.add_css_class("act");
    m_du_go.set_action_name("win.dups-run");
    bar->append(m_du_go);
    res_page->append(*bar);
    m_du_states.add(*res_page, "results");

    // done
    auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    done->set_valign(Gtk::Align::CENTER);
    done->set_halign(Gtk::Align::CENTER);
    done->set_margin(40);
    done->append(*hero("rouster-hero-done", 112));
    m_du_done_big.add_css_class("hero-big");
    done->append(m_du_done_big);
    m_du_done_caption.add_css_class("hero-sub");
    m_du_done_caption.set_wrap(true);
    m_du_done_caption.set_justify(Gtk::Justification::CENTER);
    m_du_done_caption.set_max_width_chars(58);
    done->append(m_du_done_caption);
    auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    btns->set_halign(Gtk::Align::CENTER);
    btns->set_margin_top(12);
    auto undo = Gtk::make_managed<Gtk::Button>("Undo");
    undo->add_css_class("ghost");
    undo->set_action_name("win.undo");
    auto act = Gtk::make_managed<Gtk::Button>("Activity");
    act->add_css_class("ghost");
    act->signal_clicked().connect([this] { go_to("activity"); });
    auto more = Gtk::make_managed<Gtk::Button>("Look again");
    more->add_css_class("ghost");
    more->set_action_name("win.dups-find");
    btns->append(*undo);
    btns->append(*act);
    btns->append(*more);
    done->append(*btns);
    m_du_states.add(*done, "done");

    m_du_page.append(m_du_states);
    m_stack.add(m_du_page, "dups", "Duplicates");
}

// ─────────────────────────────────────────────────────────────────────────────
// Archive (s009) -- the Sweep shape. Four states:
//   start   : hero, one line, the orb ("Look").
//   running : spinner, files looked at; Stop.
//   results : two views -- Old files (cards per top folder, a check per row,
//             where each would go) and In the archive (from the manifest,
//             a check per row to put back). THE PREVIEW either way.
//   done    : what moved, Undo, Activity.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_archive_page() {  // zone: Archive
    m_ar_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_ar_states.set_transition_duration(220);
    m_ar_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-archive", 128));
    start->append(*label("Archive", "hero-title"));
    m_ar_sub.add_css_class("hero-sub");
    m_ar_sub.set_wrap(true);
    m_ar_sub.set_justify(Gtk::Justification::CENTER);
    m_ar_sub.set_max_width_chars(52);
    m_ar_sub.set_text("Files you haven't touched in a year, moved out of the way into ~/Archive/<year>/, "
                      "with their folders kept. Nothing is chosen for you; anything archived can go back "
                      "where it came from, any time.");
    start->append(m_ar_sub);
    m_ar_find.set_label("Look");
    m_ar_find.add_css_class("orb");
    m_ar_find.set_halign(Gtk::Align::CENTER);
    m_ar_find.set_margin_top(18);
    m_ar_find.set_action_name("win.archive-find");
    start->append(m_ar_find);
    m_ar_states.add(*start, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_ar_spinner.add_css_class("big");
    m_ar_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_ar_spinner);
    m_ar_run_big.add_css_class("hero-big");
    run->append(m_ar_run_big);
    m_ar_run_caption.add_css_class("hero-caption");
    run->append(m_ar_run_caption);
    m_ar_cancel.set_label("Stop");
    m_ar_cancel.add_css_class("ghost");
    m_ar_cancel.set_halign(Gtk::Align::CENTER);
    m_ar_cancel.set_margin_top(12);
    m_ar_cancel.set_action_name("win.archive-cancel");
    run->append(m_ar_cancel);
    m_ar_states.add(*run, "running");

    // results
    auto res_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(16);
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-archive", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_ar_big.add_css_class("hero-big");
    m_ar_big.set_xalign(0.0f);
    m_ar_caption.add_css_class("hero-caption");
    m_ar_caption.set_xalign(0.0f);
    m_ar_caption.set_wrap(true);
    words->append(m_ar_big);
    words->append(m_ar_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Look again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.archive-find");
    head->append(*again);
    res->append(*head);
    res->append(m_ar_list);
    scroll->set_child(*res);
    res_page->append(*scroll);

    // The act bar stays put under the list.
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    bar->add_css_class("act-bar");
    m_ar_gate.add_css_class("dim");
    m_ar_gate.set_xalign(0.0f);
    m_ar_gate.set_hexpand(true);
    m_ar_gate.set_wrap(true);
    bar->append(m_ar_gate);
    m_ar_go.add_css_class("act");
    m_ar_go.set_action_name("win.archive-run");
    bar->append(m_ar_go);
    res_page->append(*bar);
    m_ar_states.add(*res_page, "results");

    // done
    auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    done->set_valign(Gtk::Align::CENTER);
    done->set_halign(Gtk::Align::CENTER);
    done->set_margin(40);
    done->append(*hero("rouster-hero-done", 112));
    m_ar_done_big.add_css_class("hero-big");
    done->append(m_ar_done_big);
    m_ar_done_caption.add_css_class("hero-sub");
    m_ar_done_caption.set_wrap(true);
    m_ar_done_caption.set_justify(Gtk::Justification::CENTER);
    m_ar_done_caption.set_max_width_chars(58);
    done->append(m_ar_done_caption);
    auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    btns->set_halign(Gtk::Align::CENTER);
    btns->set_margin_top(12);
    auto undo = Gtk::make_managed<Gtk::Button>("Undo");
    undo->add_css_class("ghost");
    undo->set_action_name("win.undo");
    auto act = Gtk::make_managed<Gtk::Button>("Activity");
    act->add_css_class("ghost");
    act->signal_clicked().connect([this] { go_to("activity"); });
    auto more = Gtk::make_managed<Gtk::Button>("Look again");
    more->add_css_class("ghost");
    more->set_action_name("win.archive-find");
    btns->append(*undo);
    btns->append(*act);
    btns->append(*more);
    done->append(*btns);
    m_ar_states.add(*done, "done");

    m_ar_page.append(m_ar_states);
    m_stack.add(m_ar_page, "archive", "Archive");
}

// ─────────────────────────────────────────────────────────────────────────────
// Profile backup (s012a, §4.8) -- the Sweep shape. Four states:
//   start   : hero, one line, the orb ("Look"), the newest earlier backup.
//   running : spinner; Look asks dconf, dnf (cache only) and flatpak.
//   results : cards per group with a check per row, what couldn't be
//             gathered, the notes box. THE PREVIEW. The act bar makes one new
//             folder under ~/Archive/Profile -- it changes nothing else.
//   done    : where it went; Open folder / Undo / Activity / Look again.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_profile_page() {  // zone: Profile backup
    m_pr_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_pr_states.set_transition_duration(220);
    m_pr_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-profile", 128));
    start->append(*label("Profile backup", "hero-title"));
    m_pr_sub.add_css_class("hero-sub");
    m_pr_sub.set_wrap(true);
    m_pr_sub.set_justify(Gtk::Justification::CENTER);
    m_pr_sub.set_max_width_chars(52);
    m_pr_sub.set_text("How this machine is set up -- dotfiles, GNOME settings, extensions, autostart, user "
                      "services, fonts, and what you installed -- gathered into one folder in "
                      "~/Archive/Profile. Your own files aren't in it, and nothing else changes.");
    start->append(m_pr_sub);
    m_pr_last.add_css_class("dim");
    start->append(m_pr_last);
    m_pr_find.set_label("Look");
    m_pr_find.add_css_class("orb");
    m_pr_find.set_halign(Gtk::Align::CENTER);
    m_pr_find.set_margin_top(18);
    m_pr_find.set_action_name("win.profile-find");
    start->append(m_pr_find);
    m_pr_backups.set_margin_top(18);
    m_pr_backups.set_size_request(520, -1);
    start->append(m_pr_backups);
    auto start_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    start_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    start_scroll->set_child(*start);
    m_pr_states.add(*start_scroll, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_pr_spinner.add_css_class("big");
    m_pr_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_pr_spinner);
    run->append(*label("Gathering", "hero-big"));
    run->append(*label("dotfiles, dconf, extensions, services, fonts, packages", "hero-caption"));
    m_pr_states.add(*run, "running");

    // results
    auto res_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(16);
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-profile", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_pr_big.add_css_class("hero-big");
    m_pr_big.set_xalign(0.0f);
    m_pr_caption.add_css_class("hero-caption");
    m_pr_caption.set_xalign(0.0f);
    m_pr_caption.set_wrap(true);
    words->append(m_pr_big);
    words->append(m_pr_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Look again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.profile-find");
    head->append(*again);
    res->append(*head);
    res->append(m_pr_list);

    // Notes: what can't be packaged. Kept between backups (Rouster's own file).
    auto notes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    notes->add_css_class("card");
    notes->append(*label("Notes", "card-title", 0.0f));
    notes->append(*label("For what can't be packaged -- \"custom Nautilus in /usr/local: rebuild from "
                         "~/Develop/nautilus at tag 50.2.2\". Saved with each backup as NOTES.md.", "dim", 0.0f));
    m_pr_notes = Gtk::make_managed<Gtk::TextView>();
    m_pr_notes->set_name("shell.profile.notes");
    m_pr_notes->add_css_class("pr-notes");
    m_pr_notes->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_pr_notes->set_size_request(-1, 90);
    m_pr_notes->set_top_margin(8);
    m_pr_notes->set_bottom_margin(8);
    m_pr_notes->set_left_margin(10);
    m_pr_notes->set_right_margin(10);
    notes->append(*m_pr_notes);
    res->append(*notes);
    scroll->set_child(*res);
    res_page->append(*scroll);

    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    bar->add_css_class("act-bar");
    m_pr_gate.add_css_class("dim");
    m_pr_gate.set_xalign(0.0f);
    m_pr_gate.set_hexpand(true);
    m_pr_gate.set_wrap(true);
    bar->append(m_pr_gate);
    m_pr_go.add_css_class("act");
    m_pr_go.set_action_name("win.profile-run");
    bar->append(m_pr_go);
    res_page->append(*bar);
    m_pr_states.add(*res_page, "results");

    // done
    auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    done->set_valign(Gtk::Align::CENTER);
    done->set_halign(Gtk::Align::CENTER);
    done->set_margin(40);
    done->append(*hero("rouster-hero-done", 112));
    m_pr_done_big.add_css_class("hero-big");
    done->append(m_pr_done_big);
    m_pr_done_caption.add_css_class("hero-sub");
    m_pr_done_caption.set_wrap(true);
    m_pr_done_caption.set_justify(Gtk::Justification::CENTER);
    m_pr_done_caption.set_max_width_chars(58);
    done->append(m_pr_done_caption);
    auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    btns->set_halign(Gtk::Align::CENTER);
    btns->set_margin_top(12);
    auto open = Gtk::make_managed<Gtk::Button>("Open folder");
    open->add_css_class("ghost");
    open->signal_clicked().connect([this] { on_profile_open(); });
    auto undo = Gtk::make_managed<Gtk::Button>("Undo");
    undo->add_css_class("ghost");
    undo->set_action_name("win.undo");
    auto act = Gtk::make_managed<Gtk::Button>("Activity");
    act->add_css_class("ghost");
    act->signal_clicked().connect([this] { go_to("activity"); });
    auto more = Gtk::make_managed<Gtk::Button>("Look again");
    more->add_css_class("ghost");
    more->set_action_name("win.profile-find");
    auto list = Gtk::make_managed<Gtk::Button>("Your backups");
    list->add_css_class("ghost");
    list->signal_clicked().connect([this] { paint_profile_start(); m_pr_states.set_visible_child("start"); });
    for (auto* b : {open, undo, act, more, list}) btns->append(*b);
    done->append(*btns);
    m_pr_states.add(*done, "done");

    // restore (s012b): the look back at one backup -- THE PREVIEW of a restore.
    auto rs_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto rs_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    rs_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    rs_scroll->set_vexpand(true);
    auto rs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    rs->set_margin_start(32);
    rs->set_margin_end(32);
    rs->set_margin_top(12);
    rs->set_margin_bottom(16);
    auto rs_head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    rs_head->append(*hero("rouster-hero-profile", 88));
    auto rs_words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    rs_words->set_valign(Gtk::Align::CENTER);
    rs_words->set_hexpand(true);
    m_pr_rs_title.add_css_class("hero-big");
    m_pr_rs_title.set_xalign(0.0f);
    m_pr_rs_sub.add_css_class("hero-caption");
    m_pr_rs_sub.set_xalign(0.0f);
    m_pr_rs_sub.set_wrap(true);
    rs_words->append(m_pr_rs_title);
    rs_words->append(m_pr_rs_sub);
    rs_head->append(*rs_words);
    auto rs_back = Gtk::make_managed<Gtk::Button>("Back");
    rs_back->add_css_class("ghost");
    rs_back->set_valign(Gtk::Align::CENTER);
    rs_back->signal_clicked().connect([this] { paint_profile_start(); m_pr_states.set_visible_child("start"); });
    rs_head->append(*rs_back);
    rs->append(*rs_head);
    rs->append(m_pr_rs_list);
    rs_scroll->set_child(*rs);
    rs_page->append(*rs_scroll);
    auto rs_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    rs_bar->add_css_class("act-bar");
    m_pr_rs_gate.add_css_class("dim");
    m_pr_rs_gate.set_xalign(0.0f);
    m_pr_rs_gate.set_hexpand(true);
    m_pr_rs_gate.set_wrap(true);
    rs_bar->append(m_pr_rs_gate);
    m_pr_rs_go.add_css_class("act");
    m_pr_rs_go.signal_clicked().connect([this] { on_profile_restore(); });
    rs_bar->append(m_pr_rs_go);
    rs_page->append(*rs_bar);
    m_pr_states.add(*rs_page, "restore");

    m_pr_page.append(m_pr_states);
    m_stack.add(m_pr_page, "profile", "Profile backup");
}

// ─────────────────────────────────────────────────────────────────────────────
// Projects (s010, §4.7) -- organize first. Five states:
//   home    : templates (each with New project...) and the projects found in
//             their roots; Install starter templates...; Open templates folder.
//   new     : id + title; the plan below them IS the preview, live as you type.
//   preview : any other plan, step by step (starter templates, New version).
//   project : the open project -- drop files (or Add files...) and the queue
//             shows where each would go; then what's in it, misfits flagged,
//             New version on the files that fit.
//   done    : what happened; Undo / Activity / Back.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_projects_page() {  // zone: Projects
    m_pj_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_pj_states.set_transition_duration(220);
    m_pj_states.set_vexpand(true);

    const auto scrolled = [](Gtk::Widget& child) {
        auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll->set_vexpand(true);
        child.set_margin_start(32);
        child.set_margin_end(32);
        child.set_margin_top(12);
        child.set_margin_bottom(16);
        scroll->set_child(child);
        return scroll;
    };
    const auto act_bar = [](Gtk::Label& gate, Gtk::Button& go) {
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
        bar->add_css_class("act-bar");
        gate.add_css_class("dim");
        gate.set_xalign(0.0f);
        gate.set_hexpand(true);
        gate.set_wrap(true);
        bar->append(gate);
        go.add_css_class("act");
        bar->append(go);
        return bar;
    };
    const auto back_button = [this](const char* to) {
        auto b = Gtk::make_managed<Gtk::Button>("Back");
        b->add_css_class("ghost");
        b->set_valign(Gtk::Align::CENTER);
        const std::string where = to;
        b->signal_clicked().connect([this, where] {
            if (where == "home") on_pj_home();
            else { paint_pj_project(); m_pj_states.set_visible_child(where); }
        });
        return b;
    };

    // home (painted by paint_projects)
    m_pj_states.add(*scrolled(m_pj_home), "home");

    // new
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        head->append(*hero("rouster-hero-projects", 72));
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_valign(Gtk::Align::CENTER);
        words->set_hexpand(true);
        m_pj_new_title.add_css_class("hero-big");
        m_pj_new_title.set_xalign(0.0f);
        words->append(m_pj_new_title);
        m_pj_new_where.add_css_class("hero-caption");
        m_pj_new_where.set_xalign(0.0f);
        m_pj_new_where.set_wrap(true);
        words->append(m_pj_new_where);
        head->append(*words);
        head->append(*back_button("home"));
        body->append(*head);
        auto form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        m_pj_id.set_placeholder_text("Id (job number, app name)");
        m_pj_id.set_width_chars(16);
        m_pj_title.set_placeholder_text("Title");
        m_pj_title.set_hexpand(true);
        m_pj_id.signal_changed().connect([this] { on_pj_form(); });
        m_pj_title.signal_changed().connect([this] { on_pj_form(); });
        m_pj_title.signal_activate().connect([this] { if (m_pj_new_go.get_sensitive()) on_pj_create(); });
        form->append(m_pj_id);
        form->append(m_pj_title);
        body->append(*form);
        body->append(m_pj_new_list);
        page->append(*scrolled(*body));
        m_pj_new_go.signal_clicked().connect([this] { on_pj_create(); });
        page->append(*act_bar(m_pj_new_gate, m_pj_new_go));
        m_pj_states.add(*page, "new");
    }

    // preview
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        head->append(*hero("rouster-hero-projects", 72));
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_valign(Gtk::Align::CENTER);
        words->set_hexpand(true);
        m_pj_pv_title.add_css_class("hero-big");
        m_pj_pv_title.set_xalign(0.0f);
        m_pj_pv_title.set_wrap(true);
        words->append(m_pj_pv_title);
        m_pj_pv_sub.add_css_class("hero-caption");
        m_pj_pv_sub.set_xalign(0.0f);
        m_pj_pv_sub.set_wrap(true);
        words->append(m_pj_pv_sub);
        head->append(*words);
        auto back = Gtk::make_managed<Gtk::Button>("Back");
        back->add_css_class("ghost");
        back->set_valign(Gtk::Align::CENTER);
        back->signal_clicked().connect([this] {
            if (m_pj_has_open && m_pj_back == "project") { paint_pj_project(); m_pj_states.set_visible_child("project"); }
            else on_pj_home();
        });
        head->append(*back);
        body->append(*head);
        body->append(m_pj_pv_list);
        page->append(*scrolled(*body));
        m_pj_pv_go.signal_clicked().connect([this] { on_pj_confirm(); });
        page->append(*act_bar(m_pj_pv_gate, m_pj_pv_go));
        m_pj_states.add(*page, "preview");
    }

    // project
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        head->append(*hero("rouster-hero-projects", 72));
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_valign(Gtk::Align::CENTER);
        words->set_hexpand(true);
        m_pj_head.add_css_class("hero-big");
        m_pj_head.set_xalign(0.0f);
        m_pj_head.set_wrap(true);
        words->append(m_pj_head);
        m_pj_head_sub.add_css_class("hero-caption");
        m_pj_head_sub.set_xalign(0.0f);
        m_pj_head_sub.set_wrap(true);
        words->append(m_pj_head_sub);
        head->append(*words);
        auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        btns->set_valign(Gtk::Align::CENTER);
        auto add = Gtk::make_managed<Gtk::Button>("Add files...");
        add->add_css_class("ghost");
        add->signal_clicked().connect([this] { on_pj_add_files(); });
        auto show = Gtk::make_managed<Gtk::Button>("Open in Files");
        show->add_css_class("ghost");
        show->signal_clicked().connect([this] { if (m_pj_has_open) on_pj_show(m_pj_open.path); });
        btns->append(*add);
        btns->append(*show);
        btns->append(*back_button("home"));
        head->append(*btns);
        body->append(*head);
        body->append(m_pj_list);
        page->append(*scrolled(*body));
        m_pj_go.signal_clicked().connect([this] { on_pj_file(); });
        page->append(*act_bar(m_pj_gate, m_pj_go));
        // Drop files anywhere on the project page.
        auto drop = Gtk::DropTarget::create(GDK_TYPE_FILE_LIST, Gdk::DragAction::COPY);
        drop->signal_enter().connect([page](double, double) { page->add_css_class("pj-over"); return Gdk::DragAction::COPY; }, false);
        drop->signal_leave().connect([page] { page->remove_css_class("pj-over"); });
        drop->signal_drop().connect([this, page](const Glib::ValueBase& value, double, double) {
            page->remove_css_class("pj-over");
            if (!G_VALUE_HOLDS(value.gobj(), GDK_TYPE_FILE_LIST)) return false;
            auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value.gobj()));
            std::vector<std::string> paths;
            for (GSList* l = list ? gdk_file_list_get_files(list) : nullptr, *h = l; l; l = l->next) {
                (void)h;
                if (char* p = g_file_get_path(G_FILE(l->data))) { paths.emplace_back(p); g_free(p); }
            }
            on_pj_dropped(paths);
            return true;
        }, false);
        page->add_controller(drop);
        m_pj_states.add(*page, "project");
    }

    // done
    {
        auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        done->set_valign(Gtk::Align::CENTER);
        done->set_halign(Gtk::Align::CENTER);
        done->set_margin(40);
        done->append(*hero("rouster-hero-done", 112));
        m_pj_done_big.add_css_class("hero-big");
        m_pj_done_big.set_wrap(true);
        m_pj_done_big.set_justify(Gtk::Justification::CENTER);
        done->append(m_pj_done_big);
        m_pj_done_caption.add_css_class("hero-sub");
        m_pj_done_caption.set_wrap(true);
        m_pj_done_caption.set_justify(Gtk::Justification::CENTER);
        m_pj_done_caption.set_max_width_chars(60);
        done->append(m_pj_done_caption);
        auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        btns->set_halign(Gtk::Align::CENTER);
        btns->set_margin_top(12);
        auto undo = Gtk::make_managed<Gtk::Button>("Undo");
        undo->add_css_class("ghost");
        undo->set_action_name("win.undo");
        auto act = Gtk::make_managed<Gtk::Button>("Activity");
        act->add_css_class("ghost");
        act->signal_clicked().connect([this] { go_to("activity"); });
        m_pj_done_back.set_label("Back");
        m_pj_done_back.add_css_class("ghost");
        m_pj_done_back.signal_clicked().connect([this] {
            if (m_pj_back == "project" && m_pj_has_open) on_pj_open(m_pj_open.path);
            else on_pj_home();
        });
        btns->append(*undo);
        btns->append(*act);
        btns->append(m_pj_done_back);
        done->append(*btns);
        m_pj_states.add(*done, "done");
    }

    m_pj_page.append(m_pj_states);
    m_stack.add(m_pj_page, "projects", "Projects");
}

// ─────────────────────────────────────────────────────────────────────────────
// Rules (s011, §4.7) -- Hazel-shaped. Four states:
//   list : the rules in order (first match wins), each with its switch, Edit,
//          up / down; New rule, Add starter rules, Watch while open, and
//          Run rules now.
//   edit : the form; the draft's matches below it ARE the preview -- a rule
//          is turned on from here, after its matches were shown.
//   run  : every enabled rule's matches, checked; the act bar.
//   done : what happened; Undo / Activity / Back.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_rules_page() {  // zone: Rules
    m_rl_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_rl_states.set_transition_duration(220);
    m_rl_states.set_vexpand(true);
    const auto scrolled = [](Gtk::Widget& child) {
        auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll->set_vexpand(true);
        child.set_margin_start(32);
        child.set_margin_end(32);
        child.set_margin_top(12);
        child.set_margin_bottom(16);
        scroll->set_child(child);
        return scroll;
    };
    const auto bar_with = [](Gtk::Label& gate) {
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        bar->add_css_class("act-bar");
        gate.add_css_class("dim");
        gate.set_xalign(0.0f);
        gate.set_hexpand(true);
        gate.set_wrap(true);
        bar->append(gate);
        return bar;
    };
    const auto back = [this] {
        auto b = Gtk::make_managed<Gtk::Button>("Back");
        b->add_css_class("ghost");
        b->set_valign(Gtk::Align::CENTER);
        b->signal_clicked().connect([this] { paint_rules(); m_rl_states.set_visible_child("list"); });
        return b;
    };

    m_rl_states.add(*scrolled(m_rl_list), "list");

    // edit
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        head->append(*hero("rouster-hero-rules", 72));
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_valign(Gtk::Align::CENTER);
        words->set_hexpand(true);
        m_rl_edit_title.add_css_class("hero-big");
        m_rl_edit_title.set_xalign(0.0f);
        words->append(m_rl_edit_title);
        m_rl_summary.add_css_class("hero-caption");
        m_rl_summary.set_xalign(0.0f);
        m_rl_summary.set_wrap(true);
        words->append(m_rl_summary);
        head->append(*words);
        head->append(*back());
        body->append(*head);

        auto grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        grid->add_css_class("card");
        const auto field = [&](const char* label, Gtk::Widget& w, const char* hint) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            auto l = Gtk::make_managed<Gtk::Label>(label);
            l->add_css_class("card-sub");
            l->set_xalign(1.0f);
            l->set_width_chars(14);
            row->append(*l);
            w.set_hexpand(true);
            row->append(w);
            if (hint) {
                auto h = Gtk::make_managed<Gtk::Label>(hint);
                h->add_css_class("dim");
                h->set_width_chars(26);
                h->set_xalign(0.0f);
                h->set_wrap(true);
                row->append(*h);
            }
            grid->append(*row);
        };
        field("Name", m_rl_name, nullptr);
        field("Watch ~/", m_rl_folder, "a folder under home; its top only");
        field("Name matches", m_rl_glob, "* and ?, any case; empty = any");
        field("Kinds", m_rl_exts, "extensions: pdf jpg; empty = any");
        field("Untouched for", m_rl_days, "days; empty = any age");
        m_rl_do = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(
            {"Move to a folder", "Move to the Trash", "Archive (~/Archive/<year>/...)", "File into a project"}));
        field("Then", *m_rl_do, nullptr);
        field("To ~/", m_rl_dest, "a folder (may use {year}); for File, the project");
        field("Rename to", m_rl_rename, "Move only; {stem} {ext} {yyyymmdd} {yyyymmdd-hhmm}; empty = keep");
        body->append(*grid);
        for (auto* e : {&m_rl_name, &m_rl_folder, &m_rl_glob, &m_rl_exts, &m_rl_days, &m_rl_dest, &m_rl_rename})
            e->signal_changed().connect([this] { on_rl_form(); });
        m_rl_do->property_selected().signal_changed().connect([this] { on_rl_form(); });
        body->append(m_rl_preview);
        page->append(*scrolled(*body));
        auto bar = bar_with(m_rl_edit_gate);
        m_rl_delete.set_label("Delete");
        m_rl_delete.add_css_class("ghost");
        m_rl_delete.signal_clicked().connect([this] { on_rl_delete(); });
        m_rl_save_off.set_label("Save, off");
        m_rl_save_off.add_css_class("ghost");
        m_rl_save_off.signal_clicked().connect([this] { on_rl_save(false); });
        m_rl_save_on.set_label("Save and turn on");
        m_rl_save_on.add_css_class("act");
        m_rl_save_on.signal_clicked().connect([this] { on_rl_save(true); });
        bar->append(m_rl_delete);
        bar->append(m_rl_save_off);
        bar->append(m_rl_save_on);
        page->append(*bar);
        m_rl_states.add(*page, "edit");
    }

    // run
    {
        auto page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
        head->append(*hero("rouster-hero-rules", 72));
        auto t = label("Run rules now", "hero-big", 0.0f);
        t->set_hexpand(true);
        head->append(*t);
        head->append(*back());
        body->append(*head);
        body->append(m_rl_run_list);
        page->append(*scrolled(*body));
        auto bar = bar_with(m_rl_run_gate);
        m_rl_go.add_css_class("act");
        m_rl_go.signal_clicked().connect([this] { on_rl_run(); });
        bar->append(m_rl_go);
        page->append(*bar);
        m_rl_states.add(*page, "run");
    }

    // done
    {
        auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        done->set_valign(Gtk::Align::CENTER);
        done->set_halign(Gtk::Align::CENTER);
        done->set_margin(40);
        done->append(*hero("rouster-hero-done", 112));
        m_rl_done_big.add_css_class("hero-big");
        done->append(m_rl_done_big);
        m_rl_done_caption.add_css_class("hero-sub");
        m_rl_done_caption.set_wrap(true);
        m_rl_done_caption.set_justify(Gtk::Justification::CENTER);
        m_rl_done_caption.set_max_width_chars(60);
        done->append(m_rl_done_caption);
        auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        btns->set_halign(Gtk::Align::CENTER);
        btns->set_margin_top(12);
        auto undo = Gtk::make_managed<Gtk::Button>("Undo");
        undo->add_css_class("ghost");
        undo->set_action_name("win.undo");
        auto act = Gtk::make_managed<Gtk::Button>("Activity");
        act->add_css_class("ghost");
        act->signal_clicked().connect([this] { go_to("activity"); });
        btns->append(*undo);
        btns->append(*act);
        btns->append(*back());
        done->append(*btns);
        m_rl_states.add(*done, "done");
    }

    m_rl_page.append(m_rl_states);
    m_stack.add(m_rl_page, "rules", "Rules");
}

// ─────────────────────────────────────────────────────────────────────────────
// Dev projects (s007) -- the Sweep shape, one card per project:
//   start   : hero, one line, the orb ("Look").
//   running : spinner, projects checked, build output found; Stop.
//   results : the checked build output; a card per workspace / repo with its
//             verdict (Backed up / Not clean / No remote), the failing checks,
//             template drift, and its build folders with a check each. The
//             cards ARE the preview; the act bar moves only build output.
//   done    : what went to the Trash, Undo, Activity, Look again.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_dev_page() {  // zone: Dev projects
    m_dv_states.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    m_dv_states.set_transition_duration(220);
    m_dv_states.set_vexpand(true);

    // start
    auto start = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    start->set_valign(Gtk::Align::CENTER);
    start->set_halign(Gtk::Align::CENTER);
    start->set_margin(40);
    start->append(*hero("rouster-hero-dev", 128));
    start->append(*label("Dev projects", "hero-title"));
    m_dv_sub.add_css_class("hero-sub");
    m_dv_sub.set_wrap(true);
    m_dv_sub.set_justify(Gtk::Justification::CENTER);
    m_dv_sub.set_max_width_chars(56);
    m_dv_sub.set_text("Your workspaces and repos under ~/Develop: is each one safely on its remote, "
                      "does it match the template, and how much build output is lying around. "
                      "Build folders go to the Trash; Undo brings them back.");
    start->append(m_dv_sub);
    m_dv_find.set_label("Look");
    m_dv_find.add_css_class("orb");
    m_dv_find.set_halign(Gtk::Align::CENTER);
    m_dv_find.set_margin_top(18);
    m_dv_find.set_action_name("win.dev-find");
    start->append(m_dv_find);
    m_dv_states.add(*start, "start");

    // running
    auto run = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    run->set_valign(Gtk::Align::CENTER);
    run->set_halign(Gtk::Align::CENTER);
    m_dv_spinner.add_css_class("big");
    m_dv_spinner.set_halign(Gtk::Align::CENTER);
    run->append(m_dv_spinner);
    m_dv_run_big.add_css_class("hero-big");
    run->append(m_dv_run_big);
    m_dv_run_caption.add_css_class("hero-caption");
    run->append(m_dv_run_caption);
    m_dv_cancel.set_label("Stop");
    m_dv_cancel.add_css_class("ghost");
    m_dv_cancel.set_halign(Gtk::Align::CENTER);
    m_dv_cancel.set_margin_top(12);
    m_dv_cancel.set_action_name("win.dev-cancel");
    run->append(m_dv_cancel);
    m_dv_states.add(*run, "running");

    // results
    auto res_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    auto res = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    res->set_margin_start(32);
    res->set_margin_end(32);
    res->set_margin_top(12);
    res->set_margin_bottom(16);
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    head->append(*hero("rouster-hero-dev", 88));
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    m_dv_big.add_css_class("hero-big");
    m_dv_big.set_xalign(0.0f);
    m_dv_caption.add_css_class("hero-caption");
    m_dv_caption.set_xalign(0.0f);
    m_dv_caption.set_wrap(true);
    words->append(m_dv_big);
    words->append(m_dv_caption);
    head->append(*words);
    auto again = Gtk::make_managed<Gtk::Button>("Look again");
    again->add_css_class("ghost");
    again->set_valign(Gtk::Align::CENTER);
    again->set_action_name("win.dev-find");
    head->append(*again);
    res->append(*head);
    res->append(m_dv_list);
    scroll->set_child(*res);
    res_page->append(*scroll);

    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    bar->add_css_class("act-bar");
    m_dv_gate.add_css_class("dim");
    m_dv_gate.set_xalign(0.0f);
    m_dv_gate.set_hexpand(true);
    m_dv_gate.set_wrap(true);
    bar->append(m_dv_gate);
    m_dv_go.add_css_class("act");
    m_dv_go.set_action_name("win.dev-run");
    bar->append(m_dv_go);
    res_page->append(*bar);
    m_dv_states.add(*res_page, "results");

    // done
    auto done = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    done->set_valign(Gtk::Align::CENTER);
    done->set_halign(Gtk::Align::CENTER);
    done->set_margin(40);
    done->append(*hero("rouster-hero-done", 112));
    m_dv_done_big.add_css_class("hero-big");
    done->append(m_dv_done_big);
    m_dv_done_caption.add_css_class("hero-sub");
    m_dv_done_caption.set_wrap(true);
    m_dv_done_caption.set_justify(Gtk::Justification::CENTER);
    m_dv_done_caption.set_max_width_chars(58);
    done->append(m_dv_done_caption);
    auto btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    btns->set_halign(Gtk::Align::CENTER);
    btns->set_margin_top(12);
    auto undo = Gtk::make_managed<Gtk::Button>("Undo");
    undo->add_css_class("ghost");
    undo->set_action_name("win.undo");
    auto act = Gtk::make_managed<Gtk::Button>("Activity");
    act->add_css_class("ghost");
    act->signal_clicked().connect([this] { go_to("activity"); });
    auto more = Gtk::make_managed<Gtk::Button>("Look again");
    more->add_css_class("ghost");
    more->set_action_name("win.dev-find");
    btns->append(*undo);
    btns->append(*act);
    btns->append(*more);
    done->append(*btns);
    m_dv_states.add(*done, "done");

    // confirm (s007b): the retire / restore plan, step by step -- its preview.
    auto cf_page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    auto cf_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    cf_scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    cf_scroll->set_vexpand(true);
    auto cf = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    cf->set_margin_start(32);
    cf->set_margin_end(32);
    cf->set_margin_top(12);
    cf->set_margin_bottom(16);
    auto cf_head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    cf_head->append(*hero("rouster-hero-dev", 72));
    auto cf_words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    cf_words->set_valign(Gtk::Align::CENTER);
    cf_words->set_hexpand(true);
    m_dv_cf_title.add_css_class("hero-title");
    m_dv_cf_title.set_xalign(0.0f);
    m_dv_cf_sub.add_css_class("hero-caption");
    m_dv_cf_sub.set_xalign(0.0f);
    m_dv_cf_sub.set_wrap(true);
    cf_words->append(m_dv_cf_title);
    cf_words->append(m_dv_cf_sub);
    cf_head->append(*cf_words);
    cf->append(*cf_head);
    cf->append(m_dv_cf_list);
    cf_scroll->set_child(*cf);
    cf_page->append(*cf_scroll);
    auto cf_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    cf_bar->add_css_class("act-bar");
    auto back = Gtk::make_managed<Gtk::Button>("Back");
    back->add_css_class("ghost");
    back->signal_clicked().connect([this] { m_dv_states.set_visible_child("results"); });
    cf_bar->append(*back);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    cf_bar->append(*spacer);
    m_dv_cf_go.add_css_class("act");
    m_dv_cf_go.signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_dev_confirm));
    cf_bar->append(m_dv_cf_go);
    cf_page->append(*cf_bar);
    m_dv_states.add(*cf_page, "confirm");

    m_dv_page.append(m_dv_states);
    m_stack.add(m_dv_page, "dev", "Dev projects");
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
    auto intro = label("Everything Rouster has moved, shared or put in the Trash, newest first. "
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
