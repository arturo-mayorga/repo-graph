#include "rgv/ecs/Resources.h"

namespace rgv::ecs {

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

} // namespace rgv::ecs
