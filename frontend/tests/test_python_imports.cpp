// The Python import extractor: statements out of source, files out of statements.
//
// The parser's failure mode that matters is a false positive -- an edge conjured from
// a comment, a docstring, or a string literal -- because the product is an argument
// that the graph can be trusted. The resolver's is a wrong target, which is why
// anything it cannot place in the repository produces nothing at all.

#include "TestMain.h"

#include "PythonImports.h"

#include <map>
#include <set>
#include <string>

using namespace rgv;

namespace {

const watch::ImportStmt* stmt_for(const std::vector<watch::ImportStmt>& ss,
                                  const std::string& module, int level = 0) {
    for (const auto& s : ss) {
        if (s.module == module && s.level == level) return &s;
    }
    return nullptr;
}

bool resolves(const std::vector<watch::FileImport>& fi, const std::string& to) {
    for (const auto& f : fi) {
        if (f.to == to) return true;
    }
    return false;
}


} // namespace

// -- parsing ------------------------------------------------------------------

TEST(plain_imports_are_split_per_module_and_aliases_are_dropped) {
    const auto ss = watch::parse_python_imports("import os\nimport a.b as c, d\n");
    CHECK_EQ(ss.size(), 3u);
    CHECK(stmt_for(ss, "os") != nullptr);
    CHECK(stmt_for(ss, "a.b") != nullptr);
    CHECK(stmt_for(ss, "d") != nullptr);
    CHECK_EQ(stmt_for(ss, "a.b")->line, 2);
    CHECK(stmt_for(ss, "a.b")->names.empty());
}

TEST(from_imports_carry_the_module_and_the_names_without_aliases) {
    const auto ss = watch::parse_python_imports("from x.y import z, w as v\n");
    CHECK_EQ(ss.size(), 1u);
    CHECK_EQ(ss[0].module, std::string("x.y"));
    CHECK_EQ(ss[0].level, 0);
    CHECK_EQ(ss[0].names.size(), 2u);
    CHECK_EQ(ss[0].names[0], std::string("z"));
    CHECK_EQ(ss[0].names[1], std::string("w"));
}

TEST(relative_imports_count_their_dots) {
    const auto ss = watch::parse_python_imports(
        "from . import a\nfrom ..pkg.mod import f\nfrom .sib import g\n");
    CHECK_EQ(ss.size(), 3u);
    CHECK(stmt_for(ss, "", 1) != nullptr);
    CHECK_EQ(stmt_for(ss, "", 1)->names[0], std::string("a"));
    CHECK(stmt_for(ss, "pkg.mod", 2) != nullptr);
    CHECK(stmt_for(ss, "sib", 1) != nullptr);
}

// A statement can span lines two ways. Evidence points at the line it starts on, which
// is where a reader's eye goes.
TEST(a_statement_spanning_lines_is_one_statement_reported_at_its_first_line) {
    const auto ss = watch::parse_python_imports(
        "x = 1\n"
        "from pkg.mod import (\n"
        "    a,\n"
        "    b,  # trailing\n"
        ")\n"
        "from other import c, \\\n"
        "    d\n");
    CHECK_EQ(ss.size(), 2u);
    const auto* p = stmt_for(ss, "pkg.mod");
    CHECK(p != nullptr);
    CHECK_EQ(p->line, 2);
    CHECK_EQ(p->names.size(), 2u);
    CHECK_EQ(p->names[1], std::string("b"));
    CHECK_EQ(p->snippet, std::string("from pkg.mod import ("));
    const auto* o = stmt_for(ss, "other");
    CHECK(o != nullptr);
    CHECK_EQ(o->line, 6);
    CHECK_EQ(o->names.size(), 2u);
}

// The false positives that would discredit the graph.
TEST(imports_in_comments_and_strings_are_not_imports) {
    const auto ss = watch::parse_python_imports(
        "# import fake_a\n"
        "\"\"\"\n"
        "import fake_b\n"
        "from fake_c import x\n"
        "\"\"\"\n"
        "s = 'import fake_d'\n"
        "t = '''\nimport fake_e\n'''\n"
        "import real\n");
    CHECK_EQ(ss.size(), 1u);
    CHECK(stmt_for(ss, "real") != nullptr);
}

