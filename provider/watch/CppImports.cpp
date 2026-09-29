#include "CppImports.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <sstream>

namespace rgv::watch {
namespace {

std::string trim(std::string s) {
    const auto ws = " \t\r\n";
    const auto b  = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string dirname_of(const std::string& rel) {
    const auto slash = rel.rfind('/');
    return slash == std::string::npos ? std::string{} : rel.substr(0, slash);
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string              cur;
    std::istringstream       in(path);
    while (std::getline(in, cur, '/')) {
        if (!cur.empty()) parts.push_back(cur);
    }
    return parts;
}

// `.` and `..` resolved away, empty when it climbs out of the repository -- which is
// also how an absolute path from outside the tree falls out.
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    for (const auto& p : split_path(path)) {
        if (p == ".") continue;
        if (p == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
            continue;
        }
        parts.push_back(p);
    }
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += '/';
        out += p;
    }
    return out;
}

// One physical line with its comments blanked and its string literals kept.
//
// Both halves matter, and for opposite reasons. An include inside a comment is not an
// include; an include inside a string literal is not one either -- a test that writes a
// C++ fixture to disk is full of them -- and the path we are after IS a string, so the
// strings cannot simply be blanked the way the Python reader blanks them. Block
// comments and raw strings carry across lines, which is the whole reason this is
// stateful.
struct Scrubber {
    bool        in_block = false;
    bool        in_raw   = false;   // inside an R"delim( ... )delim"
    std::string raw_delim;          // its delimiter, which is usually empty

