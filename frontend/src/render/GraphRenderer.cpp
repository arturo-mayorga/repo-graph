#include "rgv/render/GraphRenderer.h"

#include "rgv/render/GL.h"

#include <cstring>

namespace rgv::render {
namespace {

using namespace rgv::gl;

const char* kEdgeVS = R"(#version 330 core
layout(location = 0) in vec2 a_corner;   // unit quad, x along the segment
layout(location = 1) in vec2 i_a;
layout(location = 2) in vec2 i_b;
layout(location = 3) in vec4 i_rgba;
layout(location = 4) in float i_width;
layout(location = 5) in float i_dash;

uniform mat4 u_vp;
uniform float u_zoom;

out vec4 v_rgba;
out float v_along;   // distance along the segment, in world units
out float v_dash;

void main() {
    vec2 d = i_b - i_a;
    float len = max(length(d), 1e-4);
    vec2 dir = d / len;
    vec2 nrm = vec2(-dir.y, dir.x);

    // Width is specified in pixels and converted here, so a hairline stays a hairline
    // when the user zooms out over a monorepo.
    float w = i_width / max(u_zoom, 1e-4);
    vec2 pos = i_a + dir * (a_corner.x * len) + nrm * (a_corner.y * w);

    v_rgba = i_rgba;
    v_along = a_corner.x * len;
    v_dash = i_dash;
    gl_Position = u_vp * vec4(pos, 0.0, 1.0);
}
)";

const char* kEdgeFS = R"(#version 330 core
in vec4 v_rgba;
in float v_along;
in float v_dash;
out vec4 frag;

void main() {
    if (v_dash > 0.0) {
        float phase = mod(v_along, v_dash * 2.0);
        if (phase > v_dash) discard;
    }
    frag = v_rgba;
}
)";

const char* kArrowVS = R"(#version 330 core
layout(location = 0) in vec2 a_corner;   // triangle in arrow-local space
layout(location = 1) in vec2 i_tip;
layout(location = 2) in vec2 i_dir;
layout(location = 3) in float i_size;
layout(location = 4) in vec4 i_rgba;

uniform mat4 u_vp;
uniform float u_zoom;

out vec4 v_rgba;

void main() {
    vec2 dir = normalize(i_dir);
    vec2 nrm = vec2(-dir.y, dir.x);
    float s = i_size / max(u_zoom, 1e-4);
    vec2 pos = i_tip + dir * (a_corner.x * s) + nrm * (a_corner.y * s);
    v_rgba = i_rgba;
    gl_Position = u_vp * vec4(pos, 0.0, 1.0);
}
)";

const char* kArrowFS = R"(#version 330 core
in vec4 v_rgba;
out vec4 frag;
void main() { frag = v_rgba; }
)";

const char* kNodeVS = R"(#version 330 core
layout(location = 0) in vec2 a_corner;   // unit quad in [-0.5, 0.5]
layout(location = 1) in vec2 i_center;
layout(location = 2) in vec2 i_half;
layout(location = 3) in vec4 i_fill;
layout(location = 4) in vec4 i_stroke;
layout(location = 5) in float i_stroke_w;
layout(location = 6) in float i_dash;
layout(location = 7) in float i_radius;

uniform mat4 u_vp;
uniform float u_zoom;

out vec2 v_local;      // position within the node, world units, centred
out vec2 v_half;
out vec4 v_fill;
out vec4 v_stroke;
out float v_stroke_w;
out float v_dash;
out float v_radius;
out float v_px;        // one screen pixel in world units, for antialiasing

void main() {
    // Pad the quad so the stroke and its antialiasing have room outside the body.
    float pad = (i_stroke_w / max(u_zoom, 1e-4)) + 2.0 / max(u_zoom, 1e-4);
    vec2 half_padded = i_half + vec2(pad);
    vec2 local = a_corner * 2.0 * half_padded;

    v_local = local;
    v_half = i_half;
    v_fill = i_fill;
    v_stroke = i_stroke;
    v_stroke_w = i_stroke_w / max(u_zoom, 1e-4);
    v_dash = i_dash;
    v_radius = i_radius;
    v_px = 1.0 / max(u_zoom, 1e-4);

    gl_Position = u_vp * vec4(i_center + local, 0.0, 1.0);
}
)";

const char* kNodeFS = R"(#version 330 core
in vec2 v_local;
in vec2 v_half;
in vec4 v_fill;
in vec4 v_stroke;
in float v_stroke_w;
in float v_dash;
in float v_radius;
in float v_px;
out vec4 frag;

float rounded_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    float r = min(v_radius, min(v_half.x, v_half.y));
    float d = rounded_box(v_local, v_half, r);

    float aa = v_px * 1.2;
    float inside = 1.0 - smoothstep(-aa, aa, d);
    float ring = 1.0 - smoothstep(v_stroke_w * 0.5 - aa, v_stroke_w * 0.5 + aa, abs(d));

    if (v_dash > 0.0) {
        // March the dash phase around the perimeter so the pattern reads as a
        // continuous dashed outline rather than four independent edges.
        float perim = atan(v_local.y, v_local.x) * (v_half.x + v_half.y);
        if (mod(perim, v_dash * 2.0) > v_dash) ring = 0.0;
    }

    vec4 body = vec4(v_fill.rgb, v_fill.a * inside);
    frag = mix(body, vec4(v_stroke.rgb, v_stroke.a), ring * v_stroke.a);
    if (frag.a < 0.004) discard;
}
)";

