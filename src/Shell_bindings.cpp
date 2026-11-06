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
    m_act_sw_find   = add_action("sweep-find",   sigc::mem_fun(*this, &Shell::on_sweep_find));
    m_act_sw_cancel = add_action("sweep-cancel", sigc::mem_fun(*this, &Shell::on_sweep_cancel));
    m_act_sw_run    = add_action("sweep-run",    sigc::mem_fun(*this, &Shell::on_sweep_run));
    m_act_sw_cancel->set_enabled(false);
    m_act_sw_run->set_enabled(false);
    add_action("go-sweep",      [this]() { go_to("sweep"); });
    m_act_dv_find   = add_action("dev-find",   sigc::mem_fun(*this, &Shell::on_dev_find));
    m_act_dv_cancel = add_action("dev-cancel", sigc::mem_fun(*this, &Shell::on_dev_cancel));
    m_act_dv_run    = add_action("dev-run",    sigc::mem_fun(*this, &Shell::on_dev_run));
    m_act_dv_cancel->set_enabled(false);
    m_act_dv_run->set_enabled(false);
    m_act_du_find   = add_action("dups-find",   sigc::mem_fun(*this, &Shell::on_dups_find));
    m_act_du_cancel = add_action("dups-cancel", sigc::mem_fun(*this, &Shell::on_dups_cancel));
    m_act_du_run    = add_action("dups-run",    sigc::mem_fun(*this, &Shell::on_dups_run));
    m_act_du_cancel->set_enabled(false);
    m_act_du_run->set_enabled(false);
    add_action("go-dups",       [this]() { go_to("dups"); });
    m_act_ar_find   = add_action("archive-find",   sigc::mem_fun(*this, &Shell::on_archive_find));
    m_act_ar_cancel = add_action("archive-cancel", sigc::mem_fun(*this, &Shell::on_archive_cancel));
    m_act_ar_run    = add_action("archive-run",    sigc::mem_fun(*this, &Shell::on_archive_run));
    m_act_ar_cancel->set_enabled(false);
    m_act_ar_run->set_enabled(false);
    add_action("go-archive",    [this]() { go_to("archive"); });
    m_act_pr_find   = add_action("profile-find",   sigc::mem_fun(*this, &Shell::on_profile_find));
    m_act_pr_run    = add_action("profile-run",    sigc::mem_fun(*this, &Shell::on_profile_run));
    m_act_pr_run->set_enabled(false);
    add_action("go-profile",    [this]() { go_to("profile"); });
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
