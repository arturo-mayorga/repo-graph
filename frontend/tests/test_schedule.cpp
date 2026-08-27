// The ECS itself: the world, the resource store, and the schedule.
//
// These are the invariants the rest of the design rests on. If phase ordering or
// resource lookup is wrong, every system downstream is wrong in a way that looks like
// a rendering bug.
#include "TestMain.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ecs/System.h"
#include "rgv/ecs/World.h"

#include <memory>
#include <string>
#include <vector>

using namespace rgv;

namespace {

// Records the order it ran in, so ordering can be asserted rather than assumed.
class Recorder final : public ecs::System {
public:
    Recorder(std::string label, std::vector<std::string>& log) : label_(std::move(label)), log_(log) {}

    std::string_view name() const override { return label_; }
    void             setup(ecs::World&) override { log_.push_back("setup:" + label_); }
    void             run(ecs::World&, const ecs::FrameContext&) override { log_.push_back(label_); }
    void             teardown(ecs::World&) override { log_.push_back("teardown:" + label_); }

private:
    std::string               label_;
    std::vector<std::string>& log_;
};

std::unique_ptr<ecs::System> rec(const char* label, std::vector<std::string>& log) {
    return std::make_unique<Recorder>(label, log);
}

} // namespace

// -- resources ---------------------------------------------------------------

TEST(resources_are_stored_and_retrieved_by_type) {
    ecs::World world;
    world.add_resource<ecs::Filters>().max_impact_depth = 5;
    CHECK_EQ(world.resource<ecs::Filters>().max_impact_depth, 5);
    CHECK(world.has_resource<ecs::Filters>());
    CHECK(!world.has_resource<ecs::Selection>());
}

// A resource is a singleton: asking twice must hand back the same object, or systems
// would each mutate their own copy and none of them would see the others.
TEST(a_resource_is_a_singleton) {
    ecs::World world;
    world.add_resource<ecs::Selection>();
    world.resource<ecs::Selection>().node = "pkg:a";
    CHECK_EQ(world.resource<ecs::Selection>().node, std::string("pkg:a"));
    CHECK(&world.resource<ecs::Selection>() == &world.resource<ecs::Selection>());
}

TEST(an_optional_resource_reports_absence_rather_than_throwing) {
    ecs::World world;
    CHECK(world.find_resource<ecs::Filters>() == nullptr);
    world.add_resource<ecs::Filters>();
    CHECK(world.find_resource<ecs::Filters>() != nullptr);
}

// -- schedule ----------------------------------------------------------------

// Phases order the frame; insertion order orders within a phase. Both matter: panels
// must draw before the graph, and input must be read before anything acts on it.
TEST(systems_run_in_phase_order_then_insertion_order) {
    std::vector<std::string> log;
    ecs::World               world;
    ecs::Schedule            schedule;

    // Deliberately attached out of phase order.
    schedule.add(ecs::Phase::Render, rec("render-a", log))
        .add(ecs::Phase::Input, rec("input-a", log))
        .add(ecs::Phase::Render, rec("render-b", log))
        .add(ecs::Phase::Input, rec("input-b", log))
        .add(ecs::Phase::Present, rec("present", log));

    schedule.setup(world);
    log.clear();
    schedule.run(world, ecs::FrameContext{});

    const std::vector<std::string> expected{"input-a", "input-b", "render-a", "render-b",
                                            "present"};
    CHECK(log == expected);
}

TEST(setup_runs_in_the_same_order_as_the_frame) {
    std::vector<std::string> log;
    ecs::World               world;
    ecs::Schedule            schedule;
    schedule.add(ecs::Phase::Render, rec("render", log)).add(ecs::Phase::Input, rec("input", log));
    schedule.setup(world);

    const std::vector<std::string> expected{"setup:input", "setup:render"};
    CHECK(log == expected);
}

// Reverse order, so a system can rely on what it was set up alongside still existing
// while it tears down -- the renderer needs its GL context until it is finished.
TEST(teardown_runs_in_reverse_order) {
    std::vector<std::string> log;
    ecs::World               world;
    ecs::Schedule            schedule;
    schedule.add(ecs::Phase::Input, rec("input", log)).add(ecs::Phase::Render, rec("render", log));
    schedule.setup(world);
    log.clear();
    schedule.teardown(world);

    const std::vector<std::string> expected{"teardown:render", "teardown:input"};
    CHECK(log == expected);
}

// The listing is what makes the frame inspectable from outside -- and is what `rgv
// --schedule` prints.
TEST(the_listing_names_every_system_in_execution_order) {
    std::vector<std::string> log;
    ecs::World               world;
    ecs::Schedule            schedule;
    schedule.add(ecs::Phase::Simulate, rec("layout", log)).add(ecs::Phase::Ingest, rec("source", log));
    schedule.setup(world);

    const std::vector<std::string> expected{"Ingest/source", "Simulate/layout"};
    CHECK(schedule.listing() == expected);
    CHECK_EQ(schedule.size(), std::size_t{2});
}

// -- commands ----------------------------------------------------------------

TEST(the_command_queue_preserves_the_order_things_were_asked_for) {
    ecs::CommandQueue queue;
    queue.push(ecs::SelectNode{"a"});
    queue.push(ecs::ClearSelection{});
    queue.push(ecs::SelectNode{"b"});

    CHECK_EQ(queue.pending.size(), std::size_t{3});
    CHECK(std::holds_alternative<ecs::SelectNode>(queue.pending[0]));
    CHECK(std::holds_alternative<ecs::ClearSelection>(queue.pending[1]));
    CHECK_EQ(std::get<ecs::SelectNode>(queue.pending[2]).id, std::string("b"));
}
