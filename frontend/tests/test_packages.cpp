// The manifest readers: package detection and declared dependencies.
//
// This is the only part of the provider that knows an ecosystem exists, so it is the
// part where a mistake shows up as a wrong edge rather than a crash -- and a wrong
// dependency edge is worse than a missing one, because the whole product is an argument
// that the graph can be trusted.

#include "TestMain.h"

#include "Packages.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace rgv;

namespace {

namespace fs = std::filesystem;

struct Repo {
    fs::path root;

    explicit Repo(const std::string& name)
        : root(fs::path(RGV_TEST_TMP) / ("pkg-" + name)) {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void write(const std::string& rel, const std::string& text) const {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }

    // The walk the provider does, in the shape scan_packages expects.
    std::map<std::string, bool> entries() const {
        std::map<std::string, bool> out;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            out[fs::relative(e.path(), root).generic_string()] = e.is_directory();
        }
        return out;
    }

    std::vector<watch::Package> scan() const { return watch::scan_packages(root, entries()); }
};

const watch::Package* find(const std::vector<watch::Package>& ps, const std::string& name) {
    for (const auto& p : ps) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool depends_on(const watch::Package& p, const std::string& name) {
    for (const auto& d : p.deps) {
        if (d.name == name) return true;
    }
    return false;
}

} // namespace

TEST(an_npm_workspace_yields_packages_and_their_declared_dependencies) {
    Repo r("npm");
    r.write("packages/auth/package.json",
            R"({"name":"@acme/auth","version":"1.4.0",)"
            R"("dependencies":{"@acme/logger":"workspace:*","jsonwebtoken":"^9"},)"
            R"("devDependencies":{"vitest":"^1"}})");
    r.write("packages/logger/package.json", R"({"name":"@acme/logger","version":"0.2.0"})");

    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 2u);

    const auto* auth = find(pkgs, "@acme/auth");
    CHECK(auth != nullptr);
    CHECK_EQ(auth->rel, std::string("packages/auth"));
    CHECK_EQ(auth->provider, std::string("npm"));
    CHECK_EQ(auth->version, std::string("1.4.0"));
    CHECK(depends_on(*auth, "@acme/logger"));
    CHECK(depends_on(*auth, "jsonwebtoken"));
    CHECK(depends_on(*auth, "vitest"));
}

// A workspace root often has a package.json with no name. It configures the workspace;
// it is not something anything can depend on, and emitting it as a package puts a node
// on screen that means nothing.
TEST(a_manifest_with_no_name_is_not_a_package) {
    Repo r("unnamed");
    r.write("package.json", R"({"private":true,"workspaces":["packages/*"]})");
    r.write("packages/a/package.json", R"({"name":"a"})");
    CHECK_EQ(r.scan().size(), 1u);
}

// Evidence is what the provenance inspector shows. An edge the user cannot trace back
// to a line in a file is an assertion they have to take on faith.
TEST(a_dependency_carries_the_line_that_declares_it) {
    Repo r("evidence");
    r.write("package.json", "{\n  \"name\": \"solo\",\n  \"dependencies\": {\n"
                            "    \"requests\": \"^2\"\n  }\n}\n");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].deps.size(), 1u);
    CHECK_EQ(pkgs[0].deps[0].line, 4);
    CHECK_EQ(pkgs[0].deps[0].snippet, std::string("\"requests\": \"^2\""));
    CHECK_EQ(pkgs[0].deps[0].artifact, std::string("package.json"));
}

TEST(a_pep621_pyproject_yields_its_dependencies) {
    Repo r("pep621");
    r.write("pyproject.toml",
            "[project]\nname = \"acme-core\"\nversion = \"0.3.0\"\n"
            "dependencies = [\n  \"pydantic>=2\",\n  \"structlog\",\n]\n");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].name, std::string("acme-core"));
    CHECK_EQ(pkgs[0].provider, std::string("python"));
    CHECK_EQ(pkgs[0].version, std::string("0.3.0"));
    CHECK(depends_on(pkgs[0], "pydantic"));
    CHECK(depends_on(pkgs[0], "structlog"));
}

// Poetry states dependencies as a table rather than an array, and names the interpreter
// among them. `python` is not a package in the repository and must not become a node.
TEST(a_poetry_pyproject_yields_its_dependencies_without_the_interpreter) {
    Repo r("poetry");
    r.write("pyproject.toml",
            "[tool.poetry]\nname = \"acme-api\"\n\n[tool.poetry.dependencies]\n"
            "python = \"^3.11\"\nfastapi = \"^0.110\"\n\n"
            "[tool.poetry.group.dev.dependencies]\npytest = \"^8\"\n");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK(depends_on(pkgs[0], "fastapi"));
    CHECK(depends_on(pkgs[0], "pytest"));
    CHECK(!depends_on(pkgs[0], "python"));
}

// Version specifiers, extras and environment markers are not part of the name.
TEST(a_requirement_is_reduced_to_its_package_name) {
    Repo r("reqs");
    r.write("pyproject.toml",
            "[project]\nname = \"n\"\ndependencies = [\"flask[async]==3.0\", "
            "\"django ; python_version < '3.9'\", \"a-b.c>=1\"]\n");
    const auto pkgs = r.scan();
    CHECK(depends_on(pkgs[0], "flask"));
    CHECK(depends_on(pkgs[0], "django"));
    CHECK(depends_on(pkgs[0], "a-b.c"));
}

// PEP 503: `acme_store` and `acme-store` are the same distribution. Comparing them
// literally makes a package in the repo look like a third-party one, which is exactly
// the kind of quietly-wrong edge that discredits the graph.
TEST(package_names_normalize_so_internal_dependencies_resolve) {
    CHECK_EQ(watch::normalize("Acme_Store"), watch::normalize("acme-store"));
    CHECK_EQ(watch::normalize("a.b_c"), std::string("a-b-c"));

    Repo r("normalize");
    r.write("libs/store/pyproject.toml", "[project]\nname = \"acme_store\"\n");
    r.write("services/api/pyproject.toml",
            "[project]\nname = \"acme-api\"\ndependencies = [\"acme-store\"]\n");

    const auto  pkgs = r.scan();
    const auto* api  = find(pkgs, "acme-api");
    CHECK(api != nullptr);
    CHECK(depends_on(*api, "acme-store"));
    // The point: the dependency matches a package that is present under another spelling.
    const auto* store = find(pkgs, "acme_store");
    CHECK(store != nullptr);
    CHECK_EQ(watch::normalize(store->name), watch::normalize(api->deps[0].name));
}

// Two packages claiming one name would produce edges pointing at whichever happened to
// be scanned last. A missing edge is recoverable; a wrong one is not.
TEST(a_duplicate_package_name_is_dropped_rather_than_guessed_at) {
    Repo r("dupe");
    r.write("a/pyproject.toml", "[project]\nname = \"same\"\n");
    r.write("b/pyproject.toml", "[project]\nname = \"same\"\n");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].rel, std::string("a"));   // first by path, deterministically
}

// A repository that is one package has its manifest at the root.
TEST(a_single_package_repository_is_found_at_its_root) {
    Repo r("solo");
    r.write("pyproject.toml", "[project]\nname = \"solo\"\ndependencies = [\"click\"]\n");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].rel, std::string(""));
}

TEST(an_unparseable_manifest_is_skipped_rather_than_fatal) {
    Repo r("broken");
    r.write("a/package.json", "{ this is not json");
    r.write("b/package.json", R"({"name":"fine"})");
    const auto pkgs = r.scan();
    CHECK_EQ(pkgs.size(), 1u);
    CHECK_EQ(pkgs[0].name, std::string("fine"));
}