bool compile(GLenum type, const char* src, GLuint* out, std::string* err) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        if (err) *err += log;
        glDeleteShader(s);
        return false;
    }
    *out = s;
    return true;
}

bool link_program(const char* vs, const char* fs, GLuint* out, std::string* err) {
    GLuint v = 0, f = 0;
    if (!compile(GL_VERTEX_SHADER, vs, &v, err)) return false;
    if (!compile(GL_FRAGMENT_SHADER, fs, &f, err)) { glDeleteShader(v); return false; }

    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);

    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        if (err) *err += log;
        glDeleteProgram(p);
        glDeleteShader(v);
        glDeleteShader(f);
        return false;
    }
    glDetachShader(p, v);
    glDetachShader(p, f);
    glDeleteShader(v);
    glDeleteShader(f);
    *out = p;
    return true;
}

void attrib(GLuint index, GLint size, GLsizei stride, std::size_t offset, GLuint divisor) {
    glEnableVertexAttribArray(index);
    glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(offset));
    glVertexAttribDivisor(index, divisor);
}

void upload(GLuint vbo, const void* data, std::size_t bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // Orphan then refill: the instance set is rebuilt every frame, and orphaning lets
    // the driver hand back fresh storage instead of stalling on the in-flight buffer.
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_DRAW);
    if (bytes) glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), data);
}

} // namespace

