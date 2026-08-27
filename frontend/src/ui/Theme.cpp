#include "rgv/ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace rgv::ui {

const Theme& theme() {
    static const Theme t;
    return t;
}

Vec4 impact_color(int distance) {
    const Theme& t = theme();
    if (distance < 0) return t.node_stroke;
    if (distance == 0) return t.changed;
    if (distance == 1) return t.direct;
    // Fade with distance so depth is legible without a legend, but never all the way
    // to context colour -- an impacted node must always look impacted.
    const float f = std::min(1.0f, (distance - 2) / 5.0f) * 0.45f;
    return mix(t.transitive, t.node_stroke, f);
}

} // namespace rgv::ui
