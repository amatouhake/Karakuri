// Getting through the night: sleeping in a bed, or walling in. Same rules as players: beds work
// only at night, blocks come from the inventory.

#include "mod/Jobs.h"

#include "mod/BotManager.h"
#include "mod/Game.h"
#include "mod/Mod.h"
#include "mod/World.h"

#include "ll/api/service/Bedrock.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include <cmath>
#include <optional>

namespace karakuri {

namespace {

Vec3 center(BlockPos const& p) { return {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}; }

BlockPos feetBlock(SimulatedPlayer& sp) {
    auto f = sp.getFeetPos();
    return {(int)std::floor(f.x), (int)std::floor(f.y + 0.5f), (int)std::floor(f.z)};
}

std::optional<BlockPos> nearestBed(SimulatedPlayer& sp, int radius) {
    auto&                   region = sp.getDimensionBlockSource();
    auto                    c      = feetBlock(sp);
    std::optional<BlockPos> best;
    int                     bestD = 1 << 30;
    for (int x = -radius; x <= radius; ++x)
        for (int y = -radius; y <= radius; ++y)
            for (int z = -radius; z <= radius; ++z) {
                BlockPos q{c.x + x, c.y + y, c.z + z};
                int      d = x * x + y * y + z * z;
                if (d >= bestD || !region.getChunkAt(q)) continue;
                if (region.getBlock(q).getTypeName() == "minecraft:bed") {
                    best  = q;
                    bestD = d;
                }
            }
    return best;
}

class SleepJob final : public Job {
public:
    std::string describe() const override { return "sleep"; }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        if (mInner) mInner->pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        if (++mTicks > 20 * 60 * 10) return JobStep::fail("timed out");
        if (mSlept) {
            if (sp.isSleeping()) return JobStep::running();
            auto level = ll::service::getLevel();
            bool night = level && (level->getTime() % 24000) >= 12542 && (level->getTime() % 24000) < 23460;
            return JobStep::done(night ? "woke up (still night)" : "slept until morning");
        }
        if (!BotManager::instance().isNight()) return JobStep::fail("beds only work at night");
        if (mInner) {
            auto r = mInner->step(sp);
            if (!r.finished()) return JobStep::running();
            mInner.reset();
            if (r.state == JobState::Failed) return JobStep::fail("could not reach a bed: " + r.reason);
        }
        auto bed = nearestBed(sp, 16);
        if (!bed) {
            if (mPlaced++ > 0) return JobStep::fail("placing a bed failed");
            int slot = -1;
            auto& inv = sp.getInventory();
            for (int i = 0; i < 9; ++i)
                if (!inv.getItem(i).isNull() && inv.getItem(i).getTypeName() == "minecraft:bed") slot = i;
            if (slot < 0) return JobStep::fail("no bed within 16 blocks and none in the hotbar");
            // A bed takes two blocks and extends away from the player who places it: find a cell
            // and the one beyond it (seen from the bot), both open with solid ground, at the
            // bot's level or one step up or down.
            auto& region = sp.getDimensionBlockSource();
            auto  f      = feetBlock(sp);
            for (int d = 1; d <= 3; ++d)
                for (auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                    for (int o = -1; o <= 1; ++o)
                        for (int dy : {0, -1, 1}) {
                            BlockPos q{f.x + dx * d + dz * o, f.y + dy, f.z + dz * d + dx * o};
                            BlockPos q2{q.x + dx, q.y, q.z + dz};
                            if (isAir(region, q) && isAir(region, q2) && isAir(region, {q.x, q.y + 1, q.z})
                                && isSolid(region, {q.x, q.y - 1, q.z}) && isSolid(region, {q2.x, q2.y - 1, q2.z})) {
                                sp.setSelectedSlot(slot);
                                mInner = makePlace(q);
                                return JobStep::running();
                            }
                        }
            return JobStep::fail("no flat open spot within 3 blocks to place the bed");
        }
        // Beds only accept a player standing close by (much closer than block reach).
        constexpr float kBedReach = 2.5f;
        if (!withinReach(sp, center(*bed), kBedReach)) {
            if (mApproached++ > 2) return JobStep::fail("could not get next to the bed");
            mInner = makeApproach(*bed, kBedReach - 0.3f);
            return JobStep::running();
        }
        sp.simulateLookAt(center(*bed), sim::LookDuration::Instant);
        sp.simulateInteract(*bed, ScriptModuleMinecraft::ScriptFacing::Up);
        if (++mTries > 40) return JobStep::fail("could not lie down (monsters nearby, or the bed is blocked)");
        if (sp.isSleeping()) {
            mSlept = true;
            logger().info("{} is sleeping at ({},{},{})", sp.getRealName(), bed->x, bed->y, bed->z);
        }
        return JobStep::running();
    }

private:
    std::unique_ptr<Job> mInner;
    bool                 mSlept{false};
    int                  mPlaced{0};
    int                  mApproached{0};
    int                  mTries{0};
};

class ShelterJob final : public Job {
public:
    std::string describe() const override { return "shelter"; }

