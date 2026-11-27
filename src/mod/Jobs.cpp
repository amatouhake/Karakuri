#include "mod/Jobs.h"

#include "mod/Game.h"

#include "ll/api/service/Bedrock.h"

#include "mc/scripting/modules/gametest/ScriptNavigationResult.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"

namespace karakuri {

void Job::pause(SimulatedPlayer& sp) {
    sp.simulateStopMoving();
    sp.simulateStopDestroyingBlock();
    sp.simulateStopUsingItem();
    mStarted = false;
}

namespace {

Vec3 center(BlockPos const& p) { return {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}; }

// Walks with the engine's navigation. It only walks: no digging, pillaring or bridging
// (docs/SPIKE-0.md), and it sometimes finds no path to reachable targets.
class GotoJob final : public Job {
public:
    GotoJob(Vec3 target, float tolerance) : mTarget(target), mTolerance(tolerance) {}

    std::string describe() const override {
        return fmt::format("goto ({:.1f},{:.1f},{:.1f})", mTarget.x, mTarget.y, mTarget.z);
    }

    JobStep step(SimulatedPlayer& sp) override {
        float dist = sp.getFeetPos().distanceTo(mTarget);
        if (dist <= mTolerance) {
            sp.simulateStopMoving();
            return JobStep::done();
        }
        if (!mStarted) {
            auto res = sp.simulateNavigateToLocation(mTarget, 1.0f);
            if (res.mPath->empty()) return JobStep::fail("no path to target");
            mPartial = !res.mIsFullPath;
            mStarted = true;
            mWatch.reset(dist);
        }
        if (++mTicks > mMaxTicks) return JobStep::fail("timed out");
        if (mWatch.update(dist)) {
            sp.simulateStopMoving();
            return JobStep::fail(
                mPartial ? fmt::format("target unreachable on foot; stopped {:.1f} blocks away", dist)
                         : fmt::format("stuck {:.1f} blocks away", dist)
            );
        }
        return JobStep::running();
    }

private:
    Vec3          mTarget;
    float         mTolerance;
    bool          mPartial{false};
    int           mMaxTicks{20 * 120};
    ProgressWatch mWatch{40, 0.3};
};

class MineJob final : public Job {
public:
    explicit MineJob(BlockPos pos) : mPos(pos) {}

    std::string describe() const override { return fmt::format("mine ({},{},{})", mPos.x, mPos.y, mPos.z); }

    JobStep step(SimulatedPlayer& sp) override {
        auto& region = sp.getDimensionBlockSource();
        if (!mStarted) {
            if (isAir(region, mPos)) return mBlock.empty() ? JobStep::fail("block is air") : JobStep::done();
            if (!withinReach(sp, center(mPos))) return JobStep::fail("out of reach");
            mBlock = blockName(region, mPos);
            sp.simulateLookAt(center(mPos), sim::LookDuration::Instant);
            if (!sp.simulateDestroyBlock(mPos, faceToward(mPos, sp.getEyePos())))
                return JobStep::fail("engine refused to start breaking");
            mStarted = true;
        }
        if (blockName(region, mPos) != mBlock) return JobStep::done(fmt::format("broke {}", mBlock));
        if (!withinReach(sp, center(mPos))) {
            sp.simulateStopDestroyingBlock();
            return JobStep::fail("moved out of reach");
        }
        // Stone by hand takes 150 ticks; obsidian with a diamond pickaxe about 190.
        if (++mTicks > 20 * 30) {
            sp.simulateStopDestroyingBlock();
            return JobStep::fail("timed out");
        }
        if (!sp.isSimulatingDestroyingBlock()) mStarted = false; // knocked out of it: start again
        return JobStep::running();
    }

private:
    BlockPos    mPos;
    std::string mBlock;
};

class PlaceJob final : public Job {
public:
    explicit PlaceJob(BlockPos pos) : mPos(pos) {}

    std::string describe() const override { return fmt::format("place ({},{},{})", mPos.x, mPos.y, mPos.z); }