// Imports inside functions, `try` blocks and `if TYPE_CHECKING:` are still
// dependencies -- a change to the target still breaks this file.
TEST(indented_imports_are_imports) {
    const auto ss = watch::parse_python_imports(
        "def f():\n    import late\n"
        "try:\n    from opt import thing\nexcept ImportError:\n    thing = None\n");
    CHECK_EQ(ss.size(), 2u);
    CHECK(stmt_for(ss, "late") != nullptr);
    CHECK(stmt_for(ss, "opt") != nullptr);
}

TEST(future_imports_are_not_dependencies) {
    const auto ss = watch::parse_python_imports("from __future__ import annotations\n");
    CHECK(ss.empty());
}

TEST(a_name_that_merely_starts_with_import_is_not_a_statement) {
    const auto ss = watch::parse_python_imports("importer = 1\nfromage = 2\nimports.run()\n");
    CHECK(ss.empty());
}

// -- resolution ---------------------------------------------------------------

namespace {

struct Tree {
    std::set<std::string>       files;
    std::map<std::string, bool> entries;
    std::vector<std::string>    packages;

    void file(const std::string& rel) {
        files.insert(rel);
        entries[rel] = false;
        std::string dir = rel;
        for (auto slash = dir.rfind('/'); slash != std::string::npos; slash = dir.rfind('/')) {
            dir = dir.substr(0, slash);
            entries[dir] = true;
        }
    }
    std::vector<std::string> roots() const {
        return watch::python_source_roots(entries, packages);
    }
    std::vector<watch::FileImport> resolve(const std::string& from, const std::string& src) const {
        return watch::resolve_python_imports(from, watch::parse_python_imports(src), files,
                                             roots());
    }
};

} // namespace

TEST(an_absolute_import_finds_a_module_or_a_package_init) {
    Tree t;
    t.file("app/__init__.py");
    t.file("app/models.py");
    t.file("main.py");
    const auto fi = t.resolve("main.py", "import app.models\nimport app\n");
    CHECK_EQ(fi.size(), 2u);
    CHECK(resolves(fi, "app/models.py"));
    CHECK(resolves(fi, "app/__init__.py"));
}

// `from app import models` names a submodule; `from app import User` names something
// inside `app/__init__.py`. Same syntax, different target -- the file system decides.
TEST(a_from_import_prefers_a_submodule_over_the_package_init) {
    Tree t;
    t.file("app/__init__.py");
    t.file("app/models.py");
    t.file("main.py");
    const auto sub = t.resolve("main.py", "from app import models\n");
    CHECK_EQ(sub.size(), 1u);
    CHECK(resolves(sub, "app/models.py"));

    const auto init = t.resolve("main.py", "from app import User\n");
    CHECK_EQ(init.size(), 1u);
    CHECK(resolves(init, "app/__init__.py"));

    const auto both = t.resolve("main.py", "from app import models, User\n");
    CHECK_EQ(both.size(), 2u);
}

TEST(a_from_import_of_a_module_attribute_targets_the_module) {
    Tree t;
    t.file("app/models.py");
    t.file("main.py");
    const auto fi = t.resolve("main.py", "from app.models import User, Group\n");
    CHECK_EQ(fi.size(), 1u);
    CHECK(resolves(fi, "app/models.py"));
    CHECK_EQ(fi[0].line, 1);
    CHECK_EQ(fi[0].snippet, std::string("from app.models import User, Group"));
}

