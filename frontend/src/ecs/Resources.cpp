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

} // namespace rgv::ecs
