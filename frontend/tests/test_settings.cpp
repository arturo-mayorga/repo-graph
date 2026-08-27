// Preference persistence. Every test uses an explicit temporary path -- a test that
// wrote to the real ~/.config would clobber the developer's own settings.
#include "TestMain.h"

#include "rgv/config/Settings.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace rgv;

namespace {

// Unique per test, and removed on scope exit even when a check throws.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag)
        : path(fs::temp_directory_path() / (std::string("rgv-test-") + tag)) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path file() const { return path / "settings.json"; }
};

void write_text(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::trunc);
    out << text;
}

// Restores the variable on scope exit so tests cannot leak environment into each other.
struct ScopedEnv {
    std::string name;
    std::string previous;
    bool        had_previous;

    ScopedEnv(const char* n, const char* value) : name(n) {
        const char* old = std::getenv(n);
        had_previous    = old != nullptr;
        if (old) previous = old;
        if (value) ::setenv(n, value, 1);
        else ::unsetenv(n);
    }
    ~ScopedEnv() {
        if (had_previous) ::setenv(name.c_str(), previous.c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

} // namespace

TEST(settings_round_trip_through_a_file) {
    TempDir tmp("roundtrip");

    config::Settings s;
    s.ui_text_scale    = 1.45f;
    s.graph_text_scale = 0.85f;
    CHECK(config::save(s, tmp.file()));
    CHECK(fs::exists(tmp.file()));

    const config::Settings back = config::load(tmp.file());
    CHECK(std::abs(back.ui_text_scale - 1.45f) < 1e-4f);
    CHECK(std::abs(back.graph_text_scale - 0.85f) < 1e-4f);
}

// The two sizes are independent: setting one must not disturb the other.
TEST(the_two_text_scales_are_stored_separately) {
    TempDir tmp("separate");

    config::Settings s;
    s.ui_text_scale    = 2.0f;
    s.graph_text_scale = 0.75f;
    CHECK(config::save(s, tmp.file()));

    const config::Settings back = config::load(tmp.file());
    CHECK(back.ui_text_scale > back.graph_text_scale);
}

// Files written before the split carry one `text_scale`. Honouring it for both beats
// silently resetting a preference the user already set.
TEST(a_legacy_single_text_scale_is_migrated_to_both) {
    TempDir tmp("legacy");
    write_text(tmp.file(), R"({"text_scale": 1.75})");

    const config::Settings s = config::load(tmp.file());
    CHECK(std::abs(s.ui_text_scale - 1.75f) < 1e-4f);
    CHECK(std::abs(s.graph_text_scale - 1.75f) < 1e-4f);
}

// When both the legacy key and the new ones are present, the specific keys win.
TEST(explicit_scales_override_the_legacy_key) {
    TempDir tmp("override");
    write_text(tmp.file(),
               R"({"text_scale": 1.75, "ui_text_scale": 1.1, "graph_text_scale": 0.9})");

    const config::Settings s = config::load(tmp.file());
    CHECK(std::abs(s.ui_text_scale - 1.1f) < 1e-4f);
    CHECK(std::abs(s.graph_text_scale - 0.9f) < 1e-4f);
}

// Losing a preference must never stop the app from starting.
TEST(a_missing_file_yields_defaults) {
    TempDir tmp("missing");
    const config::Settings s = config::load(tmp.path / "does-not-exist.json");
    CHECK_EQ(s.ui_text_scale, 1.0f);
    CHECK_EQ(s.graph_text_scale, 1.0f);
}

TEST(a_malformed_file_yields_defaults_instead_of_throwing) {
    TempDir tmp("malformed");
    write_text(tmp.file(), "{ this is not json ");
    const config::Settings s = config::load(tmp.file());
    CHECK_EQ(s.ui_text_scale, 1.0f);
    CHECK_EQ(s.graph_text_scale, 1.0f);
}

TEST(a_file_with_the_wrong_type_yields_defaults) {
    TempDir tmp("wrongtype");
    write_text(tmp.file(), R"({"ui_text_scale": "enormous"})");
    const config::Settings s = config::load(tmp.file());
    CHECK_EQ(s.ui_text_scale, 1.0f);
}

// A hand-edited file must not be able to make the UI unusable.
TEST(an_out_of_range_scale_is_clamped_on_load) {
    TempDir tmp("clamp");

    write_text(tmp.file(), R"({"ui_text_scale": 99.0, "graph_text_scale": 99.0})");
    CHECK_EQ(config::load(tmp.file()).ui_text_scale, config::Settings::kMaxTextScale);
    CHECK_EQ(config::load(tmp.file()).graph_text_scale, config::Settings::kMaxTextScale);

    write_text(tmp.file(), R"({"ui_text_scale": 0.0001, "graph_text_scale": 0.0001})");
    CHECK_EQ(config::load(tmp.file()).ui_text_scale, config::Settings::kMinTextScale);
    CHECK_EQ(config::load(tmp.file()).graph_text_scale, config::Settings::kMinTextScale);
}

TEST(sanitize_rejects_non_finite_values) {
    config::Settings s;
    s.ui_text_scale    = std::nan("");
    s.graph_text_scale = std::nan("");
    s.sanitize();
    CHECK_EQ(s.ui_text_scale, 1.0f);
    CHECK_EQ(s.graph_text_scale, 1.0f);
}

// Saving must work on a first run, when no config directory exists yet.
TEST(save_creates_missing_parent_directories) {
    TempDir tmp("mkdir");
    const fs::path nested = tmp.path / "a" / "b" / "settings.json";

    config::Settings s;
    s.ui_text_scale = 1.2f;
    CHECK(config::save(s, nested));
    CHECK(fs::exists(nested));
    CHECK(std::abs(config::load(nested).ui_text_scale - 1.2f) < 1e-4f);
}

// An interrupted write must not destroy settings that were already good.
TEST(saving_leaves_no_temporary_file_behind) {
    TempDir tmp("atomic");
    CHECK(config::save(config::Settings{}, tmp.file()));

    int leftovers = 0;
    for (const auto& e : fs::directory_iterator(tmp.path)) {
        if (e.path().extension() == ".tmp") ++leftovers;
    }
    CHECK_EQ(leftovers, 0);
}

// XDG Base Directory spec: honour XDG_CONFIG_HOME, else ~/.config.
TEST(settings_path_follows_the_xdg_base_directory_spec) {
    {
        ScopedEnv xdg("XDG_CONFIG_HOME", "/xdg-root");
        const fs::path p = config::settings_path();
        CHECK_EQ(p.string(), std::string("/xdg-root/rgv/settings.json"));
    }
    {
        ScopedEnv xdg("XDG_CONFIG_HOME", nullptr);
        ScopedEnv home("HOME", "/home/someone");
        const fs::path p = config::settings_path();
        CHECK_EQ(p.string(), std::string("/home/someone/.config/rgv/settings.json"));
    }
}

// An empty variable is not a valid path; the spec says treat it as unset.
TEST(an_empty_xdg_config_home_falls_back_to_home) {
    ScopedEnv xdg("XDG_CONFIG_HOME", "");
    ScopedEnv home("HOME", "/home/someone");
    CHECK_EQ(config::settings_path().string(),
             std::string("/home/someone/.config/rgv/settings.json"));
}
