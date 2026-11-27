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
// Crafts `count` of `item` (e.g. "oak_planks"). Recipes needing a 3x3 grid use a crafting table
// within reach: walks to one nearby, or places one from the hotbar.
std::unique_ptr<Job> makeCraft(std::string item, int count);
// Smelts `count` of `item` in a furnace nearby (or one placed from the hotbar), adding fuel from
// the inventory, and takes the result.
std::unique_ptr<Job> makeSmelt(std::string item, int count);
// Moves items between the inventory and a chest (the nearest one, or the one at `pos`).
// An empty `item` means any item; count <= 0 means all.
std::unique_ptr<Job> makeChest(bool put, std::string item, int count, std::optional<BlockPos> pos);
// Walks over dropped items within `radius` until none are left (or none can be reached).
std::unique_ptr<Job> makeCollect(float radius = 8.0f);
// Attacks `target`, or the nearest monster within `radius` when no target is given.
std::unique_ptr<Job> makeAttack(std::optional<ActorUniqueID> target, float radius = 16.0f);

} // namespace karakuri
