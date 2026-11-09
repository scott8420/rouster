#include "core/Help.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace rouster::core::help {

namespace {

std::vector<Topic> build() {
    std::vector<Topic> t;

    // ── Start here ──────────────────────────────────────────────────────────
    t.push_back({"welcome", "Rouster at a glance", "Start here",
        "Keeps your home folder in order: organizes first, cleans second, and never loses anything on the way.",
        {
            {"", {
                "A roustabout does the heavy hauling and the cleanup. Rouster looks through your home folder, "
                "tells you what can go and why, files what belongs somewhere, and puts finished work away -- "
                "and every change it makes can be taken back."}},
            {"Every module works the same way", {
                "- Look. Rouster reads; looking never changes anything.",
                "- Read the list. Safe items are already checked; anything doubtful is left unchecked with the reason beside it.",
                "- Act. The big button at the bottom does exactly what the list shows -- nothing more.",
                "- Undo. Every act is one step in Activity, and Undo puts it back."}},
            {"The sidebar", {
                "- Clean up: Smart Scan, Sweep, System, Duplicates, Dev projects.",
                "- Organize: Tidy, Projects, Rules, Archive.",
                "- Safety: Preflight, Profile backup, Activity."}},
            {"Getting help", {
                "Press F1 on any page to open its help. Ctrl+? lists the keyboard shortcuts."}},
        }});

    t.push_back({"safety", "How Rouster keeps you safe", "Start here",
        "A cleaner that loses one file has failed, whatever else it does well.",
        {
            {"Nothing is hard-deleted", {
                "What Rouster removes goes to the Trash (the same Trash Files uses, with the original location "
                "recorded) or to ~/Archive. Rouster never empties the Trash -- you do that in Files when you're sure. "
                "So a clean frees space only once the Trash is emptied."}},
            {"One door, one record", {
                "Every change goes through one part of Rouster (the door), which writes what it is about to do "
                "to a record before it does it, and what happened after. That record is Activity, and it is what "
                "Undo reads -- even after a crash or a restart."}},
            {"It refuses, it doesn't advise", {
                "These are never touched, whatever a module or a rule asks:",
                "- ~/.ssh, ~/.gnupg, ~/.local/share/keyrings",
                "- ~/.config/dconf, ~/.local/share/gnome-shell/extensions, ~/.local/share/flatpak",
                "- any folder named fixes, anywhere",
                "- anything outside your home folder, anything reached through a symlink, anything on another drive",
                "- Rouster's own records"}},
            {"Packages are one thing", {
                "A Scrivener project (.scriv) -- and Pages, Numbers, Keynote, .rtfd, .app, a Photos library -- is a "
                "folder on Linux but one thing to Rouster. It is never looked inside, never split, never swept, "
                "paired or trashed piece by piece. The only thing Rouster ever does with one is move it whole "
                "(Archive does, when all of it is untouched), and Undo moves it back."}},
            {"Never overwrites, never copies-and-deletes", {
                "A move never lands on a file that's already there. A move to another drive is refused rather "
                "than copied and deleted."}},
            {"Preflight gates the act buttons", {
                "If a Preflight check is red, the act buttons are off and say why. See Preflight."}},
            {"Runs as you", {
                "Rouster runs with your own permissions. The one thing that needs root -- a snapshot -- goes "
                "through a small helper that asks for your password. So do System's package cache, journal and trim -- "
                "the only things Rouster touches outside your home. See The root helper."}},
        }});

    // ── Clean up ────────────────────────────────────────────────────────────
    t.push_back({"scan", "Smart Scan", "Clean up",
        "Sorts everything in your home folder into what can go and what must stay. Looking changes nothing.",
        {
            {"How to use it", {
                "Press Scan (F5). Rouster walks your home folder and shows the total it found in each kind. "
                "Click a kind to see its biggest folders and why they're that kind. Scan a folder... (Ctrl+O) "
                "looks at just one folder."}},
            {"The kinds", {
                "- Regenerable -- caches, thumbnails, build output. Apps make them again. Safe to clean.",
                "- State -- logs and histories. Mostly safe, item by item.",
                "- Settings -- ~/.config. Backed up, never cleaned.",
                "- App data -- ~/.local/share, ~/.var/app. Never cleaned automatically.",
                "- Trash -- what's waiting to be emptied.",
                "- Protected -- the never-touch list. Always left alone.",
                "- Your files -- everything else. Counted, not judged."}},
            {"Good to know", {
                "Sizes are disk use, like du. Other drives and btrfs snapshots mounted inside your home are listed "
                "under Not scanned, never entered. Smart Scan has no act button -- Sweep does the cleaning."}},
        }});

    t.push_back({"sweep", "Sweep", "Clean up",
        "Moves caches, thumbnails, app caches and old logs to the Trash.",
        {
            {"How to use it", {
                "Press Look. Read the list: each group says why it's safe; each row has its size. Uncheck "
                "anything you'd rather keep, then press Move ... to the Trash."}},
            {"What's checked and what isn't", {
                "- Thumbnails, caches in ~/.cache, Flatpak app caches -- checked.",
                "- Caches that are slow to get back (model downloads, pip, Go, ccache, JetBrains...) -- unchecked, with the reason.",
                "- Logs untouched for 30 days -- unchecked; they're your history."}},
            {"Afterwards", {
                "Nothing is freed until you empty the Trash in Files. Undo (on the done screen or in Activity) puts "
                "everything back where it was."}},
        }});

    t.push_back({"system", "System", "Clean up",
        "Housekeeping outside your home: the package cache, the system journal, an SSD trim -- done as root.",
        {
            {"How to use it", {
                "Press Look. Rouster measures each one as you (nothing changes), then shows a row each with its size "
                "and what it means. Check what you want and press Run ... as root. Your password is asked once."}},
            {"The three", {
                "- Package cache -- packages and repository data dnf (or apt) downloaded. Checked when there's anything "
                "in it. Can't be undone, but it refills on the next update.",
                "- System journal -- the log of past boots and errors. Only what is older than 28 days goes. Unchecked; "
                "can't be undone.",
                "- SSD trim -- tells the drive which blocks are free. Changes no file. Unchecked when your system "
                "already trims weekly (fstrim.timer, Fedora's default)."}},
            {"Good to know", {
                "Each row is a step in Activity; Undo only closes it -- nothing that ran as root is taken back. "
                "If you cancel the password, nothing runs. Needs the root helper -- see The root helper. "
                "Preflight's red checks stop it, like every other clean."}},
        }});

    t.push_back({"dups", "Duplicates", "Clean up",
        "Finds files and whole folders that are exact copies, and frees the space they take.",
        {
            {"How to use it", {
                "Press Look. Rouster compares sizes, then the beginnings and ends, then whole files -- so only true "
                "copies are listed. Each group shows the keeper first and why it was chosen; Keep this on any copy "
                "makes it the keeper instead."}},
            {"Two ways to free the space", {
                "- Share blocks (btrfs, the default) -- every file stays where it is, but the copies share the "
                "keeper's storage. Nothing moves; the space comes back.",
                "- Move copies to the Trash -- the copies go; the keeper stays."}},
            {"How the keeper is chosen", {
                "Not in Downloads or on the Desktop; not a copy-like name (\"x (2).pdf\", \"Copy of x\"); then the "
                "oldest; then the shortest path."}},
            {"Good to know", {
                "Hidden files, build folders, fixes/ and anything under 4 KB are left out. The filesystem checks "
                "every byte again as it shares, so a file that changed since Look is never shared. Undo of a share "
                "just closes the step -- the files were and are identical."}},
        }});

    t.push_back({"dev", "Dev projects", "Clean up",
        "Checks your code projects are safely on GitHub, cleans their build output, and retires finished ones.",
        {
            {"How to use it", {
                "Press Look. Rouster finds projects in ~/Develop (and ~/Projects, ~/src, ~/dev, ~/code) and asks "
                "git about each one -- it never fetches and never changes a repo."}},
            {"Backed up means all six", {
                "- everything committed",
                "- no stashes",
                "- the remote answers now",
                "- every branch's commits are on the remote",
                "- no branch only here",
                "- every tag on the remote"}},
            {"Build output", {
                "build/ folders with nothing tracked by git can go to the Trash. A project with no remote is shown "
                "but never offered."}},
            {"Retire and restore", {
                "Retire... (only when Backed up) shows every step first: it writes RESTORE.txt with the exact commit, "
                "then sends the working copy, old backups and all but the newest zip to the Trash. docs/, scripts/ "
                "and fixes/ stay. A retired project shows Restore..., which clones it back at that commit."}},
            {"Template lint", {
                "For workspaces, anything not in the template is listed. Known marks a name as yours; Forget undoes that."}},
        }});

    // ── Organize ────────────────────────────────────────────────────────────
    t.push_back({"tidy", "Tidy", "Organize",
        "Reads your whole home, proposes a lifespan for what it finds, and moves what you check -- one step per card, Undo takes it back.",
        {
            {"How to use it", {
                "Press Look. Rouster looks through your home (like Smart Scan), updates its catalog, and reports:",
                "- Short life -- installers and disk images, screenshots and the like unused for a month, reflex saves "
                "such as \"report (1).pdf\" or \"Copy of notes\".",
                "- Dormant -- neither opened nor changed in 18 months: candidates for the Archive.",
                "- Undecided -- loose at the top of home or on the Desktop: no home yet.",
                "- Active -- everything else; counted, not listed.",
                "Each row names the query that proposed it. The first query to claim a file wins."}},
            {"Acting on it", {
                "Each card has its own button. Short life and Undecided rows start checked, Dormant rows don't. "
                "Hover a row to see where it would go.",
                "- Short life -> ~/Closet, remembered with 30 days to live; after that a review offers it to the Trash. "
                "Nothing is ever deleted unasked.",
                "- Undecided -> gathered into ~/Closet, to be sorted.",
                "- Dormant -> ~/Archive/<year last changed>/, its folders kept (Put back on the Archive page works any time).",
                "A name already in the Closet gets \"(from Desktop)\" or the like. A file changed since the Look is left "
                "out and said -- Look again.",
                "The Closet card makes ~/Closet and ~/Closet/Junk Drawer, and can point Downloads at the Closet "
                "(~/.config/user-dirs.dirs -- the old one goes to the Trash, so Undo puts it back). Apps started after "
                "that save there. GNOME's screenshots can't be pointed; the screenshots query gathers the old ones."}},
            {"Hands off", {
                "\"Leave a folder alone...\" marks a folder -- your Scrivener folder, covers, workups. Nothing inside it "
                "is ever proposed: not by Tidy, not by Archive, not by Duplicates (a duplicate pair with either copy "
                "inside is left out whole). Forget takes the mark off. The mark lives in the catalog, by path: rename "
                "the folder and mark it again."}},
            {"Also in the report", {
                "Folders that need tidying, worst first (files arriving, kinds mixed, much unused in a year); the top "
                "of home, folder by folder; and drift -- folders your desktop points at that aren't there, and where "
                "new files land."}},
            {"Good to know", {
                "\"Unused\" is the later of last opened and last changed. If your home is mounted noatime, Rouster "
                "can't know last opened (Preflight says so) and goes by last change.",
                "Your own files only: hidden files, code in git repositories and caches are left out; a Scrivener "
                "project is one item. Nothing in the Closet is proposed again -- it waits there to be sorted."}},
        }});

    t.push_back({"projects", "Projects", "Organize",
        "Starts projects from templates, names files the project's way, and files them where they belong.",
        {
            {"Templates", {
                "A template is a plain folder in ~/.config/rouster/templates -- edit it in Files. Three starters "
                "(Design job, Book, gtkmm app) can be added in one undoable step. Placeholders {id}, {title} and "
                "{date} are filled in when a project is made."}},
            {"File into a project", {
                "Drop files on a project: each is renamed by the project's pattern (for example "
                "{id}_{title}_{stamp}_{part}.{ext}) and routed by type. Anything that doesn't fit is flagged, never moved."}},
            {"New version", {
                "Copies a file under a fresh stamp and moves the old one into versions/. The copy shares storage "
                "where the filesystem can."}},
        }});

    t.push_back({"rules", "Rules", "Organize",
        "Hazel-style rules on a watched folder -- Downloads is the inbox.",
        {
            {"How to use it", {
                "A rule looks at one folder: name, kinds and age decide what matches; the action moves it (renamed, "
                "into dated folders), sends it to the Trash, archives it, or files it into a project. First match wins."}},
            {"Preview first", {
                "A rule turns on only from its own preview. Run rules now shows every match before anything moves, "
                "and the whole run is one Activity step."}},
            {"Watch", {
                "With Watch on, enabled rules run every 10 minutes -- only while Rouster is open."}},
        }});

    t.push_back({"archive", "Archive", "Organize",
        "Moves files you haven't touched in a long time to ~/Archive, keeping their folders.",
        {
            {"How to use it", {
                "Choose how old (6 months to 5 years; a year by default) and press Look. Old files move to "
                "~/Archive/<year>/<where they were>. A folder that's old all the way through goes as one."}},
            {"Getting things back", {
                "In the archive lists what Rouster put there; Put back sends any of it home -- at any time, not only "
                "through Undo."}},
            {"Good to know", {
                "Age is the last change (mtime). Dev projects, repos, hidden files and fixes/ are never archived."}},
        }});

    // ── Safety ──────────────────────────────────────────────────────────────
    t.push_back({"preflight", "Preflight", "Safety",
        "The checklist that must be clear before Rouster cleans.",
        {
            {"The checks", {
                "- Filesystem -- green when home is btrfs on its own subvolume, so it can be snapshotted on its own.",
                "- Snapshot -- green when one of Rouster's snapshots is under a day old.",
                "- Pika Backup -- green when Pika ran in the last 7 days. Off btrfs, Pika is the only net, so a stale one is red.",
                "- Free space -- red under 2 GB, a note under 10 GB or 10%.",
                "- Last opened -- whether your home keeps when files were last opened (relatime does); a note if "
                "it's mounted noatime, when Tidy can only go by last change. Never stops anything."}},
            {"Red stops, amber notes", {
                "A red check turns off every act button until it clears, and the button says why. Amber never blocks."}},
            {"Take snapshot", {
                "Makes a read-only btrfs snapshot of the subvolume holding your home, in "
                "/home/.rouster-snapshots/home-<date>-<time> (the time is UTC). It asks for your password. "
                "Rouster keeps its newest 5 and removes older ones it made -- never anything else.",
                "To get a file back, copy it out of the snapshot in Files; snapshots can't be changed.",
                "Needs the root helper -- see The root helper."}},
        }});

    t.push_back({"helper", "The root helper", "Safety",
        "The one small program that runs as root, so Rouster itself never has to.",
        {
            {"Installing it", {
                "From the rouster folder (asks for sudo):",
                "$ ./install.sh --helper",
                "It goes to /usr/local/libexec/rouster/rouster-helper, with its polkit action in "
                "/usr/share/polkit-1/actions. To remove both:",
                "$ ./uninstall.sh --helper"}},
            {"What it can do", {
                "Only what's on its list: take a snapshot; clear the package cache (dnf clean all, or apt-get clean); "
                "vacuum the journal (keep N days); trim (fstrim --all). No paths or commands are ever passed to it; "
                "whose home it is comes from who asked, not from Rouster. It never runs a shell."}},
            {"The password", {
                "GNOME asks for your password the first time; for a few minutes after, it doesn't ask again. "
                "Cancel the dialog and nothing runs."}},
            {"In Activity", {
                "Each root step is in Activity. Undo closes the step; nothing that ran as root is taken back -- a "
                "snapshot stays (Rouster's own expire after the newest 5). To remove one by hand:",
                "$ sudo btrfs subvolume delete /home/.rouster-snapshots/<name>"}},
        }});

    t.push_back({"profile", "Profile backup", "Safety",
        "Saves how your desktop is set up, and puts it back on a new install.",
        {
            {"Back up", {
                "Look gathers your set-up: dotfiles, GNOME settings, the extension list, autostart, user units, "
                "fonts, and the package and Flatpak lists. Back up makes one folder in ~/Archive/Profile/ with "
                "PROFILE.txt saying where each file came from and how to put it back by hand."}},
            {"Secrets (optional)", {
                "Include them, encrypted adds ~/.ssh, ~/.gnupg and your keyrings, encrypted with a passphrase you "
                "type twice. Rouster never writes the passphrase down -- keep it somewhere safe."}},
            {"Restore", {
                "Restore... compares a backup with your home now: same, missing, differs. Missing files come back; "
                "a file that differs is shown as a diff and is unchecked -- if you check it, the one there now goes "
                "to the Trash first.",
                "GNOME settings: Rouster saves your settings now, then loads the backup's; Undo puts them back exactly.",
                "Secrets and packages are lines to paste into a terminal -- Rouster never writes into ~/.ssh or ~/.gnupg itself."}},
        }});

    t.push_back({"activity", "Activity and Undo", "Safety",
        "Everything Rouster has done, newest first -- and the way back.",
        {
            {"How to use it", {
                "Each act is one step. Open a step to see every item: what moved, from where, to where. "
                "Undo takes back the newest step that isn't undone yet; press it again for the one before."}},
            {"When something can't go back", {
                "If something new sits where a file came from, or the file was changed or moved since, that item "
                "stays where it is and Activity says why. Rouster never overwrites to undo."}},
            {"After a crash", {
                "If Rouster stopped mid-step, it looks at where things are when it starts again and says what it "
                "found at the top of Activity."}},
        }});

    t.push_back({"trying", "Trying it on a test home", "Safety",
        "Run Rouster against a made-up home folder before your real one.",
        {
            {"", {
                "The selftest can build a small fake home with caches, duplicates, projects and old files:",
                "$ ./build/rouster_selftest --make-fixture /var/tmp/rfx",
                "$ ROUSTER_HOME=/var/tmp/rfx ./build/rouster",
                "Everything Rouster does then happens inside that folder. Its GNOME settings are its own, and "
                "a snapshot is only a dry run -- the helper says what it would do and does nothing.",
                "Add ~/.config/pika-backup/history.json inside the fixture or Preflight's Pika check stays red."}},
        }});

    t.push_back({"files", "Where Rouster keeps things", "Safety",
        "Rouster's own files -- all inside your home except the helper.",
        {
            {"", {
                "- ~/.local/share/rouster -- the Activity record (manifest.jsonl), saved settings for Undo, rules, known names.",
                "- ~/.local/share/rouster/catalog.db -- the catalog: Rouster's memory of your files (names, sizes, "
                "when each was first seen, last opened, last changed, moved, gone). Never their contents. "
                "Delete it and the next Smart Scan of home starts it again -- only the history and your decisions are lost.",
                "- ~/.config/rouster/templates -- project templates.",
                "- ~/Archive -- archived files and profile backups.",
                "- ~/.local/share/Trash -- what Rouster (and you) sent to the Trash.",
                "- /home/.rouster-snapshots -- snapshots (root-owned, read-only).",
                "Uninstalling Rouster removes the app; --purge also removes its settings. Your archive, backups and "
                "snapshots are yours and stay."}},
        }});
    return t;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string all_text(const Topic& t) {
    std::string s = t.title + "\n" + t.lead + "\n";
    for (const auto& sec : t.sections) {
        s += sec.heading + "\n";
        for (const auto& p : sec.paras) s += p + "\n";
    }
    return lower(s);
}

}  // namespace

const std::vector<Topic>& topics() {
    static const std::vector<Topic> t = build();
    return t;
}

const Topic* find(const std::string& id) {
    for (const auto& t : topics()) if (t.id == id) return &t;
    return nullptr;
}

std::vector<const Topic*> search(const std::string& query) {
    std::vector<std::string> words;
    std::istringstream in(lower(query));
    for (std::string w; in >> w;) words.push_back(w);
    std::vector<const Topic*> out;
    for (const auto& t : topics()) {
        const std::string text = all_text(t);
        if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return text.find(w) != std::string::npos; }))
            out.push_back(&t);
    }
    return out;
}

}  // namespace rouster::core::help
