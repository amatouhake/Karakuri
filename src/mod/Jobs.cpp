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
#include "mc/world/phys/AABB.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace karakuri {

void Job::pause(SimulatedPlayer& sp) {
    sp.simulateStopMoving();
    sp.simulateStopDestroyingBlock();
    sp.simulateStopUsingItem();
    mStarted = false;
}

namespace {

Vec3 center(BlockPos const& p) { return {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}; }

// Walks within reach of `pos` first when needed. Returns a step to hand back while approaching
// (or its failure), or nothing once the block is within reach.
std::optional<JobStep> approach(SimulatedPlayer& sp, std::unique_ptr<Job>& job, BlockPos const& pos) {
    if (withinReach(sp, center(pos))) {
        job.reset();
        return std::nullopt;
    }
    if (!job) job = makeApproach(pos, kReach - 1.0f);
    auto r = job->step(sp);
    if (!r.finished()) return JobStep::running();
    job.reset();
    if (r.state == JobState::Failed) return JobStep::fail("could not get within reach: " + r.reason);
    if (!withinReach(sp, center(pos))) return JobStep::fail("still out of reach after approaching");
    return std::nullopt;
}

class MineJob final : public Job {
public:
    explicit MineJob(BlockPos pos) : mPos(pos) {}

    std::string describe() const override { return fmt::format("mine ({},{},{})", mPos.x, mPos.y, mPos.z); }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        if (mApproach) mApproach->pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        auto& region = sp.getDimensionBlockSource();
        if (!mStarted) {
            if (isAir(region, mPos)) return mBlock.empty() ? JobStep::fail("block is air") : JobStep::done();
            if (auto r = approach(sp, mApproach, mPos); r) return *r;
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
    BlockPos             mPos;
    std::string          mBlock;
    std::unique_ptr<Job> mApproach;
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
        if (auto r = approach(sp, mApproach, mPos); r) return *r;

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

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        if (mApproach) mApproach->pause(sp);
    }

private:
    BlockPos             mPos;
    bool                 mClicked{false};
    std::unique_ptr<Job> mApproach;
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

class CollectJob final : public Job {
public:
    explicit CollectJob(float radius) : mRadius(radius) {}

    std::string describe() const override { return fmt::format("collect items within {:.0f}", mRadius); }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        if (mWalk) mWalk->pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        if (++mTicks > 20 * 90) return JobStep::fail(fmt::format("timed out after {} items", mPicked));
        if (mWalk) {
            auto r = mWalk->step(sp);
            if (!r.finished()) return JobStep::running();
            mWalk.reset();
            if (r.state == JobState::Failed) mSkipped.push_back(mTarget); // unreachable: try others
            mSettle = 10; // give the pickup a moment
            return JobStep::running();
        }
        if (mSettle > 0) {
            --mSettle;
            return JobStep::running();
        }
        auto* item = nearestItem(sp);
        if (!item) {
            sp.simulateStopMoving();
            return JobStep::done(fmt::format("{} item stacks left behind", mSkipped.size()));
        }
        ++mPicked;
        mTarget  = item->getOrCreateUniqueID();
        auto f   = item->getPosition();
        mWalk    = makePathGoto(BlockPos{(int)std::floor(f.x), (int)std::floor(f.y + 0.1f), (int)std::floor(f.z)}, 1.0f);
        return JobStep::running();
    }

private:
    Actor* nearestItem(SimulatedPlayer& sp) {
        auto  c = sp.getPosition();
        AABB  box{c - Vec3{mRadius, mRadius, mRadius}, c + Vec3{mRadius, mRadius, mRadius}};
        Actor* best  = nullptr;
        float  bestD = mRadius;
        for (auto* a : sp.getDimensionBlockSource().getEntities(box, 0.0f)) {
            if (!a || a->getTypeName() != "minecraft:item") continue;
            auto id = a->getOrCreateUniqueID();
            if (std::find(mSkipped.begin(), mSkipped.end(), id) != mSkipped.end()) continue;
            float d = a->getPosition().distanceTo(c);
            if (d <= bestD) {
                bestD = d;
                best  = a;
            }
        }
        return best;
    }

    float                      mRadius;
    int                        mPicked{0};
    int                        mSettle{0};
    ActorUniqueID              mTarget{};
    std::vector<ActorUniqueID> mSkipped;
    std::unique_ptr<Job>       mWalk;
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

std::unique_ptr<Job> makeMine(BlockPos pos) { return std::make_unique<MineJob>(pos); }
std::unique_ptr<Job> makePlace(BlockPos pos) { return std::make_unique<PlaceJob>(pos); }
std::unique_ptr<Job> makeEat() { return std::make_unique<EatJob>(); }
std::unique_ptr<Job> makeCollect(float radius) { return std::make_unique<CollectJob>(radius); }
std::unique_ptr<Job> makeAttack(std::optional<ActorUniqueID> target, float radius) {
    return std::make_unique<AttackJob>(target, radius);
}

} // namespace karakuri
