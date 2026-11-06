#include "Shell.hpp"
#include "Log.hpp"
#include "core/Recents.hpp"

#include <giomm/simpleactiongroup.h>
#include <glibmm/variant.h>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions and
// the parameterised recents action group. No slot bodies here.

namespace rouster {

void Shell::bind_actions() {  // bindings: actions + recents group
    m_act_scan   = add_action("scan",        sigc::mem_fun(*this, &Shell::on_scan));
    m_act_cancel = add_action("scan-cancel", sigc::mem_fun(*this, &Shell::on_scan_cancel));
    m_act_pick   = add_action("pick-folder", sigc::mem_fun(*this, &Shell::on_pick_folder));
    m_act_undo_action = add_action("undo", sigc::mem_fun(*this, &Shell::on_undo));
    add_action("preflight",     sigc::mem_fun(*this, &Shell::on_preflight));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("about",         sigc::mem_fun(*this, &Shell::on_about));
    add_action("shortcuts",     sigc::mem_fun(*this, &Shell::on_shortcuts));
    add_action("quit",          [this]() { close(); });

    // Recents: recents.open(<path>) carries each entry's path as its target, so
    // one action serves every menu row (the parameterised-action shape).
    auto group = Gio::SimpleActionGroup::create();
    group->add_action_with_parameter(
        "open", Glib::Variant<Glib::ustring>::variant_type(),
        [this](const Glib::VariantBase& param) {
            const std::string p =
                Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param).get();
            on_recent_open(p);
        });
    m_recents_clear = Gio::SimpleAction::create("clear");
    m_recents_clear->signal_activate().connect(
        [this](const Glib::VariantBase&) { on_recent_clear(); });
    group->add_action(m_recents_clear);
    insert_action_group("recents", group);

    m_recents = core::load_recents(m_recents_file);
}

}  // namespace rouster
