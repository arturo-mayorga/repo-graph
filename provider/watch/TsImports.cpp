#include "TsImports.h"

#include <algorithm>
#include <cstring>
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
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// `word` at `at`, not merely a prefix of a longer identifier.
bool word_at(const std::string& s, std::size_t at, const char* word) {
    const std::size_t n = std::strlen(word);
    if (s.compare(at, n, word) != 0) return false;
    if (at > 0 && ident_char(s[at - 1])) return false;
    return at + n >= s.size() || !ident_char(s[at + n]);
}

// The file with comments blanked and string contents kept, one entry per source line.
//
// Both halves matter. A `//` inside a string is not a comment, and an import inside a
// comment is not an import -- and the specifier we are after IS a string, so they cannot
// simply all be blanked the way the Python reader blanks them.
std::vector<std::string> strip_comments(const std::string& text) {
    std::vector<std::string> out;
    std::string              cur;
    bool                     in_block = false;
    char                     quote    = 0;   // ' " or `
    bool                     escaped  = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
            if (quote != '`') quote = 0;   // only a template literal spans lines
            escaped = false;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                in_block = false;
                ++i;
            }
            cur.push_back(' ');
            continue;
        }
        if (quote != 0) {
            cur.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i + 1 < text.size() && text[i + 1] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            in_block = true;
            ++i;
            cur.push_back(' ');
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') quote = c;
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// The first quoted string at or after `from`, and where it ended.
bool first_string(const std::string& s, std::size_t from, std::string* out, std::size_t* at) {
    for (std::size_t i = from; i < s.size(); ++i) {
        const char q = s[i];
        if (q != '\'' && q != '"' && q != '`') continue;
        std::string content;
        for (std::size_t j = i + 1; j < s.size(); ++j) {
            if (s[j] == '\\') { ++j; continue; }
            if (s[j] == q) {
                *out = content;
                if (at) *at = i;
                return true;
            }
            content.push_back(s[j]);
        }
        return false;   // unterminated
    }
    return false;
}

// The word `from` outside any string, before `before`.
bool has_from_before(const std::string& s, std::size_t before) {
    for (std::size_t i = 0; i + 4 <= s.size() && i < before; ++i) {
        if (word_at(s, i, "from")) return true;
    }
    return false;
}

} // namespace

std::vector<TsImport> parse_ts_imports(const std::string& text) {
    const std::vector<std::string> lines = strip_comments(text);
    std::vector<TsImport>          out;

    // `require('x')` and dynamic `import('x')` can sit anywhere in an expression, so
    // they are found by shape rather than by where the statement starts.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        for (std::size_t j = 0; j < line.size(); ++j) {
            const bool is_require = word_at(line, j, "require");
            const bool is_dynamic = word_at(line, j, "import");
            if (!is_require && !is_dynamic) continue;
            std::size_t k = j + (is_require ? 7 : 6);
            while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k]))) ++k;
            if (k >= line.size() || line[k] != '(') continue;
            std::string spec;
            std::size_t at = 0;
            if (!first_string(line, k, &spec, &at) || spec.empty()) continue;
            out.push_back(TsImport{spec, static_cast<int>(i) + 1, trim(line), false});
            j = at;
        }
    }

    // Static `import ... from 'x'` and `export ... from 'x'`, which may run over lines.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string head = trim(lines[i]);
        const bool is_import = word_at(head, 0, "import");
        const bool is_export = word_at(head, 0, "export");
        if (!is_import && !is_export) continue;
        if (is_import) {
            // `import(` is the dynamic form and was taken above.
            std::size_t k = 6;
            while (k < head.size() && std::isspace(static_cast<unsigned char>(head[k]))) ++k;
            if (k < head.size() && head[k] == '(') continue;
        }

        // Accumulate until the statement offers a string or ends. Without the bound a
        // plain `export const x = 1` would swallow the rest of the file looking for one.
        std::string joined = head;
        std::size_t j      = i;
        std::string spec;
        std::size_t at = 0;
        while (!first_string(joined, 0, &spec, &at) && joined.find(';') == std::string::npos) {
            if (++j >= lines.size() || j > i + 60) break;
            joined += " " + trim(lines[j]);
        }
        if (spec.empty() && !first_string(joined, 0, &spec, &at)) continue;
        if (spec.empty()) continue;
        // `export const greeting = 'hi'` is a string in an export, not a re-export.
        if (is_export && !has_from_before(joined, at)) continue;

        const bool type_only = word_at(joined, is_import ? 7 : 7, "type") ||
                               joined.find(" type ") != std::string::npos;
        out.push_back(TsImport{spec, static_cast<int>(i) + 1, head, type_only});
        i = j;
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const TsImport& a, const TsImport& b) { return a.line < b.line; });
    return out;
}