TEST(relative_imports_resolve_against_the_importing_file) {
    Tree t;
    t.file("app/__init__.py");
    t.file("app/models.py");
    t.file("app/views/__init__.py");
    t.file("app/views/home.py");
    t.file("app/views/util.py");
    t.file("app/views/forms.py");
    const auto fi = t.resolve("app/views/home.py",
                              "from . import util\nfrom ..models import User\n"
                              "from .forms import LoginForm\nfrom .. import models\n");
    CHECK(resolves(fi, "app/views/util.py"));
    CHECK(resolves(fi, "app/models.py"));
    CHECK(resolves(fi, "app/views/forms.py"));
    CHECK_EQ(fi.size(), 3u);   // models.py is reached twice and reported once
}

TEST(a_from_dot_import_with_no_submodule_targets_the_package_init) {
    Tree t;
    t.file("app/__init__.py");
    t.file("app/a.py");
    const auto fi = t.resolve("app/a.py", "from . import CONSTANT\n");
    CHECK_EQ(fi.size(), 1u);
    CHECK(resolves(fi, "app/__init__.py"));
}

// The src layout: `libs/core/pyproject.toml` with the code under `libs/core/src/core/`.
// An absolute `import core` from anywhere in the repo means that directory.
TEST(a_src_layout_package_is_a_source_root) {
    Tree t;
    t.packages = {"libs/core", "services/api"};
    t.file("libs/core/pyproject.toml");
    t.file("libs/core/src/core/__init__.py");
    t.file("libs/core/src/core/db.py");
    t.file("services/api/pyproject.toml");
    t.file("services/api/api/main.py");
    const auto fi = t.resolve("services/api/api/main.py", "from core.db import connect\nimport api\n");
    CHECK(resolves(fi, "libs/core/src/core/db.py"));
    // `import api` from inside the api package dir: the package dir is a root too.
    CHECK_EQ(fi.size(), 1u);   // api/ has no __init__.py and no api.py, so nothing
}

TEST(a_top_level_src_directory_is_a_source_root) {
    Tree t;
    t.file("src/thing/__init__.py");
    t.file("tests/test_thing.py");
    const auto fi = t.resolve("tests/test_thing.py", "import thing\n");
    CHECK_EQ(fi.size(), 1u);
    CHECK(resolves(fi, "src/thing/__init__.py"));
}

TEST(unresolved_imports_produce_no_edge) {
    Tree t;
    t.file("main.py");
    t.file("app/models.py");
    const auto fi = t.resolve("main.py", "import os\nimport requests\nfrom app import nothing_here\n"
                                         "from app.missing import x\n");
    CHECK(fi.empty());
}

TEST(a_file_never_imports_itself) {
    Tree t;
    t.file("app/__init__.py");
    t.file("app/x.py");
    const auto fi = t.resolve("app/x.py", "from . import x\nimport app.x\n");
    CHECK(fi.empty());
}

// Two roots that both satisfy an import is a real ambiguity. Pick deterministically
// and say so, because a heuristic edge is filtered out of traversal by default.
TEST(an_import_two_roots_satisfy_is_marked_ambiguous) {
    Tree t;
    t.packages = {"pkg"};
    t.file("util.py");
    t.file("pkg/util.py");
    t.file("pkg/main.py");
    const auto fi = t.resolve("pkg/main.py", "import util\n");
    CHECK_EQ(fi.size(), 1u);
    CHECK(fi[0].ambiguous);
    CHECK_EQ(fi[0].to, std::string("util.py"));   // the repo root is first

    Tree u;
    u.file("app/util.py");
    u.file("app/main.py");
    const auto fj = u.resolve("app/main.py", "from app import util\n");
    CHECK_EQ(fj.size(), 1u);
    CHECK(!fj[0].ambiguous);
}

TEST(source_roots_are_the_repo_root_then_packages_then_their_src_dirs) {
    Tree t;
    t.packages = {"libs/core"};
    t.file("src/a.py");
    t.file("libs/core/src/core/__init__.py");
    const auto roots = t.roots();
    CHECK_EQ(roots.size(), 4u);
    CHECK_EQ(roots[0], std::string(""));
    CHECK_EQ(roots[1], std::string("src"));
    CHECK_EQ(roots[2], std::string("libs/core"));
    CHECK_EQ(roots[3], std::string("libs/core/src"));
}

