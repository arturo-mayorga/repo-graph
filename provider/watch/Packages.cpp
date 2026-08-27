#include "Packages.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace rgv::watch {
namespace {

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

std::string trim(std::string s) {
    const auto ws = " \t\r\n";
    const auto b  = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Where a dependency is declared, so the inspector can show provenance. The manifest is
// re-scanned as text rather than tracked through the parser: a JSON reader throws line
// numbers away, and "which line mentions this name" is the question a human is asking.
void locate(const std::string& text, const std::string& needle, Dependency& d) {
    std::istringstream in(text);
    std::string        line;
    int                n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.find(needle) == std::string::npos) continue;
        d.line    = n;
        d.snippet = trim(line);
        if (!d.snippet.empty() && d.snippet.back() == ',') d.snippet.pop_back();
        return;
    }
}

// -- npm ----------------------------------------------------------------------

bool read_package_json(const fs::path& root, const std::string& rel, Package& out) {
    const std::string manifest = rel.empty() ? "package.json" : rel + "/package.json";
    const std::string text     = read_text(root / manifest);
    if (text.empty()) return false;

    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const json::exception&) {
        return false;   // a manifest we cannot read is not a package we can describe
    }
    if (!j.is_object()) return false;

    // A package.json with no name is a config file (an npm workspace root often is one),
    // not something anything can depend on.
    auto name = j.find("name");
    if (name == j.end() || !name->is_string() || name->get<std::string>().empty()) return false;

    out.name     = name->get<std::string>();
    out.rel      = rel;
    out.manifest = manifest;
    out.provider = "npm";
    if (auto v = j.find("version"); v != j.end() && v->is_string()) out.version = *v;

    const std::pair<const char*, bool> blocks[] = {
        {"dependencies", false}, {"peerDependencies", false}, {"devDependencies", true}};
    for (const auto& [key, is_dev] : blocks) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_object()) continue;
        for (const auto& [dep, spec] : it->items()) {
            if (dep.empty()) continue;
            Dependency d;
            d.name     = dep;
            d.artifact = manifest;
            d.dev      = is_dev;
            locate(text, "\"" + dep + "\"", d);
            out.deps.push_back(std::move(d));
        }
    }
    return true;
}

// -- python -------------------------------------------------------------------
//
// A targeted read of pyproject.toml rather than a TOML parser: the two tables that
// matter are `[project]` and `[tool.poetry...]`, and both state the name and the
// dependencies in forms a few lines of string handling get right. Anything more exotic
// -- dynamic metadata, PDM/hatch plugins, environment markers that change the set --
// is out of reach here and should be, because guessing would produce edges that look
// exact and are not.

// "requests >= 2.1", "flask[async]==3", "django; python_version<'3.9'" -> the name.
std::string requirement_name(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return {};
    std::size_t end = 0;
    while (end < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '-' || s[end] == '_' ||
            s[end] == '.')) {
        ++end;
    }
    return s.substr(0, end);
}

std::string toml_string(const std::string& line) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) return v.substr(1, v.size() - 2);
    return v;
}

bool read_pyproject(const fs::path& root, const std::string& rel, Package& out) {
    const std::string manifest = rel.empty() ? "pyproject.toml" : rel + "/pyproject.toml";
    const std::string text     = read_text(root / manifest);
    if (text.empty()) return false;

    std::istringstream in(text);
    std::string        line;
    std::string        table;
    int                lineno = 0;

    auto push = [&](const std::string& raw, bool dev, int at) {
        const std::string n = requirement_name(raw);
        if (n.empty() || normalize(n) == "python") return;   // the interpreter is not a dep
        Dependency d;
        d.name     = n;
        d.artifact = manifest;
        d.dev      = dev;
        d.line     = at;
        d.snippet  = trim(raw);
        out.deps.push_back(std::move(d));
    };

    // An inline array is scanned character by character rather than split on the first
    // `]`, because a requirement may carry extras -- `flask[async]` closes a bracket
    // that is not the array's, and truncating there silently drops every dependency
    // after the first one that uses them.
    bool        in_array = false;
    int         depth    = 0;
    bool        in_quote = false;
    char        quote    = 0;
    std::string item;

    auto consume = [&](const std::string& text_line, int at) {
        for (char c : text_line) {
            if (in_quote) {
                if (c == quote) {
                    in_quote = false;
                    push(item, false, at);   // the line the requirement ends on
                    item.clear();
                } else {
                    item.push_back(c);
                }
                continue;
            }
            if (c == '"' || c == '\'') { in_quote = true; quote = c; item.clear(); continue; }
            if (c == '[') ++depth;
            else if (c == ']') {
                if (--depth <= 0) { in_array = false; depth = 0; return; }
            } else if (c == '#') {
                return;   // rest of the line is a comment
            }
        }
    };

    while (std::getline(in, line)) {
        ++lineno;
        const std::string t = trim(line);

        if (in_array) { consume(t, lineno); continue; }
        if (t.empty() || t[0] == '#') continue;

        if (t.front() == '[' && t.back() == ']') {
            table = t.substr(1, t.size() - 2);
            continue;
        }

        // `dependencies = ["a", "b"]`, on one line or spread over many.
        if ((table == "project" || table == "tool.poetry") &&
            t.rfind("dependencies", 0) == 0 && t.find('[') != std::string::npos) {
            in_array = true;
            depth    = 0;
            item.clear();
            consume(t.substr(t.find('[')), lineno);
            continue;
        }

        if ((table == "project" || table == "tool.poetry") && t.rfind("name", 0) == 0) {
            const std::string v = toml_string(t);
            if (!v.empty() && out.name.empty()) out.name = v;
            continue;
        }
        if ((table == "project" || table == "tool.poetry") && t.rfind("version", 0) == 0) {
            if (out.version.empty()) out.version = toml_string(t);
            continue;
        }

        // Poetry states dependencies as a table rather than an array.
        if (table == "tool.poetry.dependencies" || table == "tool.poetry.dev-dependencies" ||
            table == "tool.poetry.group.dev.dependencies") {
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            push(trim(t.substr(0, eq)), table != "tool.poetry.dependencies", lineno);
        }
    }

    if (out.name.empty()) return false;
    out.rel      = rel;
    out.manifest = manifest;
    out.provider = "python";
    return true;
}

} // namespace

std::string normalize(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '_' || c == '.') c = '-';
    }
    return s;
}

std::vector<Package> scan_packages(const fs::path&                    root,
                                   const std::map<std::string, bool>& entries) {
    // Directories to consider: every directory in the walk, plus the repo root itself,
    // which is a package in the very common single-package case.
    std::vector<std::string> dirs{""};
    for (const auto& [rel, is_dir] : entries) {
        if (is_dir) dirs.push_back(rel);
    }

    std::vector<Package> out;
    for (const auto& rel : dirs) {
        Package p;
        if (read_package_json(root, rel, p) || read_pyproject(root, rel, p)) {
            p.id = "pkg:" + p.name;
            out.push_back(std::move(p));
        }
    }

    // Two packages claiming one name would produce edges pointing at whichever was seen
    // last. Keep the first by path order and drop the rest: a wrong edge is worse than a
    // missing one, and this is rare enough to be a real problem when it happens.
    std::sort(out.begin(), out.end(),
              [](const Package& a, const Package& b) { return a.rel < b.rel; });
    std::vector<Package> unique;
    for (auto& p : out) {
        const bool clash = std::any_of(unique.begin(), unique.end(), [&](const Package& q) {
            return normalize(q.name) == normalize(p.name);
        });
        if (!clash) unique.push_back(std::move(p));
    }
    return unique;
}

} // namespace rgv::watch
