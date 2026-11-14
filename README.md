# Rouster

A roustabout does the heavy hauling and the cleanup. Rouster mops up a Linux
home folder -- caches, duplicates, build output, system clutter -- and never
loses anything on the way. Organizing (where files live, the archive, rules,
projects) is [Tidy](https://github.com/scott8420/tidy)'s work; Rouster cleans
what Tidy leaves.

- **Smart Scan** -- one scan, one summary, safe items pre-checked.
- **Sweep** -- caches, thumbnails, Trash, old logs, orphaned app folders.
- **Duplicates** -- files and whole folders; on btrfs, reflink dedupe so
  every path stays put and the space comes back.
- **Dev projects** -- clean build output; retire workspaces whose repos are
  verified pushed to GitHub, keeping the docs and a one-click restore.
- **System** -- the package cache, the journal, fstrim (through a small
  polkit helper).
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

Fedora: `sudo dnf install gtkmm4.0-devel spdlog-devel cmake gcc-c++ ninja-build`
Ubuntu: `sudo apt install libgtkmm-4.0-dev libspdlog-dev cmake g++ ninja-build`

Built on the Cairn starter. gtkmm 4, C++23. MIT license.