// -- packages -----------------------------------------------------------------

TEST(every_directory_with_an_init_is_a_package_named_from_its_source_root) {
    std::set<std::string> files{"src/app/__init__.py", "src/app/core/__init__.py",
                                "src/app/core/db.py", "tools/gen/__init__.py", "tools/x.py"};
    const auto pkgs = watch::python_packages(files, {"", "src"});
    CHECK_EQ(pkgs.size(), 3u);
    CHECK_EQ(pkgs[0].rel, std::string("src/app"));
    CHECK_EQ(pkgs[0].module, std::string("app"));
    CHECK_EQ(pkgs[1].rel, std::string("src/app/core"));
    CHECK_EQ(pkgs[1].module, std::string("app.core"));
    CHECK_EQ(pkgs[2].rel, std::string("tools/gen"));
    CHECK_EQ(pkgs[2].module, std::string("tools.gen"));
}

TEST(a_package_at_the_repository_root_is_named_by_its_directory) {
    std::set<std::string> files{"app/__init__.py"};
    const auto pkgs = watch::python_packages(files, {""});
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].module, std::string("app"));
}

namespace {

std::pair<std::string, watch::FileImport> imp(const std::string& from, const std::string& to,
                                              int line, bool heuristic = false) {
    return {from, watch::FileImport{to, line, "import " + to, heuristic}};
}

} // namespace

TEST(imports_across_packages_aggregate_to_one_edge_with_the_first_as_evidence) {
    const std::map<std::string, std::string> owners{
        {"api/a.py", "P:api"}, {"api/b.py", "P:api"}, {"core/db.py", "P:core"},
        {"core/util.py", "P:core"}};
    auto owner = [&](const std::string& f) {
        auto it = owners.find(f);
        return it == owners.end() ? std::string{} : it->second;
    };
    const auto deps = watch::aggregate_imports(
        {imp("api/a.py", "api/b.py", 1),        // inside api: nothing
         imp("api/a.py", "core/db.py", 2),      // api -> core, the evidence
         imp("api/b.py", "core/util.py", 7),    // api -> core again
         imp("core/db.py", "core/util.py", 1),  // inside core
         imp("core/util.py", "api/a.py", 3)},   // core -> api: a cycle is still an edge
        owner);
    CHECK_EQ(deps.size(), 2u);
    CHECK_EQ(deps[0].from, std::string("P:api"));
    CHECK_EQ(deps[0].to, std::string("P:core"));
    CHECK_EQ(deps[0].artifact, std::string("api/a.py"));
    CHECK_EQ(deps[0].line, 2);
    CHECK_EQ(deps[1].from, std::string("P:core"));
    CHECK_EQ(deps[1].to, std::string("P:api"));
}

TEST(an_aggregated_edge_is_heuristic_only_when_every_import_behind_it_is) {
    auto owner = [](const std::string& f) { return f.substr(0, f.find('/')); };
    const auto weak = watch::aggregate_imports({imp("a/x.py", "b/y.py", 1, true)}, owner);
    CHECK_EQ(weak.size(), 1u);
    CHECK(weak[0].heuristic);
    const auto mixed = watch::aggregate_imports(
        {imp("a/x.py", "b/y.py", 1, true), imp("a/z.py", "b/y.py", 1, false)}, owner);
    CHECK_EQ(mixed.size(), 1u);
    CHECK(!mixed[0].heuristic);
}

TEST(a_file_owned_by_nothing_contributes_no_package_edge) {
    auto owner = [](const std::string& f) { return f.rfind("pkg/", 0) == 0 ? "P" : ""; };
    const auto deps = watch::aggregate_imports({imp("loose.py", "pkg/x.py", 1)}, owner);
    CHECK(deps.empty());
}

// -- symbols ------------------------------------------------------------------

