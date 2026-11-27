// Executes pathfinder plans one step at a time. Every step is confirmed from world state (block
// gone, block placed, feet in the expected cell); on any mismatch the job plans again from where
// the bot is. A pause (reflex) also ends in a fresh plan.

#include "mod/Jobs.h"

#include "mod/Game.h"
#include "mod/Mod.h"
#include "mod/World.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include <chrono>
#include <cmath>

namespace karakuri {

namespace {

using path::Move;
using path::Pos;

Pos feetCell(SimulatedPlayer& sp) {
    auto f = sp.getFeetPos();
    // +0.5: standing on a slab or stair puts the feet half a block into the cell below.
    return {(int)std::floor(f.x), (int)std::floor(f.y + 0.5f), (int)std::floor(f.z)};
}

BlockPos toBlock(Pos p) { return {p.x, p.y, p.z}; }
Vec3     center(Pos p) { return {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}; }
Vec3     floorCenter(Pos p) { return {p.x + 0.5f, (float)p.y, p.z + 0.5f}; }

ScriptModuleMinecraft::ScriptFacing faceFromTo(Pos support, Pos target) {
    using F = ScriptModuleMinecraft::ScriptFacing;
    int dx = target.x - support.x, dy = target.y - support.y, dz = target.z - support.z;
    if (dy > 0) return F::Up;
    if (dy < 0) return F::Down;
    if (dx > 0) return F::East;
    if (dx < 0) return F::West;
    return dz > 0 ? F::South : F::North;
}

class PathGotoJob final : public Job {
public:
    PathGotoJob(path::Goal goal, std::string label) : mGoal(goal), mLabel(std::move(label)) {}

    std::string describe() const override { return mLabel; }

    JobStep step(SimulatedPlayer& sp) override {
        if (!mStarted) { // first step, or resuming after a pause
            mStarted  = true;
            mNeedPlan = true;
        }
        Pos feet = feetCell(sp);
        if (sp.isOnGround() && path::goalReached(feet, mGoal)) {
            sp.simulateStopMoving();
            return JobStep::done(fmt::format("{} replans", mReplans));
        }
        if (mNeedPlan) {
            if (auto r = startPlan(sp, feet); r.finished()) return r;
        }
        if (mSearch) return continuePlan(sp);
        if (mIdx >= mSteps.size()) return replan(sp, feet, mPartial ? "partial path done" : "path done but goal not reached");
        return execute(sp, feet, mSteps[mIdx]);
    }

private:
    JobStep startPlan(SimulatedPlayer& sp, Pos feet) {
        mNeedPlan = false;
        float d   = path::distanceToGoal(feet, mGoal);
        if (d < mBestDist - 0.5f) {
            mBestDist   = d;
            mNoProgress = 0;
        } else if (mReplans > 0 && ++mNoProgress >= 3) {
            sp.simulateStopMoving();
            return JobStep::fail(fmt::format("no progress after {} replans ({:.1f} blocks left)", mReplans, d));
        }
        if (mReplans > 60) return JobStep::fail("too many replans");
        path::Options opt;
        opt.allowDig       = true;
        opt.scaffoldBlocks = scaffoldCount(sp);
        mScaffold = opt.scaffoldBlocks;
        mSearch.emplace(feet, mGoal, opt);
        mSearch->setMaxNodes(kMaxNodes);
        mPlanMs = 0;
        return JobStep::running();
    }

    JobStep continuePlan(SimulatedPlayer& sp) {
        BlockWorld w(sp.getDimensionBlockSource(), sp);
        auto       t0 = std::chrono::steady_clock::now();
        // About 20 microseconds per node on the test machine: keep each tick's share small.
        auto       st = mSearch->step(w, kNodesPerTick);
        mPlanMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (st == path::SearchState::Running) return JobStep::running();
        mSteps = mSearch->path();
        int moves[6]{}, breaks = 0;
        for (auto const& s : mSteps) {
            ++moves[(int)s.move];
            breaks += (int)s.breaks.size();
        }
        logger().info(
            "{} plan {}: {} steps (walk {}, ascend {}, descend {}, bridge {}, pillar {}, dig-down {}; {} blocks to "
            "break), {} nodes, {:.1f} ms over the ticks",
            sp.getRealName(),
            toString(st),
            mSteps.size(),
            moves[0],
            moves[1],
            moves[2],
            moves[3],
            moves[4],
            moves[5],
            breaks,
            mSearch->expanded(),
            mPlanMs
        );
        mPartial     = st == path::SearchState::Partial;
        int expanded = mSearch->expanded();
        mSearch.reset();
        if (mSteps.empty()) {
            sp.simulateStopMoving();
            return JobStep::fail(fmt::format(
                "no path ({} nodes searched{}, {} scaffold blocks in the hotbar)",
                expanded,
                expanded >= kMaxNodes ? ", search limit reached" : "",
                mScaffold
            ));
        }
        mIdx = 0;
        resetStep();
        return JobStep::running();
    }

    JobStep replan(SimulatedPlayer& sp, Pos feet, char const* why) {
        ++mReplans;
        logger().info("{} replan #{} at ({},{},{}): {}", sp.getRealName(), mReplans, feet.x, feet.y, feet.z, why);
        sp.simulateStopMoving();
        sp.simulateStopDestroyingBlock();
        mNeedPlan = true;
        return JobStep::running();
    }

    void resetStep() {
        mPhase      = Phase::Break;
        mBreakIdx   = 0;
        mBreaking   = false;
        mBreakTicks = 0;
        mStepTicks  = 0;
        mPlaceTries = 0;
        mJumped     = false;
        mMoving     = false;
        mBlocked    = 0;
    }

    JobStep nextStep() {
        ++mIdx;
        resetStep();
        return JobStep::running();
    }

