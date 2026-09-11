// Colour tokens. FR-32 requires changed / direct impact / transitive impact /
// unaffected context to be distinct visual states, and NFR-04 requires stale evidence
// never to read as current -- so those states are named here, once, and nothing else
// picks a colour literal.
#pragma once

#include "rgv/render/Math.h"

#include <string_view>

namespace rgv::ui {

struct Theme {
    Vec4 background{0.055f, 0.059f, 0.071f, 1.0f};
    Vec4 grid{0.10f, 0.11f, 0.13f, 1.0f};

    // Context: everything not implicated in the current change. Deliberately low
    // contrast, because the product's whole claim is that attention goes to impact.
    Vec4 node_fill{0.125f, 0.135f, 0.160f, 1.0f};
    Vec4 node_stroke{0.24f, 0.26f, 0.31f, 1.0f};
    Vec4 node_text{0.62f, 0.65f, 0.72f, 1.0f};

    // The four impact states, hue-separated rather than shade-separated so they stay
    // distinguishable at low zoom and for the most common colour-vision deficiencies.
    Vec4 changed{0.98f, 0.42f, 0.36f, 1.0f};       // the agent touched this
    Vec4 direct{0.98f, 0.68f, 0.24f, 1.0f};        // distance 1
    Vec4 transitive{0.42f, 0.62f, 0.95f, 1.0f};    // distance >= 2
    Vec4 seed_glow{0.98f, 0.42f, 0.36f, 0.18f};

    // Evidence quality. Stale gets its own colour AND a dashed stroke, because colour
    // alone is not enough to carry "do not trust this yet".
    Vec4 stale{0.73f, 0.60f, 0.30f, 1.0f};
    Vec4 pending{0.45f, 0.48f, 0.56f, 1.0f};
    Vec4 invalid{0.85f, 0.30f, 0.45f, 1.0f};
    Vec4 heuristic{0.58f, 0.50f, 0.78f, 1.0f};

    Vec4 edge{0.26f, 0.28f, 0.34f, 1.0f};
    Vec4 edge_impact{0.55f, 0.60f, 0.72f, 1.0f};
    // Symbols view: a file that constructs or invokes a symbol writes it, one that
    // merely names it reads it. Two edge colours, so the direction of data is visible
    // without reading a single label.
    Vec4 writes{0.86f, 0.52f, 0.30f, 1.0f};
    Vec4 reads{0.36f, 0.58f, 0.62f, 1.0f};
    // Dependency curves over the containment tree, drawn only for the node under the
    // pointer. Two colours, because "what this needs" and "what needs this" are
    // different questions and the answer is usually both at once.
    Vec4 dep_out{0.98f, 0.72f, 0.38f, 1.0f};       // the hovered node depends on it
    Vec4 dep_in{0.40f, 0.80f, 0.88f, 1.0f};        // it depends on the hovered node

    Vec4 path{1.00f, 0.85f, 0.35f, 1.0f};          // the explained dependency path
    Vec4 removed{0.55f, 0.28f, 0.32f, 1.0f};       // temporal compare: gone since baseline
    Vec4 added{0.30f, 0.65f, 0.45f, 1.0f};

    Vec4 selection{0.95f, 0.95f, 1.00f, 1.0f};
    Vec4 hover{0.70f, 0.75f, 0.85f, 1.0f};
};

const Theme& theme();

// Gource's signature: files coloured by extension, so a repository acquires a
// recognisable palette and you can read its composition at a glance. Curated for the
// common ones, hashed to a stable hue for everything else.
Vec4 extension_color(std::string_view name);

// Colour for a node at `distance` in the blast radius. distance < 0 => not impacted.
Vec4 impact_color(int distance);

// Applies the ImGui style that matches the theme.
void apply_imgui_style();

} // namespace rgv::ui
