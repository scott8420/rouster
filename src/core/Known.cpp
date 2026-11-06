#include "core/Known.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace rouster::core {

std::vector<std::string> load_known(const std::string& file) {
    std::vector<std::string> out;
    std::ifstream in(file);
    if (!in) return out;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (e.is_string() && !e.get<std::string>().empty()) out.push_back(e.get<std::string>());
    return out;
}

bool save_known(const std::string& file, const std::vector<std::string>& names) {
    std::vector<std::string> v = names;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    const std::string tmp = file + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << nlohmann::json(v).dump(2) << "\n";
        if (!o) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

}  // namespace rouster::core
