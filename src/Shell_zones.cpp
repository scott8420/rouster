#include "Shell.hpp"
#include "Log.hpp"

#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

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

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    auto scan = Gio::Menu::create();
    scan->append("Smart Scan", "win.scan");
    scan->append("Scan a folder\u2026", "win.pick-folder");
    m_recents_menu = Gio::Menu::create();
    scan->append_submenu("Scan again", m_recents_menu);
    menu->append_section(scan);

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
