// What the walk descends into -- the provider's first and most load-bearing answer.
//
// Everything else is built on it. A directory the walk enters becomes nodes, and those
// nodes become packages, imports, targets and an architecture; a directory full of
// derived files therefore does not add noise to the graph, it adds a second graph.
// Pointing the viewer at this repository reported 24 packages, and all 24 of them were
// temp repositories the test suite had written under `build-headless/` -- a name the
// hardcoded skip list did not happen to contain.
//
// So the answer is not another name in the list. A repository already states which of
// its directories are derived, in `.gitignore`, and that statement is maintained by the
// people who work in it. The list stays as the floor for repositories that say nothing:
// `node_modules`, `.venv`, `__pycache__` are skipped because watching them costs more
// than they can ever be worth, not because a repository failed to mention them.
//
// The alternative weighed and rejected was a `build*` prefix match. It is one line, and
// it swallows `builder/` and `buildings/` -- real source with an unlucky spelling. The
// failure modes are not symmetric: showing a derived file is noise a filter can hide,
// while hiding a source file is the provider lying about what the repository contains,
// silently, with no way for the user to notice. Every choice below leans that way.
//
// THE SUBSET OF GITIGNORE THAT IS IMPLEMENTED
//
// Supported: comments and blank lines; a trailing `/` for directory-only; a leading `/`
// or an interior `/` for root-anchored; a bare name matched against the basename at any
// depth; `*` and `?` within one path segment; a leading `**/`, which is just the
// unanchored case spelled out; and `!` negation resolved by last match wins.
//
// Not supported, and a pattern using any of it is DROPPED rather than approximated:
// character classes (`[a-z]`), backslash escapes, and `**` anywhere but the front. Only
// the root `.gitignore` is read -- not per-directory ones, not `.git/info/exclude`, not
// `core.excludesFile`. Nor is the index consulted, so the git rule that a *tracked* file
// is never ignored is not reproduced; a repository that commits a file and later adds
// its name to `.gitignore` will see that file disappear from the graph. That is the one
// deviation that can hide source, it takes a deliberately contradictory repository to
// hit, and the fix if it ever matters is to ask git rather than to guess harder.
//
// Everything else omitted here fails in the safe direction: an unread pattern and a
// dropped one both mean the walk descends where git would not, which shows too much.
#pragma once

#include <string>
#include <vector>

namespace rgv::watch {

// Everything the provider decides not to look at, asked exactly once per entry.
class Ignores {
public:
    // Adds the patterns of one `.gitignore`, in file order. Order is meaningful: a
    // later line overrides an earlier one, which is the only thing that makes `!` mean
    // anything. Called again when the file changes; patterns accumulate, so the caller
    // rebuilds the object rather than appending to it.
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
