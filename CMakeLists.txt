cmake_minimum_required(VERSION 3.20)
project(rouster VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
find_package(spdlog REQUIRED)

# ── rouster_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable. (docs/two-layer-seam.md.)
# ── latch -- the shared door (2026-11-10), one repo beside this one ───────────
# Two ways (s019a): its SOURCE beside this one (<dir>/latch-project/latch, or
# -DLATCH_DIR=...) is used first -- the selftest's source gate then reads the
# door's code too; otherwise the INSTALLED latch (latch's ./install.sh ->
# find_package(latch)). Installed: rebuild Rouster after reinstalling latch.
set(LATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../latch-project/latch" CACHE PATH "the latch library's source (the shared door)")
if(EXISTS "${LATCH_DIR}/CMakeLists.txt")
    add_subdirectory(${LATCH_DIR} ${CMAKE_BINARY_DIR}/latch)
    set(ROUSTER_LATCH_SOURCE "${LATCH_DIR}")
    message(STATUS "latch: from source, ${LATCH_DIR}")
else()
    find_package(latch QUIET)
    if(NOT latch_FOUND)
        message(FATAL_ERROR "latch not found -- install it (latch-project/latch/install.sh), or unzip latch-project beside rouster-project, or -DLATCH_DIR=...")
    endif()
    add_library(latch ALIAS latch::latch)
    set(ROUSTER_LATCH_SOURCE "")
    message(STATUS "latch: installed, ${latch_DIR} (version ${latch_VERSION})")
endif()

add_library(rouster_core STATIC
    src/core/Recents.cpp
    src/core/Scan.cpp
    # TidyLink (s019d): Tidy's Hands off (read), its marks never touched, the plan guard.
    # (The catalog, s015b, retired at s019d -- Tidy keeps its own; SQLite left the build.)
    src/core/TidyLink.cpp
    src/core/Preflight.cpp
    # THE ONE DOOR (s004) lives in latch since 2026-11-10 (with git/child,
    # packages and the never-touch list); include/core/Door.hpp is the shim.
    # Sweep (s006): finds and plans, read-only; the door acts.
    src/core/Sweep.cpp
    # Dev projects (s007): find, verify (git, read-only), lint, plan; the door acts.
    src/core/Dev.cpp
    # Duplicates (s008): size -> head/tail -> full hash, folders, keep rules; read-only.
    src/core/Dups.cpp
    # s019c: Archive (s009, s018), Organize (s010) and Rules (s011) retired -- Tidy owns them.
    src/core/Profile.cpp
    src/core/Secrets.cpp
    # Settings: Rouster's own small files (profile-notes.md) in its data dir.
    src/core/Settings.cpp
    # Known (s007b): the names promoted to known-optional -- Rouster's own setting.
    src/core/Known.cpp
    # Helper (s013): what the app and the root helper agree on -- pure, also in the helper.
    src/core/Helper.cpp
    # Help (s013a2): the help text as data -- the selftest holds it to the pages.
    src/core/Help.cpp
    # System (s013b): package cache, journal, trim -- measured as you, done by the helper.
    src/core/System.cpp
    # About (s014a2): the About window's facts and credits, as data.
    src/core/About.cpp
    # The SHORTCUT-REGISTRY paradigm: one list, two consumers (App wires accels
    # from it, ShortcutsDialog renders from it) so a key and its advertised row
    # can't drift. GTK-free, so it lives in core and the selftest guards it
    # headless -- a pure test proves no two chords collide.
    src/core/Shortcuts.cpp
    # The UNDO paradigm (s046): core/Undo.hpp is the journal (header-only, a
    # template over the app's store); Stones is the demo model + its verbs +
    # the verb table the selftest gate walks. docs/undo.md.
    src/core/Stones.cpp
)
target_include_directories(rouster_core PUBLIC include)
target_link_libraries(rouster_core PUBLIC latch)
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(rouster_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(rouster_selftest src/selftest.cpp)
target_link_libraries(rouster_selftest PRIVATE rouster_core)
# The source gate reads src/ and include/ at test time.
target_compile_definitions(rouster_selftest PRIVATE ROUSTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                   LATCH_SOURCE_DIR="${ROUSTER_LATCH_SOURCE}")

# ── rouster-helper (s013) -- the small ROOT helper, run only through pkexec ──────
# Its own tiny link line: libc + the shared, pure Helper.cpp. No GTK, no spdlog,
# no json -- the less a root program carries, the less there is to trust.
# Installed by `install.sh --helper` at core::helper::kHelperPath.
add_executable(rouster-helper src/helper/main.cpp src/core/Helper.cpp)
target_include_directories(rouster-helper PRIVATE include)

# ── App resources (gresource) -- the RESOURCE-PIPELINE paradigm ────────────────
# glib-compile-resources embeds resources/rouster.gresource.xml into a C source;
# App::on_activate registers it and points the icon theme at the bundle so
# set_icon_name("rouster-logo-symbolic") resolves out of the binary. The generated
# .c compiles as C (project enables C above) so rouster_resources_get_resource()
# has C linkage, matching the extern "C" prototype in App.cpp. Adding an icon:
# drop the .svg in resources/icons/scalable/apps/, list it in the .xml, rebuild.
# (docs/resources-and-dialog-lifetime.md)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(ROUSTER_GRESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/resources/rouster.gresource.xml")
set(ROUSTER_RESOURCES_C   "${CMAKE_CURRENT_BINARY_DIR}/rouster-resources.c")
file(GLOB ROUSTER_RESOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/scalable/apps/*.svg")
list(APPEND ROUSTER_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/rouster.css")
add_custom_command(
    OUTPUT  "${ROUSTER_RESOURCES_C}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${ROUSTER_RESOURCES_C}"
            --generate-source
            --c-name rouster_resources
            rouster.gresource.xml
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    DEPENDS "${ROUSTER_GRESOURCE_XML}" ${ROUSTER_RESOURCE_FILES}
    COMMENT "Compiling gresource bundle (rouster-resources.c)"
    VERBATIM)

# ── rouster -- the GTK app ───────────────────────────────────────────────────────
# The Shell is a SPLIT CLASS across TUs by category (see the index in
# include/Shell.hpp); all parts compile into the one rouster::Shell.
add_executable(rouster
    ${ROUSTER_RESOURCES_C}
    src/main.cpp
    src/App.cpp
    # Follows the desktop's light/dark preference (GTK4 will not do it for us
    # and we do not link libadwaita). Needs no new dependency: the D-Bus and
    # GSettings calls are GIO, which gtkmm-4.0 already brings in.
    src/Appearance.cpp
    src/Shell.cpp
    src/Shell_zones.cpp
    src/Shell_bindings.cpp
    src/Shell_handlers.cpp
    src/Shell_helpers.cpp
    src/AboutWindow.cpp
    src/HelpWindow.cpp
    src/ShortcutsDialog.cpp
    src/Registry.cpp
    src/Log.cpp
)
target_include_directories(rouster PRIVATE include ${GTKMM_INCLUDE_DIRS})
target_link_libraries(rouster     PRIVATE ${GTKMM_LIBRARIES} rouster_core spdlog::spdlog)
target_link_directories(rouster   PRIVATE ${GTKMM_LIBRARY_DIRS})
target_compile_options(rouster    PRIVATE ${GTKMM_CFLAGS_OTHER})
target_compile_definitions(rouster PRIVATE ROUSTER_VERSION="${PROJECT_VERSION}")
