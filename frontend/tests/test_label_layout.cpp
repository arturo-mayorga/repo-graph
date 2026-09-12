// Which labels win the room beside a node.
//
// A name holds a constant screen size while the graph spreads out under it, so there is
// always a fixed amount of room and more names than room. The rule is priority order,
// and a label is kept only if it clears everything already kept.

#include "TestMain.h"

#include "rgv/view/LabelLayout.h"

#include <algorithm>

using rgv::Vec2;
using rgv::view::choose_labels;
using rgv::view::LabelBox;

namespace {

LabelBox at(float x, float y, float w, float h, float priority) {
    return LabelBox{Vec2{x, y}, Vec2{x + w, y + h}, priority};
}

bool kept(const std::vector<int>& chosen, int i) {
    return std::find(chosen.begin(), chosen.end(), i) != chosen.end();
}

} // namespace

TEST(labels_that_do_not_touch_are_all_drawn) {
    const auto chosen = choose_labels({at(0, 0, 50, 12, 1), at(200, 0, 50, 12, 2),
                                       at(0, 100, 50, 12, 3)});
    CHECK_EQ(chosen.size(), 3u);
}

TEST(the_higher_priority_label_wins_the_room) {
    // Two boxes on the same spot; only the busier node is named.
    const auto chosen = choose_labels({at(0, 0, 80, 12, 3), at(10, 4, 80, 12, 9)});
    CHECK_EQ(chosen.size(), 1u);
    CHECK(kept(chosen, 1));
    CHECK(!kept(chosen, 0));
}

// Greedy, and in priority order: the first two take the room and the third is left out
// even though dropping one of them would have fitted all three.
TEST(labels_are_taken_in_priority_order) {
    const auto chosen = choose_labels({at(0, 0, 100, 12, 5),     // 0: wins
                                       at(90, 0, 100, 12, 4),    // 1: overlaps 0, out
                                       at(190, 0, 100, 12, 3)}); // 2: clears 0, in
    CHECK_EQ(chosen.size(), 2u);
    CHECK(kept(chosen, 0));
    CHECK(!kept(chosen, 1));
    CHECK(kept(chosen, 2));
    CHECK_EQ(chosen[0], 0);   // chosen in priority order
}

// The same picture has to resolve the same way, or names flicker between equals.
TEST(equal_priorities_break_on_index_so_the_choice_is_stable) {
    const std::vector<LabelBox> boxes{at(0, 0, 80, 12, 1), at(10, 0, 80, 12, 1)};
    const auto first  = choose_labels(boxes);
    const auto second = choose_labels(boxes);
    CHECK_EQ(first.size(), 1u);
    CHECK(kept(first, 0));
    CHECK(first == second);
}

TEST(touching_at_the_edge_is_not_overlapping) {
    const auto chosen = choose_labels({at(0, 0, 50, 12, 1), at(50, 0, 50, 12, 1)});
    CHECK_EQ(chosen.size(), 2u);
}

// The binning must not lose a pair that spans several cells.
TEST(a_wide_label_still_blocks_one_far_along_it) {
    const auto chosen = choose_labels({at(0, 0, 900, 12, 9), at(800, 2, 60, 12, 1)});
    CHECK_EQ(chosen.size(), 1u);
    CHECK(kept(chosen, 0));
}

TEST(nothing_in_gives_nothing_out) {
    CHECK(choose_labels({}).empty());
}
