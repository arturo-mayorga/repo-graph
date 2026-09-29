#include "rgv/systems/TransportSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Resources.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace rgv::systems {

void TransportSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto& queue = world.resource<ecs::CommandQueue>();

    // Taken off the queue whatever happens -- including when there is no timeline to
    // move. The transport panel's buttons arrive here as commands, and a command this
    // system declines to drain is one CommandSystem would have to know about, which is
    // exactly the second owner this arrangement exists to avoid.
    std::vector<ecs::Command> mine;
    const auto is_mine = [](const ecs::Command& c) {
        return std::visit(
            [](const auto& v) {
                return ecs::is_transport_command_v<std::decay_t<decltype(v)>>;
            },
            c);
    };
    const auto split = std::stable_partition(queue.pending.begin(), queue.pending.end(),
                                             [&](const ecs::Command& c) { return !is_mine(c); });
    mine.assign(std::make_move_iterator(split), std::make_move_iterator(queue.pending.end()));
    queue.pending.erase(split, queue.pending.end());

    auto& handle = world.resource<ecs::SourceHandle>();
    if (!handle.source) return;

    Timeline* timeline = handle.source->timeline();
    if (!timeline) return;   // a live source has no transport, and that is fine

    const auto& input = world.resource<ecs::FrameInput>();
    if (input.play_pressed) timeline->playing() ? timeline->pause() : timeline->play();
    if (input.step_pressed) timeline->step_event();
    if (input.restart_pressed) timeline->restart();

    // The same three verbs, plus the two the keyboard has no key for, arriving from the
    // transport panel instead of from the keys.
    for (const auto& command : mine) {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, ecs::TransportPlayPause>) {
                    timeline->playing() ? timeline->pause() : timeline->play();
                } else if constexpr (std::is_same_v<T, ecs::TransportStep>) {
                    timeline->step_event();
                } else if constexpr (std::is_same_v<T, ecs::TransportRestart>) {
                    timeline->restart();
                } else if constexpr (std::is_same_v<T, ecs::TransportSeek>) {
                    // Scrubbing is not watching: landing somewhere and immediately
                    // playing on from it loses the frame the user was aiming at.
                    timeline->pause();
                    timeline->seek_ms(c.ms);
                } else if constexpr (std::is_same_v<T, ecs::TransportRate>) {
                    timeline->set_rate(c.rate);
                }
            },
            command);
    }
}

} // namespace rgv::systems