    std::string scrub(const std::string& raw) {
        std::string out;
        std::size_t i = 0;
        while (i < raw.size()) {
            if (in_raw) {
                if (raw[i] == ')' && raw.compare(i + 1, raw_delim.size(), raw_delim) == 0 &&
                    i + 1 + raw_delim.size() < raw.size() &&
                    raw[i + 1 + raw_delim.size()] == '"') {
                    i += 2 + raw_delim.size();
                    in_raw = false;
                    raw_delim.clear();
                } else {
                    ++i;
                }
                continue;
            }
            if (in_block) {
                if (raw[i] == '*' && i + 1 < raw.size() && raw[i + 1] == '/') {
                    in_block = false;
                    i += 2;
                } else {
                    ++i;
                }
                continue;
            }
            const char c = raw[i];
            if (c == '/' && i + 1 < raw.size() && raw[i + 1] == '/') break;
            if (c == '/' && i + 1 < raw.size() && raw[i + 1] == '*') {
                in_block = true;
                i += 2;
                continue;
            }
            if (c == '"' && starts_raw(raw, i)) {
                const auto open = raw.find('(', i + 1);
                if (open != std::string::npos) {
                    raw_delim = raw.substr(i + 1, open - i - 1);
                    in_raw    = true;
                    out.pop_back();   // the R already went out
                    i = open + 1;
                    continue;
                }
            }
            if (c == '"' || (c == '\'' && !(i > 0 && ident_char(raw[i - 1])))) {
                // A literal, copied through: `'` after an identifier character is a
                // digit separator (`1'000`), not the start of one.
                out.push_back(c);
                ++i;
                while (i < raw.size()) {
                    const char d = raw[i];
                    out.push_back(d);
                    ++i;
                    if (d == '\\' && i < raw.size()) {
                        out.push_back(raw[i]);
                        ++i;
                        continue;
                    }
                    if (d == c) break;
                }
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return out;
    }

private:
    static bool starts_raw(const std::string& raw, std::size_t quote) {
        if (quote == 0 || raw[quote - 1] != 'R') return false;
        if (quote == 1) return true;
        const char before = raw[quote - 2];
        // The encoding prefixes, and nothing else: `FOOR"` is an identifier.
        return !ident_char(before) || before == 'u' || before == 'U' || before == 'L' ||
               before == '8';
    }
};

bool word_at(const std::string& s, std::size_t at, const char* word) {
    const std::size_t n = std::char_traits<char>::length(word);
    if (s.compare(at, n, word) != 0) return false;
    if (at > 0 && ident_char(s[at - 1])) return false;
    return at + n >= s.size() || !ident_char(s[at + n]);
}

// `#include "path"` or `#include <path>` on an already-joined logical line.
bool read_directive(const std::string& line, std::string* path, bool* angled) {
    const std::string t = trim(line);
    if (t.empty() || t[0] != '#') return false;
    std::size_t i = 1;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    if (!word_at(t, i, "include")) return false;
    i += 7;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    if (i >= t.size()) return false;
    const char open  = t[i];
    const char close = open == '<' ? '>' : (open == '"' ? '"' : '\0');
    if (close == '\0') return false;   // `#include MACRO`: not a path, not a guess
    const auto end = t.find(close, i + 1);
    if (end == std::string::npos || end == i + 1) return false;
    *path   = t.substr(i + 1, end - i - 1);
    *angled = open == '<';
    return true;
}

} // namespace

std::vector<CppInclude> parse_cpp_includes(const std::string& text) {
    std::vector<CppInclude> out;
    std::istringstream      in(text);
    std::string             raw;
    Scrubber                scrub;

    std::string logical;
    std::string snippet;
    int         start   = 0;
    int         lineno  = 0;
    bool        joining = false;

    while (std::getline(in, raw)) {
        ++lineno;
        std::string code      = trim(scrub.scrub(raw));
        bool        continued = false;
        if (!code.empty() && code.back() == '\\') {
            continued = true;
            code      = trim(code.substr(0, code.size() - 1));
        }
        if (joining) {
            logical += ' ' + code;
        } else {
            start   = lineno;
            snippet = code;
            logical = code;
        }
        joining = continued;
        if (joining) continue;

        std::string path;
        bool        angled = false;
        if (read_directive(logical, &path, &angled)) {
            out.push_back(CppInclude{path, angled, start, snippet});
        }
        logical.clear();
    }
    return out;
}

std::vector<std::string> cpp_source_roots(const std::map<std::string, bool>& entries) {
    std::vector<std::string> includes;
    std::vector<std::string> srcs;
    for (const auto& [rel, is_dir] : entries) {
        if (!is_dir) continue;
        const std::string name = rel.substr(rel.rfind('/') + 1);
        if (name == "include" || name == "inc") includes.push_back(rel);
        else if (name == "src") srcs.push_back(rel);
    }
    // The repository root first, then the header trees, then the source trees: a
    // repository that ships headers means the `include` copy when both exist. `entries`
    // is sorted, so the order within each group is stable and so is the pick an
    // ambiguous include lands on.
    std::vector<std::string> roots{""};
    roots.insert(roots.end(), includes.begin(), includes.end());
    roots.insert(roots.end(), srcs.begin(), srcs.end());
    return roots;
}

std::string resolve_cpp_include(const std::string& from_rel, const CppInclude& inc,
                                const std::set<std::string>&    files,
                                const std::vector<std::string>& roots, bool* ambiguous) {
    if (ambiguous) *ambiguous = false;
    if (inc.path.empty()) return {};

    if (!inc.angled) {
        // The standard looks beside the including file first, so finding it there
        // settles the question -- `foo.cpp` including `foo.h` is not a guess merely
        // because some other directory also has a `foo.h`.
        const std::string dir   = dirname_of(from_rel);
        const std::string local = normalize(dir.empty() ? inc.path : dir + "/" + inc.path);
        if (!local.empty() && files.count(local)) return local;
    }

    std::string hit;
    for (const auto& r : roots) {
        const std::string cand = normalize(r.empty() ? inc.path : r + "/" + inc.path);
        if (cand.empty() || !files.count(cand)) continue;
        if (hit.empty()) hit = cand;
        else if (cand != hit && ambiguous) *ambiguous = true;
    }
    return hit;
}

std::vector<FileImport> resolve_cpp_includes(const std::string&              from_rel,
                                             const std::vector<CppInclude>&  includes,
                                             const std::set<std::string>&    files,
                                             const std::vector<std::string>& roots) {
    std::vector<FileImport> out;
    for (const auto& inc : includes) {
        bool              ambiguous = false;
        const std::string to        = resolve_cpp_include(from_rel, inc, files, roots, &ambiguous);
        if (to.empty() || to == from_rel) continue;
        bool seen = false;
        for (const auto& f : out) {
            if (f.to == to) { seen = true; break; }
        }
        if (seen) continue;
        out.push_back(FileImport{to, inc.line, inc.snippet, ambiguous});
    }
    return out;
}

// -- build targets -------------------------------------------------------------

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Words that decorate a target rather than build it. A keyword that slipped through
// would name no file and fall out at ownership time anyway; the list is what keeps
// `EXCLUDE_FROM_ALL` from being reported as a source the scanner could not place.
bool target_keyword(const std::string& token) {
    static const std::set<std::string> kw{
        "STATIC", "SHARED", "MODULE", "OBJECT",         "INTERFACE",      "UNKNOWN",
        "WIN32",  "GLOBAL", "MANUAL", "MACOSX_BUNDLE",  "EXCLUDE_FROM_ALL"};
    return kw.count(upper(token)) > 0;
}

// A CMake line with its `#` comment removed. A `#` inside a quoted argument is not one.
std::string strip_cmake_comment(const std::string& raw) {
    std::string out;
    bool        quoted = false;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (quoted && c == '\\' && i + 1 < raw.size()) {
            out.push_back(c);
            out.push_back(raw[++i]);
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (c == '#' && !quoted) break;
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> cmake_arguments(const std::string& body) {
    std::vector<std::string> out;
    std::string              cur;
    bool                     quoted = false;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (quoted && c == '\\' && i + 1 < body.size()) {
            cur.push_back(body[++i]);
            continue;
        }
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The one variable a scanner can expand without being CMake is the directory the file
// is in. A source list is evidence; anything else in it -- a variable holding generated
// files, a generator expression -- is dropped rather than guessed at.
std::string expand_source(std::string token, const std::string& dir_rel) {
    if (token.empty() || token[0] == '/') return {};
    bool expanded = false;
    for (const char* var : {"${CMAKE_CURRENT_SOURCE_DIR}", "${CMAKE_CURRENT_LIST_DIR}"}) {
        for (;;) {
            const auto at = token.find(var);
            if (at == std::string::npos) break;
            token    = token.substr(0, at) + dir_rel + token.substr(at + std::strlen(var));
            expanded = true;
        }
    }
    if (token.find("${") != std::string::npos || token.find("$<") != std::string::npos) return {};
    // An expanded path already starts at the repository root; a bare one is relative to
    // the file that wrote it.
    return normalize(expanded || dir_rel.empty() ? token : dir_rel + "/" + token);
}

} // namespace

std::vector<CppTarget> parse_cmake_targets(const std::string& text, const std::string& dir_rel) {
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string        raw;
        while (std::getline(in, raw)) lines.push_back(strip_cmake_comment(raw));
    }

    std::vector<CppTarget> out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        for (std::size_t j = 0; j < line.size(); ++j) {
            const bool exe = word_at(line, j, "add_executable");
            const bool lib = word_at(line, j, "add_library");
            if (!exe && !lib) continue;
            std::size_t k = j + (exe ? 14 : 11);
            while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
            if (k >= line.size() || line[k] != '(') continue;

            // The command runs until its parentheses balance, however many lines that
            // takes -- a source list is usually one file per line.
            std::string body;
            int         depth = 0;
            std::size_t end   = i;
            for (std::size_t n = i; n < lines.size(); ++n) {
                const std::string& cur = lines[n];
                for (std::size_t p = (n == i ? k : 0); p < cur.size(); ++p) {
                    if (cur[p] == '(') ++depth;
                    else if (cur[p] == ')') --depth;
                    if (depth == 0) { end = n; break; }
                    if (depth > 0 && !(n == i && p == k)) body.push_back(cur[p]);
                }
                if (depth == 0) { end = n; break; }
                body.push_back(' ');
            }

            const auto args = cmake_arguments(body);
            if (args.empty()) break;

            CppTarget t;
            t.name    = args[0];
            t.kind    = exe ? "executable" : "library";
            t.line    = static_cast<int>(i) + 1;
            t.snippet = trim(lines[i]);
            bool alias = false;
            for (std::size_t a = 1; a < args.size(); ++a) {
                const std::string word = upper(args[a]);
                // An alias or an imported target is another name for something that
                // already exists, or for something built elsewhere. Neither is a unit
                // of this repository.
                if (word == "ALIAS" || word == "IMPORTED") { alias = true; break; }
                if (target_keyword(args[a])) continue;
                const std::string src = expand_source(args[a], dir_rel);
                if (src.empty()) continue;
                if (std::find(t.sources.begin(), t.sources.end(), src) == t.sources.end()) {
                    t.sources.push_back(src);
                }
            }
            if (!alias && !t.name.empty()) out.push_back(std::move(t));
            i = end;
            break;
        }
    }
    return out;
}

// -- ownership ------------------------------------------------------------------

namespace {

int depth_of(const std::string& dir) {
    return dir.empty() ? 0 : static_cast<int>(split_path(dir).size());
}

// The deepest directory that holds every one of `files`.
std::string common_dir(const std::vector<std::string>& files) {
    bool                     first = true;
    std::vector<std::string> common;
    for (const auto& f : files) {
        std::vector<std::string> parts = split_path(dirname_of(f));
        if (first) {
            common = std::move(parts);
            first  = false;
            continue;
        }
        std::size_t n = 0;
        while (n < common.size() && n < parts.size() && common[n] == parts[n]) ++n;
        common.resize(n);
    }
    std::string out;
    for (const auto& p : common) {
        if (!out.empty()) out += '/';
        out += p;
    }
    return out;
}

bool under(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return !rel.empty();
    return rel.size() > dir.size() && rel.compare(0, dir.size(), dir) == 0 && rel[dir.size()] == '/';
}

} // namespace

const std::string* CppOwnership::owner_of(const std::string& rel) const {
    auto it = owners.find(rel);
    return it == owners.end() || it->second.empty() ? nullptr : &it->second.front();
}

CppOwnership cpp_ownership(const std::vector<CppTarget>&                          targets,
                           const std::map<std::string, bool>&                     entries,
                           const std::map<std::string, std::vector<std::string>>& includes) {
    CppOwnership own;

    std::set<std::string> files;
    for (const auto& [rel, is_dir] : entries) {
        if (!is_dir) files.insert(rel);
    }

    // Where each target's listed sources sit, which is how specific its claim on a file
    // is: the provider binary, all of whose sources are one directory, outranks the test
    // binary that compiles two of them from across the repository.
    std::map<std::string, std::string>              listed_home;
    std::map<std::string, std::vector<std::string>> listed;
    std::map<std::string, std::string>              kind;
    for (const auto& t : targets) {
        if (listed.count(t.name)) continue;   // one declaration per name; the first wins
        std::vector<std::string> here;
        for (const auto& s : t.sources) {
            if (files.count(s)) here.push_back(s);
        }
        listed_home[t.name] = common_dir(here);
        kind[t.name]        = t.kind;
        listed[t.name]      = std::move(here);
    }

    // tier 0: listed in the target. tier 1: a header the target's sources reach.
    std::map<std::string, std::map<std::string, int>> claims;   // file -> target -> tier
    auto claim = [&](const std::string& file, const std::string& target, int tier) {
        auto [it, fresh] = claims[file].try_emplace(target, tier);
        if (!fresh) it->second = std::min(it->second, tier);
    };

    for (const auto& [name, sources] : listed) {
        std::set<std::string>   seen(sources.begin(), sources.end());
        std::deque<std::string> queue;
        for (const auto& s : sources) {
            claim(s, name, 0);
            queue.push_back(s);
        }
        while (!queue.empty()) {
            const std::string cur = queue.front();
            queue.pop_front();
            auto it = includes.find(cur);
            if (it == includes.end()) continue;
            for (const auto& next : it->second) {
                if (!files.count(next) || !seen.insert(next).second) continue;
                claim(next, name, 1);
                queue.push_back(next);
            }
        }
    }

    // One parent, every other claim an `owns` edge (contract 3.1).
    for (auto& [file, by_target] : claims) {
        std::vector<std::string> ranked;
        for (const auto& [name, tier] : by_target) ranked.push_back(name);
        std::sort(ranked.begin(), ranked.end(), [&](const std::string& a, const std::string& b) {
            if (by_target.at(a) != by_target.at(b)) return by_target.at(a) < by_target.at(b);
            const bool lib_a = kind.at(a) == "library", lib_b = kind.at(b) == "library";
            if (lib_a != lib_b) return lib_a;
            const int da = depth_of(listed_home.at(a)), db = depth_of(listed_home.at(b));
            if (da != db) return da > db;
            return a < b;
        });
        for (const auto& name : ranked) {
            if (by_target.at(name) == 0) own.listed[file].push_back(name);
        }
        own.owners[file] = std::move(ranked);
    }

    std::map<std::string, std::vector<std::string>> mine;   // target -> files it parents
    for (const auto& [file, ranked] : own.owners) mine[ranked.front()].push_back(file);
    for (const auto& [name, held] : mine) {
        own.names.push_back(name);
        own.home[name] = common_dir(held);
    }

    // Which node moves onto the target: the outermost directory whose owned files all
    // belong to it, or the file itself where a directory is split between targets.
    // Moving the directory is what keeps the filesystem view's tree recognisable -- a
    // build target is not drawn there, so everything under it is drawn at the target's
    // own directory, and a directory that moves lands one level from where it was
    // instead of every file in it landing at the top.
    std::map<std::string, std::set<std::string>> under_dir;   // dir -> owners beneath it
    for (const auto& [file, ranked] : own.owners) {
        std::string dir = file;
        for (;;) {
            const auto slash = dir.rfind('/');
            if (slash == std::string::npos) break;
            dir = dir.substr(0, slash);
            under_dir[dir].insert(ranked.front());
        }
    }

    auto moved_ancestor = [&](const std::string& rel) {
        std::string dir = rel;
        for (;;) {
            const auto slash = dir.rfind('/');
            if (slash == std::string::npos) return false;
            dir = dir.substr(0, slash);
            if (own.parent.count(dir)) return true;
        }
    };

    // Ascending, so a directory is considered before anything inside it and the
    // outermost single-owner directory is the one that moves.
    for (const auto& [dir, owners_here] : under_dir) {
        if (owners_here.size() != 1 || moved_ancestor(dir)) continue;
        const std::string& target = *owners_here.begin();
        // Strictly below the target's home, or the target would end up inside the
        // directory that hangs off it.
        if (under(own.home.at(target), dir)) own.parent[dir] = target;
    }
    for (const auto& [file, ranked] : own.owners) {
        if (!moved_ancestor(file)) own.parent[file] = ranked.front();
    }
    return own;
}

} // namespace rgv::watch
