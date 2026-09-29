#include "Ignore.h"

#include <sstream>
#include <unordered_set>

namespace rgv::watch {
namespace {

// The floor, for a repository that says nothing. Names, not prefixes: `build` is here
// and `builder` is not, and that distinction is the whole point (see Ignore.h).
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

// `*` and `?` stop at a separator, which is what makes `src/*.o` different from
// `src/**/*.o` -- and `**` is not implemented, so the difference is real.
bool glob_match(const std::string& pat, const std::string& s) {
    std::size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' ? s[i] != '/' : pat[p] == s[i])) {
            ++p;
            ++i;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos && s[mark] != '/') {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// A pattern this matcher cannot read honestly is not approximated. Returning false
// drops it, and the walk then descends where git would not -- the safe direction.
bool supported(const std::string& glob) {
    if (glob.empty()) return false;
    if (glob.find('\\') != std::string::npos) return false;   // escapes
    if (glob.find('[') != std::string::npos) return false;    // character classes
    if (glob.find(']') != std::string::npos) return false;
    return glob.find("**") == std::string::npos;              // leading `**/` already stripped
}

std::string trim_trailing_spaces(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace

void Ignores::add_gitignore(const std::string& text) {
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        line = trim_trailing_spaces(line);
        if (line.empty() || line[0] == '#') continue;

        Rule r;
        if (line[0] == '!') {
            r.negate = true;
            line.erase(0, 1);
        }
        if (!line.empty() && line.back() == '/') {
            r.dir_only = true;
            line.pop_back();
        }
        // Anchored is git's rule: a slash anywhere but the end ties the pattern to the
        // repository root, so `doc/frotz/` is not `a/doc/frotz/`.
        if (!line.empty() && line[0] == '/') {
            r.anchored = true;
            line.erase(0, 1);
        } else if (line.rfind("**/", 0) == 0) {
            line.erase(0, 3);   // "match at any depth" is what an unanchored name means
        } else if (line.find('/') != std::string::npos) {
            r.anchored = true;
        }

        if (!supported(line)) continue;
        r.glob = std::move(line);
        rules_.push_back(std::move(r));
    }
}

bool Ignores::skips(const std::string& rel, bool is_dir) const {
    const std::string name = base_name(rel);
    if (name.empty()) return true;
    // Ahead of the patterns and not overridable by them. These are skipped for what
    // they cost to walk and watch, and a repository that commits its `node_modules`
    // has not thereby made it part of its architecture.
    if (built_in().count(name) > 0) return true;
    if (name.size() > 1 && name[0] == '.') return true;

    // Last match wins -- the only reason `!` means anything.
    bool ignored = false;
    for (const auto& r : rules_) {
        if (r.dir_only && !is_dir) continue;
        if (glob_match(r.glob, r.anchored ? rel : name)) ignored = !r.negate;
    }
    return ignored;
}

} // namespace rgv::watch