namespace {

std::string dirname_of(const std::string& rel) {
    const auto slash = rel.rfind('/');
    return slash == std::string::npos ? std::string{} : rel.substr(0, slash);
}

// `.` and `..` resolved away. Empty when it climbs out of the repository.
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    std::string              cur;
    std::istringstream       in(path);
    while (std::getline(in, cur, '/')) {
        if (cur.empty() || cur == ".") continue;
        if (cur == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
            continue;
        }
        parts.push_back(cur);
    }
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += '/';
        out += p;
    }
    return out;
}

// Extensions a specifier may have left off, TypeScript before JavaScript so a source
// file wins over anything compiled beside it.
const char* const kExts[] = {".ts",  ".tsx", ".mts", ".cts", ".d.ts",
                             ".js",  ".jsx", ".mjs", ".cjs", ".json"};

std::string try_path(const std::set<std::string>& files, const std::string& base) {
    if (base.empty()) return {};
    if (files.count(base)) return base;

    // TypeScript compiled for ESM writes `./util.js` and means the `./util.ts` beside
    // it. Only after the literal file has been looked for, so a specifier that meant
    // exactly what it said still gets it.
    static const std::pair<const char*, const char*> js_to_ts[] = {
        {".js", ".ts"}, {".js", ".tsx"}, {".js", ".d.ts"}, {".jsx", ".tsx"},
        {".mjs", ".mts"}, {".cjs", ".cts"}};
    for (const auto& [from, to] : js_to_ts) {
        const std::size_t n = std::strlen(from);
        if (base.size() > n && base.compare(base.size() - n, n, from) == 0) {
            const std::string swapped = base.substr(0, base.size() - n) + to;
            if (files.count(swapped)) return swapped;
        }
    }
    for (const char* ext : kExts) {
        if (files.count(base + ext)) return base + ext;
    }
    for (const char* ext : kExts) {
        if (files.count(base + "/index" + ext)) return base + "/index" + ext;
    }
    return {};
}

} // namespace

std::string resolve_ts_specifier(const std::string& from_rel, const std::string& spec,
                                 const std::set<std::string>&  files,
                                 const std::vector<TsPackage>& packages) {
    if (spec.empty() || spec[0] == '/') return {};

    if (spec[0] == '.') {
        const std::string dir = dirname_of(from_rel);
        return try_path(files, normalize(dir.empty() ? spec : dir + "/" + spec));
    }

    // Bare: a package in this repository, or something in node_modules that is not ours
    // to report. The longest matching name wins, so `@acme/auth-core` is not mistaken
    // for a subpath of `@acme/auth`.
    const TsPackage* best = nullptr;
    for (const auto& p : packages) {
        if (p.name.empty()) continue;
        const bool exact = spec == p.name;
        const bool under = spec.size() > p.name.size() &&
                           spec.compare(0, p.name.size(), p.name) == 0 &&
                           spec[p.name.size()] == '/';
        if (!exact && !under) continue;
        if (!best || p.name.size() > best->name.size()) best = &p;
    }
    if (best == nullptr) return {};

    if (spec.size() > best->name.size()) {
        return try_path(files, normalize(best->rel + "/" + spec.substr(best->name.size() + 1)));
    }
    if (!best->entry.empty()) {
        const std::string hit = try_path(files, normalize(best->rel + "/" + best->entry));
        if (!hit.empty()) return hit;
    }
    // The conventional entries, for a manifest that names one this repository does not
    // contain -- `main` usually points into a build directory nobody checks in.
    for (const char* where : {"", "/src"}) {
        const std::string hit = try_path(files, normalize(best->rel + where));
        if (!hit.empty()) return hit;
    }
    return {};
}

std::vector<FileImport> resolve_ts_imports(const std::string&            from_rel,
                                           const std::vector<TsImport>&  imports,
                                           const std::set<std::string>&  files,
                                           const std::vector<TsPackage>& packages) {
    std::vector<FileImport> out;
    for (const auto& imp : imports) {
        const std::string to = resolve_ts_specifier(from_rel, imp.spec, files, packages);
        if (to.empty() || to == from_rel) continue;
        bool seen = false;
        for (const auto& f : out) {
            if (f.to == to) { seen = true; break; }
        }
        if (seen) continue;
        out.push_back(FileImport{to, imp.line, imp.snippet, false});
    }
    return out;
}

} // namespace rgv::watch
