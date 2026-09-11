#include "PythonImports.h"

#include <algorithm>
#include <cctype>
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

bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

// One physical line with its string literals emptied and its comment removed, so that
// nothing inside a string can look like a statement and a `#` cannot hide one. Triple
// quotes carry across lines, which is the whole reason this is stateful.
struct Scrubber {
    std::string triple;   // the delimiter we are inside, or empty

    std::string scrub(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        std::size_t i = 0;
        while (i < raw.size()) {
            if (!triple.empty()) {
                if (raw.compare(i, 3, triple) == 0) { triple.clear(); i += 3; }
                else ++i;
                continue;
            }
            const char c = raw[i];
            if (c == '#') break;
            if (c == '"' || c == '\'') {
                const std::string tq(3, c);
                if (raw.compare(i, 3, tq) == 0) {
                    triple = tq;
                    i += 3;
                    continue;
                }
                // A single-line string: skip to its close, honouring escapes.
                out.push_back(' ');
                for (++i; i < raw.size() && raw[i] != c; ++i) {
                    if (raw[i] == '\\') ++i;
                }
                ++i;
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return out;
    }
};

// A keyword followed by a boundary, so `importer = 1` is not a statement.
bool keyword_at(const std::string& s, std::size_t at, const char* kw) {
    const std::size_t n = std::char_traits<char>::length(kw);
    if (s.compare(at, n, kw) != 0) return false;
    const std::size_t after = at + n;
    if (after >= s.size()) return false;
    const char c = s[after];
    return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == '.';
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// "a.b as c" -> "a.b"; "(x" -> "x".
std::string first_token(const std::string& s) {
    std::string t = trim(s);
    std::size_t b = 0;
    while (b < t.size() && (t[b] == '(' || t[b] == ')' || std::isspace(static_cast<unsigned char>(t[b])))) ++b;
    std::size_t e = b;
    while (e < t.size() && (is_ident(t[e]) || t[e] == '*')) ++e;
    return t.substr(b, e - b);
}

// "x as y" -> "y"; "" when there is no alias.
std::string alias_of(const std::string& part) {
    const std::string t  = trim(part);
    const auto        as = t.find(" as ");
    if (as == std::string::npos) return {};
    return first_token(t.substr(as + 4));
}

void parse_statement(const std::string& code, int line, const std::string& snippet,
                     std::vector<ImportStmt>& out) {
    const std::string t = trim(code);
    if (keyword_at(t, 0, "import")) {
        for (const auto& part : split(t.substr(6), ',')) {
            const std::string m = first_token(part);
            if (m.empty()) continue;
            // `import a.b as c` binds c; `import a.b` binds a.
            std::string       b     = alias_of(part);
            if (b.empty()) b = m.substr(0, m.find('.'));
            out.push_back(ImportStmt{m, 0, {}, {b}, line, snippet});
        }
        return;
    }
    if (!keyword_at(t, 0, "from")) return;

    std::size_t i = 4;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    int level = 0;
    while (i < t.size() && t[i] == '.') { ++level; ++i; }
    std::size_t e = i;
    while (e < t.size() && is_ident(t[e])) ++e;
    const std::string module = t.substr(i, e - i);
    if (module == "__future__") return;
    if (level == 0 && module.empty()) return;

    const std::size_t imp = t.find("import", e);
    if (imp == std::string::npos || !keyword_at(t, imp, "import")) return;

    std::vector<std::string> names;
    std::vector<std::string> bound;
    for (const auto& part : split(t.substr(imp + 6), ',')) {
        const std::string n = first_token(part);
        if (n.empty()) continue;
        names.push_back(n);
        const std::string a = alias_of(part);
        bound.push_back(a.empty() ? n : a);
    }
    out.push_back(ImportStmt{module, level, std::move(names), std::move(bound), line, snippet});
}

int paren_balance(const std::string& s) {
    int d = 0;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') ++d;
        else if (c == ')' || c == ']' || c == '}') --d;
    }
    return d;
}

} // namespace

std::vector<ImportStmt> parse_python_imports(const std::string& text) {
    std::vector<ImportStmt> out;
    std::istringstream      in(text);
    std::string             raw;
    Scrubber                scrub;

    std::string logical;   // the statement being assembled across physical lines
    std::string snippet;
    int         start   = 0;
    int         lineno  = 0;
    int         depth   = 0;
    bool        joining = false;

    auto flush = [&] {
        if (!logical.empty()) {
            for (const auto& stmt : split(logical, ';')) parse_statement(stmt, start, snippet, out);
        }
        logical.clear();
        depth   = 0;
        joining = false;
    };

    while (std::getline(in, raw)) {
        ++lineno;
        std::string code = scrub.scrub(raw);
        bool continued = false;
        {
            const std::string t = trim(code);
            if (!t.empty() && t.back() == '\\') { continued = true; code = t.substr(0, t.size() - 1); }
        }

        if (!joining) {
            const std::string t = trim(code);
            const bool starts = keyword_at(t, 0, "import") || keyword_at(t, 0, "from");
            if (!starts) continue;
            start   = lineno;
            snippet = trim(code);
            logical = code;
        } else {
            logical += ' ' + code;
        }
        depth += paren_balance(code);
        joining = continued || depth > 0;
        if (!joining) flush();
    }
    flush();
    return out;
}

std::vector<std::string> python_source_roots(const std::map<std::string, bool>& entries,
                                             const std::vector<std::string>&    package_dirs) {
    std::vector<std::string> roots;
    auto add = [&](const std::string& r) {
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    };
    auto is_dir = [&](const std::string& r) {
        auto it = entries.find(r);
        return it != entries.end() && it->second;
    };
    auto with_src = [&](const std::string& r) {
        add(r);
        const std::string src = r.empty() ? "src" : r + "/src";
        if (is_dir(src)) add(src);
    };
    with_src("");
    for (const auto& p : package_dirs) with_src(p);
    return roots;
}

namespace {

std::string join_under(const std::string& root, const std::vector<std::string>& parts) {
    std::string p = root;
    for (const auto& part : parts) {
        if (!p.empty()) p += '/';
        p += part;
    }
    return p;
}

// `a/b.py` before `a/b/__init__.py`, which is also the interpreter's order.
std::string find_module(const std::set<std::string>& files, const std::string& root,
                        const std::vector<std::string>& parts) {
    const std::string base = join_under(root, parts);
    if (files.count(base + ".py")) return base + ".py";
    if (files.count(base + "/__init__.py")) return base + "/__init__.py";
    return {};
}

std::vector<std::string> dotted_parts(const std::string& module) {
    std::vector<std::string> parts;
    if (module.empty()) return parts;
    for (const auto& p : split(module, '.')) {
        if (!p.empty()) parts.push_back(p);
    }
    return parts;
}

// Directory `level` dots up from the importing file, or nullopt-equivalent "" with
// `ok=false` when the dots climb out of the repository.
std::string relative_base(const std::string& from_rel, int level, bool& ok) {
    std::string dir = from_rel;
    for (int i = 0; i < level; ++i) {
        const auto slash = dir.rfind('/');
        if (slash == std::string::npos) {
            // Climbing above the root is only fine on the last step, which lands at "".
            ok = (i == level - 1) && !dir.empty();
            return {};
        }
        dir = dir.substr(0, slash);
    }
    ok = true;
    return dir;
}

} // namespace

std::string resolve_python_module(const std::string& from_rel, const std::string& module,
                                  int level, const std::set<std::string>& files,
                                  const std::vector<std::string>& roots, bool* ambiguous) {
    if (ambiguous) *ambiguous = false;
    const auto parts = dotted_parts(module);
    if (level > 0) {
        bool              ok   = false;
        const std::string base = relative_base(from_rel, level, ok);
        if (!ok || parts.empty()) return {};
        return find_module(files, base, parts);
    }
    std::string hit;
    for (const auto& r : roots) {
        const std::string found = find_module(files, r, parts);
        if (found.empty()) continue;
        if (hit.empty()) hit = found;
        else if (found != hit && ambiguous) *ambiguous = true;
    }
    return hit;
}


std::vector<FileImport> resolve_python_imports(const std::string&              from_rel,
                                               const std::vector<ImportStmt>&  stmts,
                                               const std::set<std::string>&    files,
                                               const std::vector<std::string>& roots) {
    std::vector<FileImport> out;

    auto emit = [&](const std::string& to, const ImportStmt& s, bool ambiguous) {
        if (to.empty() || to == from_rel) return;
        for (const auto& f : out) {
            if (f.to == to) return;
        }
        out.push_back(FileImport{to, s.line, s.snippet, ambiguous});
    };

    // One target per name over one set of roots; "" when nothing matched. The first
    // root wins, and a second root that also matches -- to a different file -- makes it
    // ambiguous.
    auto resolve_in = [&](const std::vector<std::string>& in_roots,
                          const std::vector<std::string>& parts, const ImportStmt& s) {
        std::vector<std::vector<std::string>> targets;   // one list of parts per lookup
        if (s.names.empty()) {
            targets.push_back(parts);
        } else {
            for (const auto& n : s.names) {
                auto sub = parts;
                sub.push_back(n);
                targets.push_back(std::move(sub));
            }
        }
        for (const auto& want : targets) {
            std::string hit;
            bool        ambiguous = false;
            for (const auto& r : in_roots) {
                std::string found = find_module(files, r, want);
                // `from pkg import name` where name is not a submodule: the name lives in
                // the module itself.
                if (found.empty() && !s.names.empty()) found = find_module(files, r, parts);
                if (found.empty()) continue;
                if (hit.empty()) hit = found;
                else if (found != hit) ambiguous = true;
            }
            emit(hit, s, ambiguous);
        }
    };

    for (const auto& s : stmts) {
        const auto parts = dotted_parts(s.module);
        if (s.level == 0) {
            resolve_in(roots, parts, s);
        } else {
            bool              ok   = false;
            const std::string base = relative_base(from_rel, s.level, ok);
            if (!ok) continue;
            resolve_in({base}, parts, s);
        }
    }
    return out;
}

} // namespace rgv::watch

