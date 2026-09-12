// The TypeScript reader: specifiers out of source, files out of specifiers.
//
// Resolution is where the work is. A specifier is extensionless, the file it names may
// be `.ts`, `.tsx`, `.js` or a directory with an `index` in it, TypeScript compiled for
// ESM writes `./util.js` and means `./util.ts`, and a bare specifier is either a package
// in this repository or something in `node_modules` that is not ours to report.

#include "TestMain.h"

#include "TsImports.h"

#include <set>
#include <string>

using namespace rgv;

namespace {

const watch::TsImport* spec_for(const std::vector<watch::TsImport>& is, const std::string& s) {
    for (const auto& i : is) {
        if (i.spec == s) return &i;
    }
    return nullptr;
}

struct Repo {
    std::set<std::string>          files;
    std::vector<watch::TsPackage>  packages;

    void file(const std::string& rel) { files.insert(rel); }
    std::string resolve(const std::string& from, const std::string& spec) const {
        return watch::resolve_ts_specifier(from, spec, files, packages);
    }
};

} // namespace

// -- parsing ------------------------------------------------------------------

TEST(every_static_import_form_yields_its_specifier) {
    const auto is = watch::parse_ts_imports(
        "import def from './a';\n"
        "import { x, y as z } from './b';\n"
        "import * as ns from './c';\n"
        "import './d';\n"
        "import def2, { q } from './e';\n");
    CHECK_EQ(is.size(), 5u);
    for (const char* s : {"./a", "./b", "./c", "./d", "./e"}) CHECK(spec_for(is, s) != nullptr);
    CHECK_EQ(spec_for(is, "./b")->line, 2);
}

TEST(re_exports_require_and_dynamic_import_are_dependencies_too) {
    const auto is = watch::parse_ts_imports(
        "export { a } from './re';\n"
        "export * from './star';\n"
        "export type { T } from './types';\n"
        "const m = require('./cjs');\n"
        "const lazy = await import('./dyn');\n");
    CHECK_EQ(is.size(), 5u);
    for (const char* s : {"./re", "./star", "./types", "./cjs", "./dyn"}) {
        CHECK(spec_for(is, s) != nullptr);
    }
}

// `import type` is erased at run time and is still a dependency: change the type and
// the importer stops compiling.
TEST(a_type_only_import_is_kept_and_marked) {
    const auto is = watch::parse_ts_imports("import type { T } from './t';\n");
    CHECK_EQ(is.size(), 1u);
    CHECK(is[0].type_only);
}

