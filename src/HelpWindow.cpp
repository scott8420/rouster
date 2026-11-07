#include "HelpWindow.hpp"
#include "Registry.hpp"
#include "core/Help.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/separator.h>

namespace rouster {

namespace {
Gtk::Label* label(const std::string& t, const char* css, bool wrap = true) {
    auto* l = Gtk::make_managed<Gtk::Label>(t);
    l->set_xalign(0.0f);
    l->set_wrap(wrap);
    l->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    if (css && *css) l->add_css_class(css);
    return l;
}
}  // namespace

HelpWindow::HelpWindow(Gtk::Window& parent) {
    set_name("shell.help");
    registry::add("shell.help", this);
    set_title("Rouster Help");
    set_default_size(980, 700);
    set_transient_for(parent);
    set_hide_on_close(true);   // singleton: X hides, never destructs mid-session
    add_css_class("rouster");
    add_css_class("help");

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    set_titlebar(*hb);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    auto* side = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    side->add_css_class("sidebar-pane");
    side->set_size_request(250, -1);
    m_search.set_placeholder_text("Search help");
    m_search.set_margin(12);
    side->append(m_search);
    auto* ls = Gtk::make_managed<Gtk::ScrolledWindow>();
    ls->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    ls->set_vexpand(true);
    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    ls->set_child(m_list);
    side->append(*ls);
    body->append(*side);

    m_page.set_margin_start(40);
    m_page.set_margin_end(40);
    m_page.set_margin_top(24);
    m_page.set_margin_bottom(32);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_hexpand(true);
    m_scroll.set_vexpand(true);
    m_scroll.set_child(m_page);
    body->append(m_scroll);
    set_child(*body);

    m_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (m_filling || !row) return;
        const int i = row->get_index();
        if (i >= 0 && i < static_cast<int>(m_row_ids.size()) && !m_row_ids[i].empty()) paint(m_row_ids[i]);
    });
    m_search.signal_search_changed().connect([this] {
        fill_list(m_search.get_text());
        // The first match is shown as you type.
        for (std::size_t i = 0; i < m_row_ids.size(); ++i)
            if (!m_row_ids[i].empty()) { m_list.select_row(*m_list.get_row_at_index(static_cast<int>(i))); break; }
    });
    m_search.signal_activate().connect([this] { if (!m_current.empty()) paint(m_current); });

    fill_list("");
    show_topic("welcome");
}

HelpWindow::~HelpWindow() { registry::remove(this); }

void HelpWindow::fill_list(const std::string& query) {
    m_filling = true;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    m_row_ids.clear();
    std::string group;
    const auto found = core::help::search(query);
    for (const auto* t : found) {
        if (t->group != group) {
            group = t->group;
            auto* hrow = Gtk::make_managed<Gtk::ListBoxRow>();
            hrow->set_selectable(false);
            hrow->set_activatable(false);
            hrow->set_focusable(false);
            auto* h = label(group, "sidebar-section", false);
            h->set_text(Glib::ustring(group).uppercase());
            hrow->set_child(*h);
            m_list.append(*hrow);
            m_row_ids.push_back("");
        }
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*label(t->title, "", false));
        m_list.append(*row);
        m_row_ids.push_back(t->id);
    }
    if (found.empty()) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_selectable(false);
        row->set_child(*label("Nothing matches.", "card-sub", false));
        m_list.append(*row);
        m_row_ids.push_back("");
    }
    m_filling = false;
}

void HelpWindow::show_topic(const std::string& id) {
    const std::string want = core::help::find(id) ? id : "welcome";
    if (!m_search.get_text().empty()) { m_search.set_text(""); fill_list(""); }
    for (std::size_t i = 0; i < m_row_ids.size(); ++i)
        if (m_row_ids[i] == want) {
            m_filling = true;
            m_list.select_row(*m_list.get_row_at_index(static_cast<int>(i)));
            m_filling = false;
        }
    paint(want);
}

void HelpWindow::paint(const std::string& id) {
    const auto* t = core::help::find(id);
    if (!t) return;
    m_current = id;
    while (auto* c = m_page.get_first_child()) m_page.remove(*c);
    m_page.append(*label(t->title, "hero-title"));
    m_page.append(*label(t->lead, "hero-sub"));
    for (const auto& s : t->sections) {
        if (!s.heading.empty()) m_page.append(*label(s.heading, "help-h"));
        for (const auto& p : s.paras) {
            if (p.rfind("$ ", 0) == 0) {
                auto* l = label(p.substr(2), "help-cmd", false);
                l->set_selectable(true);
                l->set_tooltip_text("Select to copy");
                m_page.append(*l);
            } else if (p.rfind("- ", 0) == 0) {
                auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
                row->set_margin_start(8);
                auto* dot = label("•", "help-p", false);
                dot->set_valign(Gtk::Align::START);
                row->append(*dot);
                auto* l = label(p.substr(2), "help-p");
                l->set_hexpand(true);
                row->append(*l);
                m_page.append(*row);
            } else {
                m_page.append(*label(p, "help-p"));
            }
        }
    }
    m_scroll.get_vadjustment()->set_value(0);
}

}  // namespace rouster
