#include "rgv/view/LabelLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace rgv::view {
namespace {

bool overlaps(const LabelBox& a, const LabelBox& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

} // namespace

float label_priority(const LabelRank& rank) {
    // Wide enough that no real node's degree fills a band, and small enough that the
    // whole range stays exactly representable in a float -- so a promoted node does not
    // quietly lose the degree that orders it against its equals.
    constexpr float kBand = 10000.0f;

    int band = 0;
    if (rank.on_path) band = 1;
    if (rank.linked) band = 2;
    if (rank.selected) band = 3;
    if (rank.hovered) band = 4;

    const float within = static_cast<float>(std::clamp(rank.degree, 0, 9999));
    return static_cast<float>(band) * kBand + within;
}

std::vector<int> choose_labels(const std::vector<LabelBox>& boxes) {
    std::vector<int> order(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (boxes[a].priority != boxes[b].priority) return boxes[a].priority > boxes[b].priority;
        return a < b;
    });

    // Binned, because the test is every candidate against everything already kept and a
    // monorepo offers hundreds of both. A cell is sized to a typical label, so a box
    // only ever meets the handful that could actually touch it.
    constexpr float kCell = 160.0f;
    std::unordered_map<std::int64_t, std::vector<int>> bins;
    auto key = [](int x, int y) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
    };

    std::vector<int> kept;
    kept.reserve(boxes.size());
    for (int i : order) {
        const LabelBox& box = boxes[i];
        const int lo_x = static_cast<int>(std::floor(box.min.x / kCell));
        const int hi_x = static_cast<int>(std::floor(box.max.x / kCell));
        const int lo_y = static_cast<int>(std::floor(box.min.y / kCell));
        const int hi_y = static_cast<int>(std::floor(box.max.y / kCell));

        bool clear = true;
        for (int cy = lo_y; cy <= hi_y && clear; ++cy) {
            for (int cx = lo_x; cx <= hi_x && clear; ++cx) {
                auto it = bins.find(key(cx, cy));
                if (it == bins.end()) continue;
                for (int other : it->second) {
                    if (overlaps(box, boxes[other])) { clear = false; break; }
                }
            }
        }
        if (!clear) continue;

        kept.push_back(i);
        for (int cy = lo_y; cy <= hi_y; ++cy) {
            for (int cx = lo_x; cx <= hi_x; ++cx) bins[key(cx, cy)].push_back(i);
        }
    }
    return kept;
}

} // namespace rgv::view