namespace rgv::watch {

std::vector<PyPackage> python_packages(const std::set<std::string>&    files,
                                       const std::vector<std::string>& roots) {
    std::vector<PyPackage> out;
    for (const auto& f : files) {
        const std::string marker = "__init__.py";
        if (f.size() <= marker.size() || f.compare(f.size() - marker.size(), marker.size(), marker) != 0) continue;
        if (f[f.size() - marker.size() - 1] != '/') continue;   // a root-level __init__.py is not a package
        const std::string rel = f.substr(0, f.size() - marker.size() - 1);

        // The longest root that is a proper prefix names the package.
        std::string best;
        bool        found = false;
        for (const auto& r : roots) {
            const bool prefix = r.empty() || (rel.size() > r.size() && rel.compare(0, r.size(), r) == 0 && rel[r.size()] == '/');
            if (prefix && (!found || r.size() > best.size())) { best = r; found = true; }
        }
        std::string module = best.empty() ? rel : rel.substr(best.size() + 1);
        std::replace(module.begin(), module.end(), '/', '.');
        out.push_back(PyPackage{rel, module});
    }
    std::sort(out.begin(), out.end(),
              [](const PyPackage& a, const PyPackage& b) { return a.rel < b.rel; });
    return out;
}

std::vector<PackageDep> aggregate_imports(
    const std::vector<std::pair<std::string, FileImport>>& file_imports,
    const std::function<std::string(const std::string&)>&  owner) {
    std::map<std::pair<std::string, std::string>, PackageDep> deps;
    for (const auto& [from, fi] : file_imports) {
        const std::string p = owner(from);
        const std::string q = owner(fi.to);
        if (p.empty() || q.empty() || p == q) continue;
        auto [it, fresh] = deps.try_emplace({p, q}, PackageDep{p, q, from, fi.line, fi.snippet, fi.ambiguous});
        if (!fresh) it->second.heuristic = it->second.heuristic && fi.ambiguous;
    }
    std::vector<PackageDep> out;
    for (auto& [key, d] : deps) out.push_back(std::move(d));
    return out;
}

} // namespace rgv::watch