TEST(an_import_spread_over_lines_is_one_import_reported_at_its_first_line) {
    const auto is = watch::parse_ts_imports(
        "const before = 1;\n"
        "import {\n"
        "  alpha,\n"
        "  beta,\n"
        "} from './wide';\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].spec, std::string("./wide"));
    CHECK_EQ(is[0].line, 2);
    CHECK_EQ(is[0].snippet, std::string("import {"));
}

// The false positives that would discredit the graph.
TEST(specifiers_in_comments_and_strings_are_not_imports) {
    const auto is = watch::parse_ts_imports(
        "// import fake from './comment';\n"
        "/* import fake2 from './block';\n"
        "   export * from './block2'; */\n"
        "const url = 'http://example.com';        // not a comment inside the string\n"
        "const s = \"import x from './in-a-string'\";\n"
        "const t = `import y from './in-a-template'`;\n"
        "import real from './real';\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].spec, std::string("./real"));
}

TEST(a_name_that_merely_starts_with_import_is_not_a_statement) {
    const auto is = watch::parse_ts_imports("const important = 1;\nexports.x = 2;\n");
    CHECK(is.empty());
}

// -- resolution ---------------------------------------------------------------

TEST(a_relative_specifier_is_extensionless_and_prefers_typescript) {
    Repo r;
    r.file("src/app.ts");
    r.file("src/util.ts");
    r.file("src/widget.tsx");
    CHECK_EQ(r.resolve("src/app.ts", "./util"), std::string("src/util.ts"));
    CHECK_EQ(r.resolve("src/app.ts", "./widget"), std::string("src/widget.tsx"));
}

TEST(a_directory_specifier_resolves_to_its_index) {
    Repo r;
    r.file("src/app.ts");
    r.file("src/models/index.ts");
    r.file("src/models/car.ts");
    CHECK_EQ(r.resolve("src/app.ts", "./models"), std::string("src/models/index.ts"));
    CHECK_EQ(r.resolve("src/app.ts", "./models/car"), std::string("src/models/car.ts"));
}

// The ESM convention: TypeScript source says `.js` and means the `.ts` beside it.
TEST(a_js_specifier_resolves_to_the_typescript_file_it_means) {
    Repo r;
    r.file("src/app.ts");
    r.file("src/util.ts");
    CHECK_EQ(r.resolve("src/app.ts", "./util.js"), std::string("src/util.ts"));

    // Unless the `.js` is really there, in which case it meant what it said.
    Repo both;
    both.file("src/app.ts");
    both.file("src/util.ts");
    both.file("src/util.js");
    CHECK_EQ(both.resolve("src/app.ts", "./util.js"), std::string("src/util.js"));
}

TEST(a_specifier_climbs_out_of_its_own_directory) {
    Repo r;
    r.file("src/deep/nested/here.ts");
    r.file("src/shared/util.ts");
    CHECK_EQ(r.resolve("src/deep/nested/here.ts", "../../shared/util"),
             std::string("src/shared/util.ts"));
}

TEST(a_bare_specifier_naming_a_package_in_the_repository_resolves_into_it) {
    Repo r;
    r.packages = {{"@acme/auth", "packages/auth", ""}, {"@acme/api", "packages/api", ""}};
    r.file("packages/api/src/server.ts");
    r.file("packages/auth/src/index.ts");
    r.file("packages/auth/src/token.ts");
    // No manifest entry, so the conventional index is tried.
    CHECK_EQ(r.resolve("packages/api/src/server.ts", "@acme/auth"),
             std::string("packages/auth/src/index.ts"));
    // A subpath is taken from the package directory.
    CHECK_EQ(r.resolve("packages/api/src/server.ts", "@acme/auth/src/token"),
             std::string("packages/auth/src/token.ts"));
}

TEST(a_manifest_entry_is_used_when_the_package_names_one) {
    Repo r;
    r.packages = {{"lib", "packages/lib", "src/main.ts"}};
    r.file("packages/lib/src/main.ts");
    r.file("packages/lib/index.ts");
    r.file("app.ts");
    CHECK_EQ(r.resolve("app.ts", "lib"), std::string("packages/lib/src/main.ts"));
}

// node_modules and Node's builtins are real dependencies, and not on anything in this
// repository. An edge to a node that does not exist is worse than a missing one.
TEST(a_specifier_this_repository_does_not_contain_resolves_to_nothing) {
    Repo r;
    r.packages = {{"@acme/auth", "packages/auth", ""}};
    r.file("packages/auth/src/index.ts");
    r.file("src/app.ts");
    CHECK(r.resolve("src/app.ts", "react").empty());
    CHECK(r.resolve("src/app.ts", "node:fs").empty());
    CHECK(r.resolve("src/app.ts", "fs").empty());
    CHECK(r.resolve("src/app.ts", "./missing").empty());
    CHECK(r.resolve("src/app.ts", "@acme/auth/nope").empty());
}

TEST(a_file_never_imports_itself) {
    Repo r;
    r.file("src/app.ts");
    const auto got = watch::resolve_ts_imports(
        "src/app.ts", watch::parse_ts_imports("import './app';\nimport './app.js';\n"),
        r.files, r.packages);
    CHECK(got.empty());
}

TEST(two_specifiers_reaching_the_same_file_are_reported_once) {
    Repo r;
    r.file("src/app.ts");
    r.file("src/util.ts");
    const auto got = watch::resolve_ts_imports(
        "src/app.ts",
        watch::parse_ts_imports("import { a } from './util';\nimport { b } from './util.js';\n"),
        r.files, r.packages);
    CHECK_EQ(got.size(), 1u);
    CHECK_EQ(got[0].to, std::string("src/util.ts"));
    CHECK_EQ(got[0].line, 1);
}
