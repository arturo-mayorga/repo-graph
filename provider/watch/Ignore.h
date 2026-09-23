// What the walk descends into.
#pragma once

#include <string>
#include <vector>

namespace rgv::watch {

// Everything the provider decides not to look at, in one place, asked exactly once per
// entry: `skips(rel, is_dir)`.
class Ignores {
public:
    // Adds the patterns of one `.gitignore`, in file order.
    void add_gitignore(const std::string& text);

    // `rel` is repo-relative and `/`-separated; `is_dir` selects directory-only rules.
    bool skips(const std::string& rel, bool is_dir) const;

private:
    struct Rule {
        std::string glob;              // the pattern, trailing `/` and leading `!` removed
        bool        negate   = false;
        bool        dir_only = false;
        bool        anchored = false;  // matched against the whole rel path, not the name
    };
    std::vector<Rule> rules_;
};

} // namespace rgv::watch
