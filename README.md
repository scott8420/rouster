# Rouster

A roustabout does the heavy hauling and the cleanup. Rouster keeps a Linux
home folder in order: it **organizes first, cleans second**, and never loses
anything on the way.

- **Smart Scan** -- one scan, one summary, safe items pre-checked.
- **Sweep** -- caches, thumbnails, Trash, old logs, orphaned app folders.
- **Duplicates** -- files and whole folders; on btrfs, reflink dedupe so
  every path stays put and the space comes back.
- **Dev projects** -- clean build output; retire workspaces whose repos are
  verified pushed to GitHub, keeping the docs and a one-click restore.
- **Archive** -- files untouched for months, by year, restorable.
- **Organize** -- project templates, datetime-stamped naming, Hazel-style
  rules for Downloads and other folders.
- **Profile** -- back up and restore the things that make the desktop feel
  like home: dotfiles, GNOME settings, extensions, fonts, package lists.

Safety first: nothing is hard-deleted, every action is recorded and undoable,
a btrfs snapshot is taken before cleanup, and the app runs unprivileged.

**Status:** early. The spine builds; the modules are on the road.

## Build, install, uninstall

```
./build.sh                    # build/rouster and build/rouster_selftest
./build/rouster_selftest
./install.sh                  # ~/.local, no sudo   (--system, --prefix DIR, --build)
./uninstall.sh                # removes exactly what install wrote (--purge for settings)
```

Fedora: `sudo dnf install gtkmm4.0-devel spdlog-devel sqlite-devel cmake gcc-c++ ninja-build`
Ubuntu: `sudo apt install libgtkmm-4.0-dev libspdlog-dev libsqlite3-dev cmake g++ ninja-build`

Built on the Cairn starter. gtkmm 4, C++23. MIT license.
