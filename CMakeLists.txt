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
add_library(rouster_core STATIC
    src/core/Recents.cpp
    src/core/Scan.cpp
    src/core/Preflight.cpp
    # THE ONE DOOR (s004): the only file that changes a user's files. The
    # selftest's source gate fails if any other file makes a write call.
    src/core/Door.cpp
    # Sweep (s006): finds and plans, read-only; the door acts.
    src/core/Sweep.cpp
    # Dev projects (s007): find, verify (git, read-only), lint, plan; the door acts.
    src/core/Dev.cpp
    # git as a child process (Dev reads with it; the door's Clone verb writes with it).
    src/core/Git.cpp
    # Known (s007b): the names promoted to known-optional -- Rouster's own setting.
    src/core/Known.cpp
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
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(rouster_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(rouster_selftest src/selftest.cpp)
target_link_libraries(rouster_selftest PRIVATE rouster_core)
# The source gate reads src/ and include/ at test time.
target_compile_definitions(rouster_selftest PRIVATE ROUSTER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

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
    src/ShortcutsDialog.cpp
    src/Registry.cpp
    src/Log.cpp
)
target_include_directories(rouster PRIVATE include ${GTKMM_INCLUDE_DIRS})
target_link_libraries(rouster     PRIVATE ${GTKMM_LIBRARIES} rouster_core spdlog::spdlog)
target_link_directories(rouster   PRIVATE ${GTKMM_LIBRARY_DIRS})
target_compile_options(rouster    PRIVATE ${GTKMM_CFLAGS_OTHER})