TEST(import_statements_record_the_local_names_they_bind) {
    const auto ss = watch::parse_python_imports(
        "import a.b as c, d.e\nfrom m import x, y as z\n");
    CHECK_EQ(ss.size(), 3u);
    CHECK_EQ(stmt_for(ss, "a.b")->bound.size(), 1u);
    CHECK_EQ(stmt_for(ss, "a.b")->bound[0], std::string("c"));
    CHECK_EQ(stmt_for(ss, "d.e")->bound[0], std::string("d"));   // `import d.e` binds d
    CHECK_EQ(stmt_for(ss, "m")->bound.size(), 2u);
    CHECK_EQ(stmt_for(ss, "m")->bound[0], std::string("x"));
    CHECK_EQ(stmt_for(ss, "m")->bound[1], std::string("z"));
}

TEST(top_level_classes_and_functions_are_symbols_and_nested_ones_are_not) {
    const auto defs = watch::parse_python_symbols(
        "@dataclass\nclass CarState:\n    def inner(self): pass\n\n"
        "def helper(x):\n    class Local: pass\n    return x\n\n"
        "async def run(): pass\n"
        "s = '''\nclass Fake: pass\n'''\n");
    CHECK_EQ(defs.size(), 3u);
    CHECK_EQ(defs[0].name, std::string("CarState"));
    CHECK_EQ(defs[0].kind, std::string("class"));
    CHECK_EQ(defs[0].line, 2);
    CHECK_EQ(defs[1].name, std::string("helper"));
    CHECK_EQ(defs[1].kind, std::string("function"));
    CHECK_EQ(defs[2].name, std::string("run"));
}

TEST(uses_of_bound_names_are_found_with_their_attribute_and_call_shape) {
    const std::string src =
        "import esper\n"
        "from ..components.car import CarState, StopQueue\n"
        "from .. import queries\n"
        "\n"
        "class CarStateView:            # not a use: a definition\n"
        "    def process(self):\n"
        "        for e, (s,) in esper.get_components(CarState):\n"
        "            queue = esper.try_component(e, StopQueue)\n"
        "            esper.add_component(e, CarState(value=1))\n"
        "            load = queries.passenger_load(e)\n"
        "            self.CarState = 'CarState'\n";   // attribute and string: not uses
    const auto ss   = watch::parse_python_imports(src);
    const auto uses = watch::parse_python_uses(src, ss);

    auto count = [&](const std::string& name, const std::string& attr, bool call) {
        int n = 0;
        for (const auto& u : uses) {
            if (u.name == name && u.attr == attr && u.call == call) ++n;
        }
        return n;
    };
    CHECK_EQ(count("CarState", "", false), 1);    // get_components(CarState)
    CHECK_EQ(count("CarState", "", true), 1);     // CarState(value=1)
    CHECK_EQ(count("StopQueue", "", false), 1);
    CHECK_EQ(count("queries", "passenger_load", true), 1);
    CHECK_EQ(count("esper", "get_components", true), 1);
    int car_state_total = 0;
    for (const auto& u : uses) {
        if (u.name == "CarState") ++car_state_total;
    }
    CHECK_EQ(car_state_total, 2);   // not the class line, the attribute, the string, or the import

    const watch::Use* call = nullptr;
    for (const auto& u : uses) {
        if (u.name == "CarState" && u.call) call = &u;
    }
    CHECK(call != nullptr);
    CHECK_EQ(call->line, 9);
    CHECK_EQ(call->snippet, std::string("esper.add_component(e, CarState(value=1))"));
}

namespace {

struct Code {
    Tree                                                 tree;
    std::map<std::string, std::string>                   text;
    std::map<std::string, std::vector<watch::ImportStmt>> stmts;
    std::map<std::string, std::vector<watch::Definition>> defs;

