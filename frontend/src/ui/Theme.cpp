#include "rgv/ui/Theme.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <cmath>

namespace rgv::ui {

const Theme& theme() {
    static const Theme t;
    return t;
}

namespace {

Vec4 from_hsv(float h, float s, float v) {
    const float c = v * s;
    const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float       r = 0, g = 0, b = 0;
    if (h < 60)       { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return Vec4{r + m, g + m, b + m, 1.0f};
}

} // namespace

Vec4 extension_color(std::string_view name) {
    const auto dot = name.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? name : name.substr(dot + 1);

    // Curated so the languages this product cares about read consistently; everything
    // else gets a stable hue rather than a lookup miss.
    struct Known { std::string_view ext; float h, s, v; };
    static constexpr Known kKnown[] = {
        {"ts", 205.0f, 0.62f, 0.92f},   {"tsx", 195.0f, 0.60f, 0.95f},
        {"js", 50.0f, 0.72f, 0.92f},    {"jsx", 42.0f, 0.70f, 0.94f},
        {"json", 32.0f, 0.55f, 0.80f},  {"md", 0.0f, 0.00f, 0.72f},
        {"css", 275.0f, 0.50f, 0.88f},  {"html", 15.0f, 0.65f, 0.88f},
        {"cpp", 145.0f, 0.55f, 0.82f},  {"h", 160.0f, 0.45f, 0.76f},
        {"hpp", 160.0f, 0.45f, 0.76f},  {"c", 150.0f, 0.50f, 0.80f},
        {"py", 220.0f, 0.55f, 0.88f},   {"rs", 22.0f, 0.60f, 0.82f},
        {"go", 188.0f, 0.58f, 0.88f},   {"yml", 300.0f, 0.35f, 0.74f},
        {"yaml", 300.0f, 0.35f, 0.74f}, {"toml", 310.0f, 0.35f, 0.74f},
    };
    for (const auto& k : kKnown) {
        if (k.ext == ext) return from_hsv(k.h, k.s, k.v);
    }

    std::uint32_t h = 2166136261u;
    for (unsigned char ch : ext) { h ^= ch; h *= 16777619u; }
    return from_hsv(static_cast<float>(h % 360u), 0.45f, 0.80f);
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
