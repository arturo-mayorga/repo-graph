// What an import resolved to, whatever language it was written in.
#pragma once

#include <string>

namespace rgv::watch {

struct FileImport {
    std::string to;               // repo-relative file the import resolves to
    int         line = 0;
    std::string snippet;
    bool        ambiguous = false;   // more than one candidate satisfied it -> heuristic
};

} // namespace rgv::watch