    JobStep step(SimulatedPlayer& sp) override {
        auto& region = sp.getDimensionBlockSource();
        if (mClicked) {
            // The engine reports success even when nothing was placed, so check the world.
            if (!isAir(region, mPos)) return JobStep::done(fmt::format("placed {}", blockName(region, mPos)));
            return JobStep::fail("nothing was placed");
        }
        if (sp.getSelectedItem().isNull()) return JobStep::fail("nothing in hand");
        if (!isAir(region, mPos)) return JobStep::fail(fmt::format("occupied by {}", blockName(region, mPos)));
        if (!withinReach(sp, center(mPos))) return JobStep::fail("out of reach");

        // Click a solid neighbor on the face that points at the target.
        using F = ScriptModuleMinecraft::ScriptFacing;
        struct Side {
            BlockPos off;
            F        face;
        };
        static constexpr Side sides[] = {
            {{0, -1, 0}, F::Up},
            {{0, 1, 0}, F::Down},
            {{1, 0, 0}, F::West},
            {{-1, 0, 0}, F::East},
            {{0, 0, 1}, F::North},
            {{0, 0, -1}, F::South},
        };
        for (auto const& s : sides) {
            BlockPos n{mPos.x + s.off.x, mPos.y + s.off.y, mPos.z + s.off.z};
            if (!isSolid(region, n)) continue;
            Vec3 hit{0.5f - s.off.x * 0.5f, 0.5f - s.off.y * 0.5f, 0.5f - s.off.z * 0.5f};
            sp.simulateLookAt(center(mPos), sim::LookDuration::Instant);
            sp.simulateUseItemInSlotOnBlock(sp.getSelectedItemSlot(), n, s.face, hit);
            mClicked = true; // survives pause(): a reflex between click and check must not hide the result
            return JobStep::running();
        }
        return JobStep::fail("no solid block next to the target to place against");
    }

private:
    BlockPos mPos;
    bool     mClicked{false};
};

class EatJob final : public Job {
public:
    std::string describe() const override { return "eat"; }

    JobStep step(SimulatedPlayer& sp) override {
        if (!mStarted) {
            int slot = findFoodSlot(sp);
            if (slot < 0) return JobStep::fail("no food in the hotbar");
            sp.setSelectedSlot(slot);
            mHunger = hunger(sp);
            mCount  = sp.getSelectedItem().mCount;
            if (!sp.simulateUseItem()) return JobStep::fail("engine refused to use the item");
            mStarted = true;
            mTicks   = 0;
        }
        if (hunger(sp) > mHunger || sp.getSelectedItem().mCount < mCount || sp.getSelectedItem().isNull()) {
            sp.simulateStopUsingItem();
            return JobStep::done(fmt::format("hunger {:.0f}", hunger(sp)));
        }
        // Eating takes 32 ticks.
        if (++mTicks > 60) {
            sp.simulateStopUsingItem();
            return JobStep::fail("did not finish eating");
        }
        return JobStep::running();
    }

private:
    float mHunger{0};
    int   mCount{0};
};

class AttackJob final : public Job {
public:
    AttackJob(std::optional<ActorUniqueID> target, float radius) : mTarget(target), mRadius(radius) {}

    std::string describe() const override { return mTarget ? "attack target" : "attack nearest monster"; }

    JobStep step(SimulatedPlayer& sp) override {
        if (!mTarget) {
            auto* m = nearestMonster(sp, mRadius);
            if (!m) return JobStep::fail("no monster nearby");
            mTarget = m->getOrCreateUniqueID();
        }
        auto  level = ll::service::getLevel();
        auto* t     = level ? level->fetchEntity(*mTarget, false) : nullptr;
        if (!t || !t->isAlive()) {
            sp.simulateStopMoving();
            return mHits > 0 ? JobStep::done(fmt::format("target down after {} hits", mHits))
                             : JobStep::done("target gone");
        }
        float dist = sp.getPosition().distanceTo(t->getPosition());
        if (dist > mRadius + 8) {
            sp.simulateStopMoving();
            return JobStep::fail("target got away");
        }
        if (++mTicks > 20 * 60) return JobStep::fail("timed out");

        mCooldown.tick();
        if (dist > 2.8f) {
            // Re-aim the navigation now and then; the target moves.
            if (!mStarted || mTicks % 10 == 0) {
                sp.simulateNavigateToEntity(*t, 1.0f);
                mStarted = true;
            }
            return JobStep::running();
        }
        sp.simulateStopMoving();
        mStarted = false;
        sp.simulateLookAt(*t, sim::LookDuration::Instant);
        if (mCooldown.ready() && sp.simulateAttack(t)) {
            ++mHits;
            mCooldown.trigger();
        }
        return JobStep::running();
    }

private:
    std::optional<ActorUniqueID> mTarget;
    float                        mRadius;
    int                          mHits{0};
    Cooldown                     mCooldown{10};
};

} // namespace

std::unique_ptr<Job> makeGoto(Vec3 target, float tolerance) { return std::make_unique<GotoJob>(target, tolerance); }
std::unique_ptr<Job> makeMine(BlockPos pos) { return std::make_unique<MineJob>(pos); }
std::unique_ptr<Job> makePlace(BlockPos pos) { return std::make_unique<PlaceJob>(pos); }
std::unique_ptr<Job> makeEat() { return std::make_unique<EatJob>(); }
std::unique_ptr<Job> makeAttack(std::optional<ActorUniqueID> target, float radius) {
    return std::make_unique<AttackJob>(target, radius);
}

} // namespace karakuri
