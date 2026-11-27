#pragma once

// L1 skills as cancellable jobs (docs/DESIGN.md 6). A job is stepped once per tick on the server
// thread and never keeps game pointers between ticks: the player is passed in every step and
// targets are kept as ids or positions.

#include "core/Job.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/level/BlockPos.h"

#include <memory>
#include <optional>
#include <string>

class SimulatedPlayer;

namespace karakuri {

class Job {
public:
    virtual ~Job() = default;

    [[nodiscard]] virtual std::string describe() const = 0;

    // Advances the job by one tick.
    virtual JobStep step(SimulatedPlayer& sp) = 0;

    // Stops whatever the job asked the engine to do. A paused job restarts its engine actions on
    // the next step (used when an L0 reflex takes over and then hands control back).
    virtual void pause(SimulatedPlayer& sp);
    virtual void cancel(SimulatedPlayer& sp) { pause(sp); }

protected:
    bool mStarted{false};
    int  mTicks{0};
};

// Pathfinder movement (src/core/Pathfinder.h): walks, digs, bridges and pillars as needed.
std::unique_ptr<Job> makePathGoto(BlockPos target, float range = 0);
// Moves until `block` is within `reach` of the eyes.
std::unique_ptr<Job> makeApproach(BlockPos block, float reach);
std::unique_ptr<Job> makeMine(BlockPos pos);
std::unique_ptr<Job> makePlace(BlockPos pos);
std::unique_ptr<Job> makeEat();
// Attacks `target`, or the nearest monster within `radius` when no target is given.
std::unique_ptr<Job> makeAttack(std::optional<ActorUniqueID> target, float radius = 16.0f);

} // namespace karakuri