    JobStep step(SimulatedPlayer& sp) override {
        auto& region = sp.getDimensionBlockSource();
        if (!mStarted) {
            if (!sp.isOnGround()) return JobStep::running();
            mFeet = feetBlock(sp);
            // Stand in the middle of the cell first: a bot overlapping a wall cell blocks placing there.
            sp.simulateMoveToLocation(Vec3{mFeet.x + 0.5f, (float)mFeet.y, mFeet.z + 0.5f}, 1.0f, false);
            mStarted = true;
            return JobStep::running();
        }
        if (mTicks < 10) {
            ++mTicks;
            return JobStep::running();
        }
        if (++mTicks % 5 != 0) return JobStep::running();
        // Cells to close, in an order where each one has something to be placed against.
        BlockPos const f = mFeet;
        BlockPos const cells[] = {
            {f.x + 1, f.y, f.z}, {f.x - 1, f.y, f.z}, {f.x, f.y, f.z + 1}, {f.x, f.y, f.z - 1},
            {f.x + 1, f.y + 1, f.z}, {f.x - 1, f.y + 1, f.z}, {f.x, f.y + 1, f.z + 1}, {f.x, f.y + 1, f.z - 1},
            {f.x + 1, f.y + 2, f.z}, // a corner block for the roof to attach to
            {f.x, f.y + 2, f.z},
        };
        int open = 0;
        for (auto const& c : cells) {
            auto k = classify(region.getBlock(c));
            if (k != path::CellKind::Air && k != path::CellKind::Passable) continue;
            ++open;
            int slot = scaffoldSlot(sp);
            if (slot < 0) return JobStep::fail(fmt::format("out of blocks with {} gaps left", open));
            // Place against any solid neighbor of the cell (not the bot's own cells).
            static constexpr int dirs[6][3] = {{0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}};
            using F                         = ScriptModuleMinecraft::ScriptFacing;
            static constexpr F faces[6]     = {F::Up, F::West, F::East, F::North, F::South, F::Down};
            bool placed = false;
            for (int d = 0; d < 6 && !placed; ++d) {
                BlockPos n{c.x + dirs[d][0], c.y + dirs[d][1], c.z + dirs[d][2]};
                // Clicking a bed, chest or door uses it instead of placing against it.
                if (!isSolid(region, n) || interactive(blockName(region, n))) continue;
                Vec3 hit{0.5f - dirs[d][0] * 0.5f, 0.5f - dirs[d][1] * 0.5f, 0.5f - dirs[d][2] * 0.5f};
                sp.setSelectedSlot(slot);
                sp.simulateLookAt(center(c), sim::LookDuration::Instant);
                sp.simulateUseItemInSlotOnBlock(slot, n, faces[d], hit);
                placed = true;
            }
            if (placed) return JobStep::running(); // one block per step; checked on the next round
            ++mUnsupported; // nothing to place against: try the next gap
        }
        mUnsupported = 0;
        // Done once closed, or once only unsupported gaps are left (reported, so the agent can fix
        // them). The bot simply stays inside afterwards.
        if (open == 0) return JobStep::done(fmt::format("sheltered at ({},{},{})", f.x, f.y, f.z));
        if (mTicks > 20 * 15)
            return JobStep::fail(fmt::format("{} gaps had nothing to place against at ({},{},{})", open, f.x, f.y, f.z));
        return JobStep::running();
    }

private:
    static bool interactive(std::string const& n) {
        for (auto const* k : {"bed", "chest", "crafting_table", "furnace", "door", "barrel", "lever", "button", "anvil"})
            if (n.find(k) != std::string::npos) return true;
        return false;
    }

    BlockPos mFeet;
    bool     mClosed{false};
    int      mUnsupported{0};
};

} // namespace

std::unique_ptr<Job> makeSleep() { return std::make_unique<SleepJob>(); }
std::unique_ptr<Job> makeShelter() { return std::make_unique<ShelterJob>(); }

} // namespace karakuri