namespace rgv::watch {
namespace {

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string ident_at(const std::string& s, std::size_t at) {
    std::size_t e = at;
    while (e < s.size() && ident_char(s[e])) ++e;
    return s.substr(at, e - at);
}

// The physical lines that belong to import statements, so a use scan can skip them.
// Same joining rules as parse_python_imports, kept in step by being the same shape.
std::vector<bool> import_lines(const std::string& text) {
    std::vector<bool>  out;
    std::istringstream in(text);
    std::string        raw;
    Scrubber           scrub;
    bool               joining = false;
    int                depth   = 0;
    while (std::getline(in, raw)) {
        std::string code = scrub.scrub(raw);
        bool        continued = false;
        {
            const std::string t = trim(code);
            if (!t.empty() && t.back() == '\\') { continued = true; code = t.substr(0, t.size() - 1); }
        }
        bool is_import = joining;
        if (!joining) {
            const std::string t = trim(code);
            is_import = keyword_at(t, 0, "import") || keyword_at(t, 0, "from");
            depth     = 0;
        }
        if (is_import) {
            depth += paren_balance(code);
            joining = continued || depth > 0;
        }
        out.push_back(is_import);
    }
    return out;
}

} // namespace

std::vector<Definition> parse_python_symbols(const std::string& text) {
    std::vector<Definition> out;
    std::istringstream      in(text);
    std::string             raw;
    Scrubber                scrub;
    int                     lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        const std::string code = scrub.scrub(raw);
        if (code.empty() || std::isspace(static_cast<unsigned char>(code[0]))) continue;   // top level only
        std::string kind;
        std::size_t at = 0;
        if (keyword_at(code, 0, "class")) { kind = "class"; at = 5; }
        else if (keyword_at(code, 0, "def")) { kind = "function"; at = 3; }
        else if (code.rfind("async ", 0) == 0 && keyword_at(code, 6, "def")) { kind = "function"; at = 9; }
        else continue;
        while (at < code.size() && std::isspace(static_cast<unsigned char>(code[at]))) ++at;
        if (at >= code.size() || !ident_start(code[at])) continue;
        out.push_back(Definition{ident_at(code, at), kind, lineno});
    }
    return out;
}

