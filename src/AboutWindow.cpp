#include "AboutWindow.hpp"
#include "widgets/Widgets.hpp"
#include "Registry.hpp"
#include "core/About.hpp"

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/linkbutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stack.h>
#include <gtkmm/stackswitcher.h>
#include <gtk/gtk.h>   // gtk_get_*_version: the GTK actually running, for bug reports

#ifndef ROUSTER_VERSION
#define ROUSTER_VERSION "dev"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// AboutWindow (s014a2) -- two tabs in the header (StackSwitcher): About (the
// colour logo, version, what Rouster is, website, licence, the GTK running)
// and Credits (core::about::credits() -- made by, built with, works with,
// inspired by). Still the hide-on-close singleton of the Cairn seed.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster {

namespace {
Gtk::Label* label(const std::string& t, const char* css, float x = 0.5f, bool wrap = true) {
    auto* l = Gtk::make_managed<Gtk::Label>(t);
    l->set_xalign(x);
    l->set_wrap(wrap);
    l->set_justify(x == 0.5f ? Gtk::Justification::CENTER : Gtk::Justification::LEFT);
    if (css && *css) l->add_css_class(css);
    return l;
}
}  // namespace

AboutWindow::AboutWindow(Gtk::Window& parent) {
    namespace ab = core::about;
    set_name("shell.about");
    registry::add("shell.about", this);
    set_title("About Rouster");
    set_modal(true);
    set_resizable(false);
    set_default_size(540, 600);
    set_transient_for(parent);
    set_hide_on_close(true);   // singleton: X hides, never destructs mid-session
    add_css_class("rouster");
    add_css_class("about");

    auto* stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    auto* sw = Gtk::make_managed<Gtk::StackSwitcher>();
    sw->set_stack(*stack);
    hb->set_title_widget(*sw);
    set_titlebar(*hb);

    // ── About ───────────────────────────────────────────────────────────────
    auto* about = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    about->set_margin(28);
    about->set_valign(Gtk::Align::CENTER);
    auto* logo = Gtk::make_managed<Gtk::Image>();
    logo->set_from_icon_name("rouster-hero-logo");   // the colour pie (s006)
    logo->set_pixel_size(112);
    about->append(*logo);
    about->append(*label(ab::kName, "hero-title"));
    auto* ver = label(std::string("Version ") + ROUSTER_VERSION, "about-version");
    about->append(*ver);
    auto* tag = label(ab::tagline(), "hero-sub");
    tag->set_max_width_chars(44);
    tag->set_margin_top(6);
    about->append(*tag);
    auto* desc = label(ab::description(), "help-p");
    desc->set_max_width_chars(52);
    desc->set_margin_top(6);
    about->append(*desc);
    auto* site = Gtk::make_managed<Gtk::LinkButton>(ab::kWebsite, "github.com/scott8420/rouster");
    site->set_halign(Gtk::Align::CENTER);
    site->set_margin_top(8);
    about->append(*site);
    about->append(*label(std::string(ab::kLicense) + "  ·  " + ab::kCopyright, "about-small"));
    auto* tech = label(std::string(ab::kAppId) + "  ·  GTK " + std::to_string(gtk_get_major_version()) + "." +
                           std::to_string(gtk_get_minor_version()) + "." + std::to_string(gtk_get_micro_version()),
                       "about-small");
    about->append(*tech);
    stack->add(*about, "about", "About");

    // ── Credits ─────────────────────────────────────────────────────────────
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    auto* cr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    cr->set_margin(28);
    for (const auto& g : ab::credits()) {
        auto* h = label(g.heading, "help-h", 0.0f);
        cr->append(*h);
        for (const auto& c : g.items) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            row->set_margin_start(8);
            auto* words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
            words->set_hexpand(true);
            words->append(*label(c.name, "card-title", 0.0f));
            words->append(*label(c.what, "card-sub", 0.0f));
            row->append(*words);
            if (!c.license.empty()) {
                auto* lic = label(c.license, "about-small", 1.0f, false);
                lic->set_valign(Gtk::Align::CENTER);
                row->append(*lic);
            }
            if (!c.url.empty()) {
                auto* go = Gtk::make_managed<Gtk::LinkButton>(c.url, "Website");
                go->set_valign(Gtk::Align::CENTER);
                row->append(*go);
            }
            cr->append(*row);
        }
    }
    scroll->set_child(*cr);
    stack->add(*scroll, "credits", "Credits");

    set_child(*stack);
}

AboutWindow::~AboutWindow() {
    registry::remove(this);
}

}  // namespace rouster
