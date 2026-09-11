#include "rgv/ecs/Resources.h"

namespace rgv::ecs {

const char* to_label(Relation r) {
    switch (r) {
        case Relation::All:     return "all";
        case Relation::Imports: return "imports";
        case Relation::Reads:   return "reads";
        case Relation::Writes:  return "writes";
    }
    return "all";
}

const char* to_label(ViewMode m) {
    switch (m) {
        case ViewMode::Architecture: return "Architecture";
        case ViewMode::Filesystem:   return "Filesystem";
        case ViewMode::FileGraph:    return "File graph";
    }
    return "?";
}

Level default_level(ViewMode m) {
    switch (m) {
        case ViewMode::Architecture: return Level::Package;
        default:                     return Level::File;
    }
}

bool add_hide_pattern(Filters& f, std::string source) {
    Filters::HidePattern p;
    p.source = std::move(source);
    try {
        p.re    = std::regex(p.source, std::regex::ECMAScript | std::regex::icase);
        p.valid = true;
    } catch (const std::regex_error&) {
        p.valid = false;
    }
    const bool ok = p.valid;
    f.hidden.push_back(std::move(p));
    return ok;
}

bool hidden_by_pattern(const Filters& f, const std::string& name, const std::string& path) {
    for (const auto& p : f.hidden) {
        if (!p.valid) continue;
        if (std::regex_search(name, p.re) || std::regex_search(path, p.re)) return true;
    }
    return false;
}

} // namespace rgv::ecs