std::vector<Use> parse_python_uses(const std::string& text, const std::vector<ImportStmt>& stmts) {
    std::set<std::string> bound;
    for (const auto& s : stmts) {
        for (const auto& b : s.bound) bound.insert(b);
    }
    std::vector<Use> out;
    if (bound.empty()) return out;

    const auto         skip = import_lines(text);
    std::istringstream in(text);
    std::string        raw;
    Scrubber           scrub;
    int                lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        const std::string code = scrub.scrub(raw);
        if (static_cast<std::size_t>(lineno) <= skip.size() && skip[lineno - 1]) continue;

        std::string prev_word;   // the identifier before this one, for def/class/as
        for (std::size_t i = 0; i < code.size();) {
            if (!ident_start(code[i])) { ++i; continue; }
            const std::string id = ident_at(code, i);
            const std::size_t start = i;
            i += id.size();

            // What precedes it: an attribute position is not a use of the name.
            std::size_t p = start;
            while (p > 0 && std::isspace(static_cast<unsigned char>(code[p - 1]))) --p;
            const bool attribute  = p > 0 && code[p - 1] == '.';
            const bool definition = prev_word == "def" || prev_word == "class" || prev_word == "as";
            prev_word             = id;
            if (attribute || definition || !bound.count(id)) continue;

            // What follows: `.attr`, and then `(`.
            std::size_t n = i;
            while (n < code.size() && std::isspace(static_cast<unsigned char>(code[n]))) ++n;
            Use u{id, "", false, lineno, trim(raw)};
            if (n < code.size() && code[n] == '.') {
                std::size_t a = n + 1;
                while (a < code.size() && std::isspace(static_cast<unsigned char>(code[a]))) ++a;
                if (a < code.size() && ident_start(code[a])) {
                    u.attr = ident_at(code, a);
                    n      = a + u.attr.size();
                    i      = n;   // the attribute is consumed; it is not a separate use
                    prev_word = u.attr;
                    while (n < code.size() && std::isspace(static_cast<unsigned char>(code[n]))) ++n;
                }
            }
            u.call = n < code.size() && code[n] == '(';
            out.push_back(std::move(u));
        }
    }
    return out;
}

