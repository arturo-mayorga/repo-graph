// Just enough 2D math for a graph view. A whole matrix library would be a dependency
// paid for on every build to get an ortho projection and some vector adds.
#pragma once

#include <cmath>

namespace rgv {

struct Vec2 {
    float x = 0.0f, y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2  operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2  operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2  operator*(float s) const { return {x * s, y * s}; }
    Vec2  operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
};

inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
inline float length_sq(const Vec2& v) { return dot(v, v); }
inline float length(const Vec2& v) { return std::sqrt(length_sq(v)); }
inline Vec2  normalize(const Vec2& v) {
    const float l = length(v);
    return l > 1e-6f ? v / l : Vec2{0.0f, 0.0f};
}
inline Vec2 lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * t; }
inline Vec2 perp(const Vec2& v) { return {-v.y, v.x}; }

struct Vec4 {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

inline Vec4 mix(const Vec4& x, const Vec4& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t,
            x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

// Column-major 4x4, laid out for direct upload to glUniformMatrix4fv.
struct Mat4 {
    float m[16]{};

    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    // Maps [l,r] x [b,t] to clip space. `t < b` is fine and gives a y-down view,
    // which matches how screen coordinates read.
    static Mat4 ortho(float l, float r, float b, float t, float n = -1.0f, float f = 1.0f) {
        Mat4 o = identity();
        o.m[0]  = 2.0f / (r - l);
        o.m[5]  = 2.0f / (t - b);
        o.m[10] = -2.0f / (f - n);
        o.m[12] = -(r + l) / (r - l);
        o.m[13] = -(t + b) / (t - b);
        o.m[14] = -(f + n) / (f - n);
        return o;
    }
};

// World <-> screen for a pan/zoom 2D camera. Kept here so picking and rendering can
// never disagree about the transform.
//
// `anchor` is the screen point that `center` maps to. It defaults to the middle of the
// framebuffer, but the app moves it to the middle of the area not covered by panels,
// so "fit to view" frames the graph in the space the user can actually see.
struct Camera {
    Vec2  center{0.0f, 0.0f};
    float zoom = 1.0f;        // pixels per world unit
    float vw   = 1.0f;        // framebuffer size in pixels
    float vh   = 1.0f;
    Vec2  anchor{-1.0f, -1.0f};   // negative => use the framebuffer centre

    Vec2 anchor_point() const {
        return anchor.x >= 0.0f ? anchor : Vec2{vw * 0.5f, vh * 0.5f};
    }
    Vec2 world_to_screen(const Vec2& w) const {
        const Vec2 a = anchor_point();
        return {(w.x - center.x) * zoom + a.x, (w.y - center.y) * zoom + a.y};
    }
    Vec2 screen_to_world(const Vec2& s) const {
        const Vec2 a = anchor_point();
        return {(s.x - a.x) / zoom + center.x, (s.y - a.y) / zoom + center.y};
    }
    Mat4 view_projection() const {
        const Vec2  a = anchor_point();
        const float l = center.x - a.x / zoom;
        const float r = center.x + (vw - a.x) / zoom;
        const float t = center.y - a.y / zoom;
        const float b = center.y + (vh - a.y) / zoom;
        return Mat4::ortho(l, r, b, t);   // b > t: y grows downward, like the screen
    }
};

} // namespace rgv