    JobStep execute(SimulatedPlayer& sp, Pos feet, path::Step const& s) {
        auto& region = sp.getDimensionBlockSource();
        if (mStepTicks++ == 0 && !(feet == s.from)) return replan(sp, feet, "not where the step starts");

        if (mPhase == Phase::Break) {
            while (mBreakIdx < s.breaks.size()) {
                Pos  b    = s.breaks[mBreakIdx];
                auto bp   = toBlock(b);
                auto kind = classify(region.getBlock(bp));
                if (kind == path::CellKind::Air || kind == path::CellKind::Passable) {
                    ++mBreakIdx;
                    mBreaking   = false;
                    mBreakTicks = 0;
                    continue;
                }
                if (++mBreakTicks > 20 * 30) return replan(sp, feet, "could not break a block");
                if (!mBreaking) {
                    if (!withinReach(sp, center(b))) return replan(sp, feet, "block out of reach");
                    sp.setSelectedSlot(bestToolSlot(sp, region.getBlock(bp)));
                    sp.simulateLookAt(center(b), sim::LookDuration::Instant);
                    if (!sp.simulateDestroyBlock(bp, faceToward(bp, sp.getEyePos())))
                        return replan(sp, feet, "engine refused to break");
                    mBreaking = true;
                } else if (!sp.isSimulatingDestroyingBlock()) {
                    mBreaking = false; // interrupted: start again next tick
                }
                return JobStep::running();
            }
            mPhase = s.place ? Phase::Place : Phase::Move;
        }

        if (mPhase == Phase::Place) {
            Pos target = *s.place;
            if (classify(region.getBlock(toBlock(target))) != path::CellKind::Air
                && classify(region.getBlock(toBlock(target))) != path::CellKind::Passable) {
                mPhase = Phase::Move;
            } else {
                int slot = scaffoldSlot(sp);
                if (slot < 0) return JobStep::fail("out of scaffold blocks");
                if (s.move == Move::Pillar) {
                    if (!mJumped) {
                        sp.setSelectedSlot(slot);
                        sp.simulateJump();
                        mJumped = true;
                        return JobStep::running();
                    }
                    if (sp.getFeetPos().y < target.y + 1.0f - 0.01f) {
                        if (mStepTicks > 20) return replan(sp, feet, "pillar jump did not rise");
                        return JobStep::running();
                    }
                }
                if (++mPlaceTries > 4) return replan(sp, feet, "placing failed");
                Pos support = s.move == Move::Pillar ? target.down() : s.from.down();
                Vec3 hit{
                    0.5f + (target.x - support.x) * 0.5f,
                    0.5f + (target.y - support.y) * 0.5f,
                    0.5f + (target.z - support.z) * 0.5f
                };
                sp.setSelectedSlot(slot);
                sp.simulateLookAt(center(target), sim::LookDuration::Instant);
                sp.simulateUseItemInSlotOnBlock(slot, toBlock(support), faceFromTo(support, target), hit);
                return JobStep::running(); // checked on the next tick
            }
        }

        // Move phase.
        bool arrived = sp.isOnGround() && feet == s.to;
        if (s.move == Move::Pillar || s.move == Move::DigDown) {
            if (arrived) return nextStep();
            if (mStepTicks > 40) return replan(sp, feet, "did not land");
            return JobStep::running();
        }
        auto  f   = sp.getFeetPos();
        Vec3  dst = floorCenter(s.to);
        float hd  = std::hypot(f.x - dst.x, f.z - dst.z);
        if (arrived && hd < 0.35f) return nextStep();
        if (!mMoving) {
            sp.simulateMoveToLocation(dst, 1.0f, true);
            mMoving = true;
        }
        auto d = sp.getPosDelta();
        if (sp.isOnGround() && std::abs(d.x) + std::abs(d.z) < 0.01f && mStepTicks > 2) {
            // Blocked on the ground: a step up (or a slab edge) needs a jump.
            if (++mBlocked >= 2) {
                sp.simulateJump();
                sp.simulateMoveToLocation(dst, 1.0f, true);
                mBlocked = 0;
            }
        }
        if (mStepTicks > 80) return replan(sp, feet, "step timed out");
        return JobStep::running();
    }

    enum class Phase { Break, Place, Move };
    static constexpr int kNodesPerTick = 800;
    static constexpr int kMaxNodes     = 20000;

    path::Goal                  mGoal;
    std::string                 mLabel;
    std::optional<path::Search> mSearch;
    std::vector<path::Step>     mSteps;
    size_t                      mIdx{0};
    bool                        mPartial{false};
    bool                        mNeedPlan{true};
    double                      mPlanMs{0};
    int                         mReplans{0};
    int                         mNoProgress{0};
    float                       mBestDist{1e30f};
    int                         mScaffold{0};

    Phase  mPhase{Phase::Break};
    size_t mBreakIdx{0};
    bool   mBreaking{false};
    int    mBreakTicks{0};
    int    mStepTicks{0};
    int    mPlaceTries{0};
    bool   mJumped{false};
    bool   mMoving{false};
    int    mBlocked{0};
};

} // namespace

std::unique_ptr<Job> makePathGoto(BlockPos target, float range) {
    return std::make_unique<PathGotoJob>(
        path::Goal{{target.x, target.y, target.z}, range, false},
        fmt::format("goto ({},{},{})", target.x, target.y, target.z)
    );
}

std::unique_ptr<Job> makeApproach(BlockPos block, float reach) {
    return std::make_unique<PathGotoJob>(
        path::Goal{{block.x, block.y, block.z}, reach, true},
        fmt::format("approach ({},{},{})", block.x, block.y, block.z)
    );
}

} // namespace karakuri
