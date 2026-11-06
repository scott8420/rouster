#include "core/Settings.hpp"

#include <filesystem>
#include <fstream>

namespace rouster::core {

bool save_own(const std::string& data_dir, const std::string& name, const std::string& text) {
    if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') return false;
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    const std::string file = data_dir + "/" + name;
    const std::string tmp = file + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << text;
        if (!o) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

}  // namespace rouster::core
