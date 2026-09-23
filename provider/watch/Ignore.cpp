#include "Ignore.h"

#include <unordered_set>

namespace rgv::watch {
namespace {

const std::unordered_set<std::string>& built_in() {
    static const std::unordered_set<std::string> s{
        ".git",   ".hg",     ".svn",          "node_modules", "build",  "dist",
        "target", ".venv",   "venv",          "__pycache__",  ".cache", ".mypy_cache",
        ".idea",  ".vscode", ".pytest_cache", ".ruff_cache",  ".tox",   ".next"};
    return s;
}

std::string base_name(const std::string& rel) {
    const auto slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

} // namespace

void Ignores::add_gitignore(const std::string& text) { (void)text; }

bool Ignores::skips(const std::string& rel, bool is_dir) const {
    (void)is_dir;
    const std::string name = base_name(rel);
    if (name.empty()) return true;
    if (built_in().count(name) > 0) return true;
    if (name.size() > 1 && name[0] == '.') return true;
    return false;
}

} // namespace rgv::watch