namespace {

struct Found {
    std::string file, name;
};

// A definition named `name` in `file`, or wherever `file` re-exports it from.
Found find_definition(const std::string& file, const std::string& name, int depth,
                      const std::map<std::string, std::vector<ImportStmt>>& stmts_by_file,
                      const std::map<std::string, std::vector<Definition>>& defs_by_file,
                      const std::set<std::string>& files, const std::vector<std::string>& roots) {
    if (file.empty() || depth < 0) return {};
    if (auto d = defs_by_file.find(file); d != defs_by_file.end()) {
        for (const auto& def : d->second) {
            if (def.name == name) return {file, name};
        }
    }
    auto s = stmts_by_file.find(file);
    if (s == stmts_by_file.end()) return {};
    for (const auto& st : s->second) {
        if (st.names.empty()) continue;   // `import m` binds a module, never a symbol
        for (std::size_t i = 0; i < st.names.size(); ++i) {
            const bool star = st.names[i] == "*";
            if (!star && st.bound[i] != name) continue;
            const std::string target = resolve_python_module(file, st.module, st.level, files, roots);
            Found f = find_definition(target, star ? name : st.names[i], depth - 1, stmts_by_file,
                                      defs_by_file, files, roots);
            if (!f.file.empty()) return f;
        }
    }
    return {};
}

} // namespace

std::vector<SymbolRef> resolve_python_uses(
    const std::string& from_rel, const std::vector<ImportStmt>& stmts,
    const std::vector<Use>& uses,
    const std::map<std::string, std::vector<ImportStmt>>& stmts_by_file,
    const std::map<std::string, std::vector<Definition>>& defs_by_file,
    const std::set<std::string>& files, const std::vector<std::string>& roots) {
    struct Binding {
        const ImportStmt* stmt;
        std::size_t       item;
    };
    std::map<std::string, Binding> bindings;
    for (const auto& s : stmts) {
        for (std::size_t i = 0; i < s.bound.size(); ++i) bindings[s.bound[i]] = {&s, i};
    }

    std::vector<SymbolRef> out;
    auto emit = [&](const Found& f, const Use& u) {
        if (f.file.empty() || f.file == from_rel) return;
        const std::string kind = u.call ? "calls" : "references";
        for (const auto& r : out) {
            if (r.file == f.file && r.symbol == f.name && r.kind == kind) return;
        }
        out.push_back(SymbolRef{f.file, f.name, kind, u.line, u.snippet});
    };
    auto find = [&](const std::string& file, const std::string& name) {
        return find_definition(file, name, 4, stmts_by_file, defs_by_file, files, roots);
    };

    for (const auto& u : uses) {
        auto b = bindings.find(u.name);
        if (b == bindings.end()) continue;
        const ImportStmt& st = *b->second.stmt;

        if (st.names.empty()) {
            // `import a.b as c` then `c.attr`: attr is defined in a.b. A bare
            // `import a.b` binds `a`, and `a.b.attr` is one level deeper than a use
            // records, so only the aliased or single-component form resolves.
            if (u.attr.empty()) continue;
            const bool aliased = st.bound[0] != st.module.substr(0, st.module.find('.'));
            if (!aliased && st.module.find('.') != std::string::npos) continue;
            emit(find(resolve_python_module(from_rel, st.module, 0, files, roots), u.attr), u);
            continue;
        }

        const std::string& name = st.names[b->second.item];
        // `from pkg import name`: a submodule, or a symbol in the module.
        const std::string sub_module = st.module.empty() ? name : st.module + "." + name;
        const std::string sub        = resolve_python_module(from_rel, sub_module, st.level, files, roots);
        if (!sub.empty()) {
            if (!u.attr.empty()) emit(find(sub, u.attr), u);
            continue;
        }
        const std::string mod = resolve_python_module(from_rel, st.module, st.level, files, roots);
        emit(find(mod, name), u);
    }
    return out;
}

} // namespace rgv::watch
