#include "core/TidyLink.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace rouster::core::tidylink {

namespace {
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}
}  // namespace

std::vector<std::string> parse_hands_off(const std::string& home, const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || trim(line.substr(0, eq)) != "hands_off") continue;
        std::string p = trim(line.substr(eq + 1));
        if (p.rfind("~/", 0) == 0) p = p.substr(2);
        else if (!p.empty() && p[0] == '/') {
            if (p.size() <= home.size() + 1 || p.compare(0, home.size() + 1, home + "/") != 0) continue;   // outside home
            p = p.substr(home.size() + 1);
        }
        while (!p.empty() && p.back() == '/') p.pop_back();
        while (p.rfind("./", 0) == 0) p = p.substr(2);
        if (p.empty() || p == "." || p[0] == '/') continue;
        bool dots = false;
        std::istringstream parts(p);
        for (std::string c; std::getline(parts, c, '/');) if (c == "..") dots = true;
        if (dots) continue;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

std::vector<std::string> hands_off(const std::string& home) {
    std::ifstream in(home + "/" + kSettings);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_hands_off(home, ss.str());
}

bool is_mark(std::string_view name) { return name == ".tidy-files" || name == ".tidy-project"; }

std::string guard(const door::Plan& plan) {
    const auto base = [](const std::string& p) {
        const auto s = p.find_last_of('/');
        return s == std::string::npos ? p : p.substr(s + 1);
    };
    for (const auto& a : plan.actions) {
        if (a.verb == door::Verb::Tag) return "a tag step -- user.tidy.* is Tidy's; Rouster never tags";
        if (a.verb == door::Verb::Root || a.verb == door::Verb::Settings) continue;
        for (const std::string* p : {&a.from, &a.to})
            if (!p->empty() && is_mark(base(*p)))
                return *p + " is Tidy's (" + base(*p) + ") -- Rouster never moves it on its own";
    }
    return {};
}

}  // namespace rouster::core::tidylink
