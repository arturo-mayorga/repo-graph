// User preferences that outlive a session.
//
// Deliberately tiny. Panel geometry, filters, and which fixture was open are session
// state, not preferences -- restoring those would fight the user rather than help
// them. What belongs here is what someone sets once because of their eyesight or
// their display, and never wants to set again.
//
// Stored per the XDG Base Directory spec: $XDG_CONFIG_HOME/rgv/settings.json, falling
// back to ~/.config/rgv/settings.json.
#pragma once

#include <filesystem>
#include <string>
#include <string>

namespace rgv::config {

struct Settings {
    // Two independent text sizes, because they solve different problems.
    //
    // `ui_text_scale` is legibility: panel chrome, the inspector, the event log. It
    // scales the panels themselves, so bigger text takes space from the graph.
    //
    // `graph_text_scale` is density: node labels, and therefore node boxes, since a
    // node is sized to hold its label. Turning it up makes each node easier to read
    // and fits fewer of them on screen; turning it down does the opposite. Someone on
    // a large monorepo may well want big panel text and small graph text at once.
    float ui_text_scale    = 1.0f;
    float graph_text_scale = 1.0f;

    // What to run when a node is opened. Empty means the desktop's own launcher, which
    // is right until it isn't: a desktop whose handler for C++ source is a
    // `Terminal=true` entry -- nvim, helix, emacs -nw -- hands an editor to a process
    // with no terminal to run it in, and the file silently never opens. There is no way
    // for us to detect that from here, so this is how a user says what to run instead.
    //
    // A `{}` token is replaced by the absolute path; without one the path is appended.
    // Split on whitespace and exec'd directly -- there is no shell in this path, so a
    // space or a quote in a filename is part of the name and nothing more.
    //
    //     "open_command": "kitty -e nvim {}"
    //     "open_command": "code -g {}"
    std::string open_command;

    static constexpr float kMinTextScale = 0.70f;
    static constexpr float kMaxTextScale = 2.50f;

    // Clamps every field into its supported range. Applied on load, so a hand-edited
    // or corrupted file degrades to something usable instead of an unreadable UI.
    void sanitize();
};

// $XDG_CONFIG_HOME/rgv/settings.json, else $HOME/.config/rgv/settings.json.
// Falls back to a relative path only if neither variable is set.
std::filesystem::path settings_path();

// A missing, unreadable, or malformed file yields defaults -- never an error. Losing
// a preference is not worth failing to start over.
Settings load(const std::filesystem::path& path);

// Creates parent directories as needed. Writes to a temporary file and renames, so an
// interrupted write cannot leave a truncated settings file behind.
// Returns false on failure; the caller decides whether that is worth reporting.
bool save(const Settings& s, const std::filesystem::path& path);

} // namespace rgv::config
