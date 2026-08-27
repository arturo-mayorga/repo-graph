#include "rgv/config/Settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace rgv::config {
namespace fs = std::filesystem;

namespace {

const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

} // namespace

void Settings::sanitize() {
    auto fix = [](float& v) {
        if (!std::isfinite(v)) v = 1.0f;
        v = std::clamp(v, kMinTextScale, kMaxTextScale);
    };
    fix(ui_text_scale);
    fix(graph_text_scale);
}

fs::path settings_path() {
    if (const char* xdg = env_or_null("XDG_CONFIG_HOME")) return fs::path(xdg) / "rgv" / "settings.json";
    if (const char* home = env_or_null("HOME")) return fs::path(home) / ".config" / "rgv" / "settings.json";
    return fs::path("rgv-settings.json");
}

Settings load(const fs::path& path) {
    Settings s;
    std::error_code ec;
    if (!fs::exists(path, ec)) return s;

    std::ifstream in(path);
    if (!in) return s;

    try {
        nlohmann::json j;
        in >> j;
        // Files written before the two sizes were split carry a single `text_scale`.
        // Honour it for both rather than silently resetting someone's preference.
        if (auto it = j.find("text_scale"); it != j.end() && it->is_number()) {
            s.ui_text_scale = s.graph_text_scale = it->get<float>();
        }
        if (auto it = j.find("ui_text_scale"); it != j.end() && it->is_number()) {
            s.ui_text_scale = it->get<float>();
        }
        if (auto it = j.find("graph_text_scale"); it != j.end() && it->is_number()) {
            s.graph_text_scale = it->get<float>();
        }
    } catch (const std::exception&) {
        // A corrupted preferences file is not an error worth surfacing: fall back to
        // defaults and let the next save overwrite it.
        return Settings{};
    }
    s.sanitize();
    return s;
}

bool save(const Settings& s, const fs::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) return false;
    }

    nlohmann::json j;
    j["ui_text_scale"]    = s.ui_text_scale;
    j["graph_text_scale"] = s.graph_text_scale;

    // Write-then-rename: a crash mid-write must not destroy the existing settings.
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << '\n';
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace rgv::config
