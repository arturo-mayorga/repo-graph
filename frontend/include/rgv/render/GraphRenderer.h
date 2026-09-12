// Instanced 2D renderer for the graph.
//
// Three instanced passes -- edges, arrowheads, nodes -- each one draw call regardless
// of element count. Node bodies are signed-distance rounded rectangles evaluated in
// the fragment shader, so a node stays crisp at any zoom without re-tessellating.
//
// Text is deliberately NOT here. Labels are drawn through Dear ImGui's draw list from
// projected positions; a glyph atlas is a real subsystem and ImGui already ships one.
#pragma once

#include "rgv/render/Math.h"

#include <string>
#include <vector>

namespace rgv::render {

class GraphRenderer {
public:
    struct Stats {
        int edges = 0, nodes = 0, arrows = 0, draw_calls = 0;
    };

    bool init(std::string* error);
    void shutdown();

    // Clears and sets up the camera for this frame.
    void begin(const Camera& camera, const Vec4& clear);

    // Widths and sizes are in SCREEN PIXELS. The shaders divide by the zoom
    // themselves, which is what keeps a hairline a hairline over a monorepo -- so a
    // caller that divides by the zoom as well gets a line that grows without bound as
    // the user zooms out. Positions are world units; only these two are not.
    void add_edge(Vec2 a, Vec2 b, Vec4 color, float width_px, float dash_world = 0.0f);
    void add_arrow(Vec2 tip, Vec2 dir, float size_px, Vec4 color);
    // `dash > 0` outlines the node with a dashed stroke, which is how stale and
    // heuristic evidence is marked without relying on colour alone.
    void add_node(Vec2 center, Vec2 half, Vec4 fill, Vec4 stroke, float stroke_w,
                  float dash = 0.0f, float radius = 5.0f);

    void         flush();
    const Stats& stats() const { return stats_; }

private:
    struct EdgeInstance { float a[2], b[2], rgba[4], width, dash; };
    struct ArrowInstance { float tip[2], dir[2], size, rgba[4]; };
    struct NodeInstance {
        float center[2], half[2], fill[4], stroke[4], stroke_w, dash, radius;
    };

    unsigned int edge_prog_ = 0, node_prog_ = 0, arrow_prog_ = 0;
    unsigned int edge_vao_ = 0, node_vao_ = 0, arrow_vao_ = 0;
    unsigned int quad_vbo_ = 0, tri_vbo_ = 0;
    unsigned int edge_vbo_ = 0, node_vbo_ = 0, arrow_vbo_ = 0;

    std::vector<EdgeInstance>  edges_;
    std::vector<ArrowInstance> arrows_;
    std::vector<NodeInstance>  nodes_;

    Mat4  vp_{};
    float zoom_ = 1.0f;
    Stats stats_;
};

} // namespace rgv::render