bool GraphRenderer::init(std::string* error) {
    if (!link_program(kEdgeVS, kEdgeFS, &edge_prog_, error)) return false;
    if (!link_program(kArrowVS, kArrowFS, &arrow_prog_, error)) return false;
    if (!link_program(kNodeVS, kNodeFS, &node_prog_, error)) return false;

    // Edge/node base quad: a triangle strip in [0..1] x [-0.5..0.5] for edges, and the
    // node shader reinterprets the same corners as [-0.5..0.5] x [-0.5..0.5].
    const float quad[8] = {0.0f, -0.5f, 1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f};
    glGenBuffers(1, &quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    const float node_quad[8] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
    GLuint      node_quad_vbo = 0;
    glGenBuffers(1, &node_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, node_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(node_quad), node_quad, GL_STATIC_DRAW);

    // Arrowhead: tip at the origin, opening backwards along -x.
    const float tri[6] = {0.0f, 0.0f, -1.0f, 0.45f, -1.0f, -0.45f};
    glGenBuffers(1, &tri_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, tri_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);

    glGenBuffers(1, &edge_vbo_);
    glGenBuffers(1, &node_vbo_);
    glGenBuffers(1, &arrow_vbo_);

    // -- edges
    glGenVertexArrays(1, &edge_vao_);
    glBindVertexArray(edge_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    attrib(0, 2, 2 * sizeof(float), 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, edge_vbo_);
    {
        const GLsizei s = sizeof(EdgeInstance);
        attrib(1, 2, s, offsetof(EdgeInstance, a), 1);
        attrib(2, 2, s, offsetof(EdgeInstance, b), 1);
        attrib(3, 4, s, offsetof(EdgeInstance, rgba), 1);
        attrib(4, 1, s, offsetof(EdgeInstance, width), 1);
        attrib(5, 1, s, offsetof(EdgeInstance, dash), 1);
    }

    // -- arrows
    glGenVertexArrays(1, &arrow_vao_);
    glBindVertexArray(arrow_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, tri_vbo_);
    attrib(0, 2, 2 * sizeof(float), 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, arrow_vbo_);
    {
        const GLsizei s = sizeof(ArrowInstance);
        attrib(1, 2, s, offsetof(ArrowInstance, tip), 1);
        attrib(2, 2, s, offsetof(ArrowInstance, dir), 1);
        attrib(3, 1, s, offsetof(ArrowInstance, size), 1);
        attrib(4, 4, s, offsetof(ArrowInstance, rgba), 1);
    }

    // -- nodes
    glGenVertexArrays(1, &node_vao_);
    glBindVertexArray(node_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, node_quad_vbo);
    attrib(0, 2, 2 * sizeof(float), 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, node_vbo_);
    {
        const GLsizei s = sizeof(NodeInstance);
        attrib(1, 2, s, offsetof(NodeInstance, center), 1);
        attrib(2, 2, s, offsetof(NodeInstance, half), 1);
        attrib(3, 4, s, offsetof(NodeInstance, fill), 1);
        attrib(4, 4, s, offsetof(NodeInstance, stroke), 1);
        attrib(5, 1, s, offsetof(NodeInstance, stroke_w), 1);
        attrib(6, 1, s, offsetof(NodeInstance, dash), 1);
        attrib(7, 1, s, offsetof(NodeInstance, radius), 1);
    }

    glBindVertexArray(0);
    return true;
}

void GraphRenderer::shutdown() {
    const GLuint bufs[] = {quad_vbo_, tri_vbo_, edge_vbo_, node_vbo_, arrow_vbo_};
    glDeleteBuffers(5, bufs);
    const GLuint vaos[] = {edge_vao_, node_vao_, arrow_vao_};
    glDeleteVertexArrays(3, vaos);
    glDeleteProgram(edge_prog_);
    glDeleteProgram(node_prog_);
    glDeleteProgram(arrow_prog_);
}

void GraphRenderer::begin(const Camera& camera, const Vec4& clear) {
    vp_ = camera.view_projection();
    edges_.clear();
    arrows_.clear();
    nodes_.clear();
    stats_ = Stats{};

    glViewport(0, 0, static_cast<GLsizei>(camera.vw), static_cast<GLsizei>(camera.vh));
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    zoom_ = camera.zoom;
}

void GraphRenderer::add_edge(Vec2 a, Vec2 b, Vec4 c, float width, float dash) {
    EdgeInstance e{};
    e.a[0] = a.x; e.a[1] = a.y;
    e.b[0] = b.x; e.b[1] = b.y;
    e.rgba[0] = c.r; e.rgba[1] = c.g; e.rgba[2] = c.b; e.rgba[3] = c.a;
    e.width = width;
    e.dash  = dash;
    edges_.push_back(e);
}

void GraphRenderer::add_arrow(Vec2 tip, Vec2 dir, float size, Vec4 c) {
    ArrowInstance a{};
    a.tip[0] = tip.x; a.tip[1] = tip.y;
    a.dir[0] = dir.x; a.dir[1] = dir.y;
    a.size   = size;
    a.rgba[0] = c.r; a.rgba[1] = c.g; a.rgba[2] = c.b; a.rgba[3] = c.a;
    arrows_.push_back(a);
}

void GraphRenderer::add_node(Vec2 center, Vec2 half, Vec4 fill, Vec4 stroke,
                             float stroke_w, float dash, float radius) {
    NodeInstance n{};
    n.center[0] = center.x; n.center[1] = center.y;
    n.half[0]   = half.x;   n.half[1]   = half.y;
    n.fill[0]   = fill.r;   n.fill[1]   = fill.g;   n.fill[2] = fill.b;   n.fill[3] = fill.a;
    n.stroke[0] = stroke.r; n.stroke[1] = stroke.g; n.stroke[2] = stroke.b; n.stroke[3] = stroke.a;
    n.stroke_w  = stroke_w;
    n.dash      = dash;
    n.radius    = radius;
    nodes_.push_back(n);
}

void GraphRenderer::flush() {
    // Painter's order: edges under arrowheads under nodes, so a node body always
    // occludes the lines that terminate on it.
    if (!edges_.empty()) {
        glUseProgram(edge_prog_);
        glUniformMatrix4fv(glGetUniformLocation(edge_prog_, "u_vp"), 1, GL_FALSE, vp_.m);
        glUniform1f(glGetUniformLocation(edge_prog_, "u_zoom"), zoom_);
        upload(edge_vbo_, edges_.data(), edges_.size() * sizeof(EdgeInstance));
        glBindVertexArray(edge_vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(edges_.size()));
        ++stats_.draw_calls;
    }
    if (!arrows_.empty()) {
        glUseProgram(arrow_prog_);
        glUniformMatrix4fv(glGetUniformLocation(arrow_prog_, "u_vp"), 1, GL_FALSE, vp_.m);
        glUniform1f(glGetUniformLocation(arrow_prog_, "u_zoom"), zoom_);
        upload(arrow_vbo_, arrows_.data(), arrows_.size() * sizeof(ArrowInstance));
        glBindVertexArray(arrow_vao_);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, static_cast<GLsizei>(arrows_.size()));
        ++stats_.draw_calls;
    }
    if (!nodes_.empty()) {
        glUseProgram(node_prog_);
        glUniformMatrix4fv(glGetUniformLocation(node_prog_, "u_vp"), 1, GL_FALSE, vp_.m);
        glUniform1f(glGetUniformLocation(node_prog_, "u_zoom"), zoom_);
        upload(node_vbo_, nodes_.data(), nodes_.size() * sizeof(NodeInstance));
        glBindVertexArray(node_vao_);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(nodes_.size()));
        ++stats_.draw_calls;
    }
    glBindVertexArray(0);
    glUseProgram(0);

    stats_.edges += static_cast<int>(edges_.size());
    stats_.arrows += static_cast<int>(arrows_.size());
    stats_.nodes += static_cast<int>(nodes_.size());
    // Drawn is drawn. A frame may flush more than once -- containers under edges under
    // modules -- and what the first pass painted must not be painted again on top.
    edges_.clear();
    arrows_.clear();
    nodes_.clear();
}

} // namespace rgv::render
