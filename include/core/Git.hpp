#pragma once
#include <atomic>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Git -- `git -C dir args...` as a child (s007a; its own file since s007b, when
// the door's Clone verb became the second caller). GTK-free.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::vcs {

struct Run {
    int         code = -1;        // exit status; -1 = did not run / killed
    std::string out, err;
    bool        timed_out = false;
};

// Prompts off (GIT_TERMINAL_PROMPT=0, askpass /bin/true, ssh BatchMode),
// optional locks off (GIT_OPTIONAL_LOCKS=0: `status` does not refresh the
// index), LC_ALL=C, no stdin. Killed after timeout_s, or when *cancel turns
// true. The user's git config still applies (url rewrites, credential helpers).
Run git(const std::string& dir, const std::vector<std::string>& args, int timeout_s,
        const std::atomic<bool>* cancel = nullptr);

}  // namespace rouster::core::vcs