    void file(const std::string& rel, const std::string& src) {
        tree.file(rel);
        text[rel]  = src;
        stmts[rel] = watch::parse_python_imports(src);
        defs[rel]  = watch::parse_python_symbols(src);
    }
    std::vector<watch::SymbolRef> refs(const std::string& rel) const {
        return watch::resolve_python_uses(rel, stmts.at(rel),
                                          watch::parse_python_uses(text.at(rel), stmts.at(rel)),
                                          stmts, defs, tree.files, tree.roots());
    }
};

const watch::SymbolRef* ref_to(const std::vector<watch::SymbolRef>& rs, const std::string& file,
                               const std::string& symbol, const std::string& kind) {
    for (const auto& r : rs) {
        if (r.file == file && r.symbol == symbol && r.kind == kind) return &r;
    }
    return nullptr;
}

} // namespace

TEST(a_use_resolves_to_the_definition_its_import_binds) {
    Code c;
    c.file("app/__init__.py", "");
    c.file("app/components/__init__.py", "");
    c.file("app/components/car.py", "class CarState: pass\nclass StopQueue: pass\n");
    c.file("app/queries.py", "def passenger_load(car): return 0\n");
    c.file("app/systems/__init__.py", "");
    c.file("app/systems/movement.py",
           "import esper\n"
           "from ..components.car import CarState as State, StopQueue\n"
           "from .. import queries\n"
           "def go(e):\n"
           "    s = esper.try_component(e, State)\n"
           "    esper.add_component(e, State(1))\n"
           "    q = queries.passenger_load(e)\n"
           "    t: StopQueue = None\n");
    const auto rs = c.refs("app/systems/movement.py");
    CHECK_EQ(rs.size(), 4u);
    const auto* read = ref_to(rs, "app/components/car.py", "CarState", "references");
    CHECK(read != nullptr);
    CHECK_EQ(read->line, 5);
    const auto* write = ref_to(rs, "app/components/car.py", "CarState", "calls");
    CHECK(write != nullptr);
    CHECK_EQ(write->line, 6);
    CHECK(ref_to(rs, "app/queries.py", "passenger_load", "calls") != nullptr);
    CHECK(ref_to(rs, "app/components/car.py", "StopQueue", "references") != nullptr);
}

// `from ..components import CarState` binds a name that `components/__init__.py` only
// re-exports. The symbol lives where it is defined, and the edge must say so.
TEST(a_re_exported_name_resolves_through_the_init_to_where_it_is_defined) {
    Code c;
    c.file("app/__init__.py", "");
    c.file("app/components/__init__.py", "from .car import CarState\n");
    c.file("app/components/car.py", "class CarState: pass\n");
    c.file("app/view.py", "from .components import CarState\nx = CarState()\n");
    const auto rs = c.refs("app/view.py");
    CHECK_EQ(rs.size(), 1u);
    CHECK_EQ(rs[0].file, std::string("app/components/car.py"));
    CHECK_EQ(rs[0].symbol, std::string("CarState"));
    CHECK_EQ(rs[0].kind, std::string("calls"));
}

TEST(a_use_of_a_name_the_target_does_not_define_produces_nothing) {
    Code c;
    c.file("app/__init__.py", "");
    c.file("app/car.py", "class CarState: pass\nLIMIT = 3\n");
    c.file("app/main.py", "import esper\nfrom .car import LIMIT, Missing\n"
                          "esper.process()\nprint(LIMIT, Missing)\n");
    CHECK(c.refs("app/main.py").empty());   // esper is external; LIMIT is not a symbol; Missing is not there
}

TEST(a_submodule_import_used_as_a_module_resolves_its_attributes) {
    Code c;
    c.file("app/__init__.py", "");
    c.file("app/policies/__init__.py", "");
    c.file("app/policies/nearest.py", "def choose(cars): pass\n");
    c.file("app/main.py", "from app.policies import nearest\nimport app.policies.nearest as alt\n"
                          "nearest.choose([])\nalt.choose([])\n");
    const auto rs = c.refs("app/main.py");
    CHECK_EQ(rs.size(), 1u);   // both spellings, one (file, symbol, kind)
    CHECK_EQ(rs[0].file, std::string("app/policies/nearest.py"));
    CHECK_EQ(rs[0].symbol, std::string("choose"));
    CHECK_EQ(rs[0].line, 3);
}
