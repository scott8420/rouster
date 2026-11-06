#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>

// Shell_zones.cpp -- ZONES. Construction of the named regions: the window frame
// and header, the sidebar+stack body, the pattern pages, and the menu model.

namespace rouster {

void Shell::build_shell() {  // zone: window + sidebar + stack
    set_title("Rouster");
    set_default_size(860, 620);

    auto header = Gtk::make_managed<Gtk::HeaderBar>();

    // Brand logo at the start: a frameless button whose icon is a symbolic
    // resource resolved from the compiled-in bundle (App registers it), so this
    // one button exercises the resource pipeline. Click opens the About window
    // (the dialog-lifetime exemplar).
    m_logo_button.set_icon_name("rouster-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About Rouster");
    m_logo_button.set_action_name("win.about");
    header->pack_start(m_logo_button);

    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    header->pack_end(m_menu_button);
    set_titlebar(*header);

    // StackSidebar drives the Stack: adding a page with a title makes a sidebar
    // row automatically. That IS the Stack/StackSidebar paradigm -- no manual
    // row wiring.
    m_sidebar.set_stack(m_stack);
    m_sidebar.set_size_request(160, -1);
    m_stack.set_hexpand(true);
    m_stack.set_vexpand(true);

    m_body.append(m_sidebar);
    m_body.append(m_stack);
    set_child(m_body);
}

void Shell::build_pages() {  // zone: the module pages
    // One module so far. Sweep, Dev projects, Duplicates... each arrive as a
    // page here, and the sidebar grows a row per page by itself.
    build_scan_page();
    build_preflight_page();
    build_activity_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Smart Scan (s002, REQUIREMENTS §4.1) -- one button, one summary: "found X"
// by kind, each kind expandable to its biggest folders, each with the one-line
// "why" from §2. READ-ONLY: there is no act button on this page, by design --
// the first one arrives after the s004 door.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_scan_page() {  // zone: Smart Scan
    m_scan_page.set_margin(16);

    auto title = Gtk::make_managed<Gtk::Label>();
    title->set_xalign(0.0f);
    title->set_markup("<span size='x-large' weight='bold'>Smart Scan</span>");
    m_scan_page.append(*title);

    m_scan_where.set_xalign(0.0f);
    m_scan_where.add_css_class("dim-label");
    m_scan_where.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    m_scan_where.set_text("Looks through " + m_home + " and sorts what it finds. Nothing is changed.");
    m_scan_page.append(m_scan_where);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_scan_button.set_label("Scan");
    m_scan_button.add_css_class("suggested-action");
    m_scan_button.set_action_name("win.scan");
    m_cancel_button.set_label("Stop");
    m_cancel_button.set_action_name("win.scan-cancel");
    m_scan_status.set_xalign(0.0f);
    m_scan_status.set_hexpand(true);
    m_scan_status.set_ellipsize(Pango::EllipsizeMode::END);
    row->append(m_scan_button);
    row->append(m_cancel_button);
    row->append(m_spinner);
    row->append(m_scan_status);
    m_scan_page.append(*row);

    m_scan_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scan_scroll.set_vexpand(true);
    m_scan_scroll.set_child(m_scan_results);
    m_scan_page.append(m_scan_scroll);

    m_stack.add(m_scan_page, "scan", "Smart Scan");
}

// ─────────────────────────────────────────────────────────────────────────────
// Preflight (s003, REQUIREMENTS §3) -- the checklist that will grey the act
// buttons once there are act buttons (s004 on). Read-only; runs at startup and
// on "Check again". Green / amber / red, each with its one line of why.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_preflight_page() {  // zone: Preflight
    m_pf_page.set_margin(16);
    // The row colours are ours, not the theme's: GTK's success/warning/error
    // classes do not reach an Image under every theme, and a checklist whose
    // red looks like its green has failed at its one job.
    auto css = Gtk::CssProvider::create();
    css->load_from_data(".pf-green { color: #26a269; } .pf-amber { color: #c88800; } .pf-red { color: #c01c28; }");
    Gtk::StyleContext::add_provider_for_display(get_display(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    auto title = Gtk::make_managed<Gtk::Label>();
    title->set_xalign(0.0f);
    title->set_markup("<span size='x-large' weight='bold'>Preflight</span>");
    m_pf_page.append(*title);

    m_pf_summary.set_xalign(0.0f);
    m_pf_summary.set_wrap(true);
    m_pf_summary.add_css_class("dim-label");
    m_pf_page.append(m_pf_summary);

    m_pf_rows.set_margin_top(6);
    m_pf_page.append(m_pf_rows);

    m_pf_button.set_label("Check again");
    m_pf_button.set_halign(Gtk::Align::START);
    m_pf_button.set_action_name("win.preflight");
    m_pf_page.append(m_pf_button);

    m_stack.add(m_pf_page, "preflight", "Preflight");
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity (s004) -- the manifest, readable: every batch that went through the
// door, newest first, each expandable to what moved where. One button takes the
// newest batch back. Nothing on this page acts except Undo.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::build_activity_page() {  // zone: Activity
    m_act_page.set_margin(16);
    auto title = Gtk::make_managed<Gtk::Label>();
    title->set_xalign(0.0f);
    title->set_markup("<span size='x-large' weight='bold'>Activity</span>");
    m_act_page.append(*title);

    auto intro = Gtk::make_managed<Gtk::Label>(
        "Everything Rouster has moved or put in the Trash, newest first. Nothing is ever deleted, "
        "and any batch can be taken back.");
    intro->set_xalign(0.0f);
    intro->set_wrap(true);
    intro->add_css_class("dim-label");
    m_act_page.append(*intro);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_act_undo.set_label("Undo");
    m_act_undo.set_action_name("win.undo");
    m_act_said.set_xalign(0.0f);
    m_act_said.set_hexpand(true);
    m_act_said.set_wrap(true);
    row->append(m_act_undo);
    row->append(m_act_said);
    m_act_page.append(*row);

    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_vexpand(true);
    scroll->set_child(m_act_list);
    m_act_page.append(*scroll);

    m_stack.add(m_act_page, "activity", "Activity");
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto scan = Gio::Menu::create();
    scan->append("Smart Scan", "win.scan");
    scan->append("Scan a folder\u2026", "win.pick-folder");
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
