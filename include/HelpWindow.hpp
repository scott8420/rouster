#pragma once
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/window.h>

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// HelpWindow (s013a2) -- paints core::help (the text lives there, GTK-free,
// held to the app by the selftest). Hide-on-close singleton like About.
// Left: a search box and the topics by group; right: the topic.
// Shell opens it with F1 at the page you're on (show_topic(page id)).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster {

class HelpWindow : public Gtk::Window {
public:
    explicit HelpWindow(Gtk::Window& parent);
    ~HelpWindow() override;
    void show_topic(const std::string& id);

private:
    void fill_list(const std::string& query);
    void paint(const std::string& id);

    Gtk::SearchEntry    m_search;
    Gtk::ListBox        m_list;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_page{Gtk::Orientation::VERTICAL, 8};
    std::vector<std::string> m_row_ids;   // per ListBox row: topic id, "" for a group header
    std::string         m_current;
    bool                m_filling = false;
};

}  // namespace rouster
