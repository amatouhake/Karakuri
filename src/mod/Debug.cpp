// `/kr debug ...`: probes for engine behavior the pathfinder depends on. Not a user feature.

#include "mod/BotManager.h"
#include "mod/Game.h"
#include "mod/Mod.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/SoftEnum.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandPositionFloat.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"

#include <cmath>

namespace karakuri {

namespace dbg {
using ll::command::SoftEnum;
enum class DebugPosOp : int { probe, moveto, waypoints, stepto };
enum class DebugOp : int { pillar, sneak, unsneak, jump };
enum class Dir : int { east, west, north, south };
struct PosParams {
    DebugPosOp           op;
    SoftEnum<BotName>    bot;
    CommandPositionFloat pos;
};
struct OpParams {
    DebugOp           op;
    SoftEnum<BotName> bot;
};
struct BridgeParams {
    SoftEnum<BotName> bot;
    Dir               dir;
};
struct TraceParams {
    SoftEnum<BotName> bot;
    int               every;
};
} // namespace dbg

namespace {
using namespace dbg;

BlockPos feetBlock(SimulatedPlayer& sp) {
    auto f = sp.getFeetPos();
    return {(int)std::floor(f.x), (int)std::floor(f.y + 0.01f), (int)std::floor(f.z)};
}

std::string probe(SimulatedPlayer& sp, BlockPos const& p) {
    auto& region = sp.getDimensionBlockSource();
    if (!region.getChunkAt(p)) return fmt::format("({},{},{}) chunk not loaded", p.x, p.y, p.z);
    auto const& b  = region.getBlock(p);
    auto const& m  = b.getMaterial();
    auto const& bt = b.getBlockType();
    return fmt::format(
        "({},{},{}) {} air={} mat.solid={} mat.liquid={} mat.blocksMotion={} type.solid={} falling={} "
        "progress/tick(held)={:.4f}",
        p.x,
        p.y,
        p.z,
        b.getTypeName(),
        b.isAir(),
        (bool)m.mSolid,
        (bool)m.mLiquid,
        (bool)m.mBlocksMotion,
        (bool)bt.mSolid,
        (bool)bt.mFalling,
        sp.getDestroyProgress(b)
    );
}

// Moves with simulateMoveToLocation and keeps logging after arrival, to see whether the engine
// stops on its own, steps up and walks off ledges.
class MoveProbe final : public Job {
public:
    explicit MoveProbe(Vec3 t) : mTarget(t) {}
    std::string describe() const override { return "debug moveto"; }
    JobStep     step(SimulatedPlayer& sp) override {
        if (!mStarted) {
            sp.simulateMoveToLocation(mTarget, 1.0f, true);
            mStarted = true;
        }
        if (mTicks % 10 == 0) logger().info("[probe] moveto t={} {}", mTicks, describe(sp));
        return ++mTicks > 200 ? JobStep::done() : JobStep::running();
    }

private:
    static std::string describe(SimulatedPlayer& sp) {
        auto f = sp.getFeetPos();
        auto d = sp.getPosDelta();
        return fmt::format("pos=({:.2f},{:.2f},{:.2f}) delta=({:.2f},{:.2f},{:.2f}) onGround={}", f.x, f.y, f.z, d.x, d.y, d.z, sp.isOnGround());
    }
    Vec3 mTarget;
};

// Like MoveProbe, but jumps when the bot is blocked on the ground (to climb one-block steps).
class StepProbe final : public Job {
public:
    explicit StepProbe(Vec3 t) : mTarget(t) {}
    std::string describe() const override { return "debug stepto"; }
    JobStep     step(SimulatedPlayer& sp) override {
        if (!mStarted) {
            sp.simulateMoveToLocation(mTarget, 1.0f, true);
            mStarted = true;
        }
        auto f = sp.getFeetPos();
        auto d = sp.getPosDelta();
        bool blocked = sp.isOnGround() && std::abs(d.x) + std::abs(d.z) < 0.01f && mTicks > 2;
        if (blocked) {
            logger().info("[probe] stepto t={} blocked at ({:.2f},{:.2f},{:.2f}) jump={}", mTicks, f.x, f.y, f.z, sp.simulateJump());
            sp.simulateMoveToLocation(mTarget, 1.0f, true);
        }
        if (mTicks % 10 == 0) logger().info("[probe] stepto t={} pos=({:.2f},{:.2f},{:.2f}) onGround={}", mTicks, f.x, f.y, f.z, sp.isOnGround());
        if (Vec3{f.x, mTarget.y, f.z}.distanceTo(mTarget) < 0.3f) return JobStep::done(fmt::format("arrived t={}", mTicks));
        return ++mTicks > 300 ? JobStep::fail("timeout") : JobStep::running();
    }

private:
    Vec3 mTarget;
};

class WaypointProbe final : public Job {
public:
    explicit WaypointProbe(Vec3 t) : mTarget(t) {}
    std::string describe() const override { return "debug waypoints"; }
    JobStep     step(SimulatedPlayer& sp) override {
        if (!mStarted) {
            std::vector<Vec3> pts;
            auto              from = sp.getFeetPos();
            float             len  = from.distanceTo(mTarget);
            int               n    = std::max(1, (int)std::ceil(len / 8.0f));
            for (int i = 1; i <= n; ++i) pts.push_back(from + (mTarget - from) * ((float)i / n));
            logger().info("[probe] waypoints n={} len={:.1f}", n, len);
            sp.simulateNavigateToLocations(std::move(pts), 1.0f);
            mStarted = true;
        }
        if (mTicks % 20 == 0) {
            auto f = sp.getFeetPos();
            logger().info("[probe] waypoints t={} pos=({:.1f},{:.1f},{:.1f})", mTicks, f.x, f.y, f.z);
        }
        return ++mTicks > 400 ? JobStep::done() : JobStep::running();
    }

private:
    Vec3 mTarget;
};

// Jump, then try to place the held block in the cell the feet just left, every tick, and report
// which tick after the jump it worked.
class PillarProbe final : public Job {
public:
    std::string describe() const override { return "debug pillar"; }
    JobStep     step(SimulatedPlayer& sp) override {
        auto& region = sp.getDimensionBlockSource();
        if (!mStarted) {
            mFeet = feetBlock(sp);
            logger().info("[probe] pillar start feet=({},{},{}) jump={}", mFeet.x, mFeet.y, mFeet.z, sp.simulateJump());
            mStarted = true;
            return JobStep::running();
        }
        ++mTicks;
        auto     f     = sp.getFeetPos();
        BlockPos below = {mFeet.x, mFeet.y - 1, mFeet.z};
        bool     tried = false;
        if (f.y >= mFeet.y + 1.0f && isAir(region, mFeet)) {
            sp.simulateLookAt(Vec3{mFeet.x + 0.5f, (float)mFeet.y, mFeet.z + 0.5f}, sim::LookDuration::Instant);
            sp.simulateUseItemInSlotOnBlock(sp.getSelectedItemSlot(), below, ScriptModuleMinecraft::ScriptFacing::Up, {0.5f, 1.0f, 0.5f});
            tried = true;
        }
        logger().info("[probe] pillar t={} feetY={:.2f} tried={} cell={}", mTicks, f.y, tried, blockName(region, mFeet));
        if (!isAir(region, mFeet)) return JobStep::done(fmt::format("placed on tick {}", mTicks));
        return mTicks > 30 ? JobStep::fail("never placed") : JobStep::running();
    }

private:
    BlockPos mFeet;
};

// Place against the side of the block under the feet, then step onto it.
class BridgeProbe final : public Job {
public:
    explicit BridgeProbe(Dir d) : mDir(d) {}
    std::string describe() const override { return "debug bridge"; }
    JobStep     step(SimulatedPlayer& sp) override {
        using F      = ScriptModuleMinecraft::ScriptFacing;
        auto& region = sp.getDimensionBlockSource();
        BlockPos off = mDir == Dir::east ? BlockPos{1, 0, 0} : mDir == Dir::west ? BlockPos{-1, 0, 0}
                     : mDir == Dir::south ? BlockPos{0, 0, 1} : BlockPos{0, 0, -1};
        F face       = mDir == Dir::east ? F::East : mDir == Dir::west ? F::West : mDir == Dir::south ? F::South : F::North;
        if (!mStarted) {
            mFeet           = feetBlock(sp);
            BlockPos below  = {mFeet.x, mFeet.y - 1, mFeet.z};
            mTarget         = {below.x + off.x, below.y, below.z + off.z};
            Vec3 hit{0.5f + off.x * 0.5f, 0.5f, 0.5f + off.z * 0.5f};
            sp.simulateLookAt(Vec3{mTarget.x + 0.5f, mTarget.y + 0.5f, mTarget.z + 0.5f}, sim::LookDuration::Instant);
            bool ok = sp.simulateUseItemInSlotOnBlock(sp.getSelectedItemSlot(), below, face, hit);
            logger().info("[probe] bridge place against ({},{},{}) target=({},{},{}) ok={}", below.x, below.y, below.z, mTarget.x, mTarget.y, mTarget.z, ok);
            mStarted = true;
            return JobStep::running();
        }
        ++mTicks;
        if (mTicks == 2) {
            logger().info("[probe] bridge target now {}", blockName(region, mTarget));
            sp.simulateMoveToLocation(Vec3{mTarget.x + 0.5f, (float)mFeet.y, mTarget.z + 0.5f}, 1.0f, true);
        }
        if (mTicks % 5 == 0) {
            auto f = sp.getFeetPos();
            logger().info("[probe] bridge t={} pos=({:.2f},{:.2f},{:.2f}) onGround={} sneaking={}", mTicks, f.x, f.y, f.z, sp.isOnGround(), sp.isSneaking());
        }
        return mTicks > 60 ? JobStep::done() : JobStep::running();
    }

private:
    Dir      mDir;
    BlockPos mFeet, mTarget;
};

} // namespace

void registerDebugCommands(ll::command::CommandHandle& cmd) {
    auto& bots = BotManager::instance();
    cmd.overload<PosParams>().text("debug").required("op").required("bot").required("pos").execute(
        [&bots](CommandOrigin const& origin, CommandOutput& out, PosParams const& p, Command const& c) {
            auto* sp = bots.player(p.bot);
            if (!sp) return out.error("no such bot in the world");
            auto pos = origin.getExecutePosition(c.mVersion, p.pos);
            switch (p.op) {
            case DebugPosOp::probe: {
                auto s = probe(*sp, {(int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z)});
                logger().info("[probe] {}", s);
                return out.success(s);
            }
            case DebugPosOp::moveto:
                return out.success(bots.assign(p.bot, std::make_unique<MoveProbe>(pos)).empty() ? "ok" : "failed");
            case DebugPosOp::stepto:
                return out.success(bots.assign(p.bot, std::make_unique<StepProbe>(pos)).empty() ? "ok" : "failed");
            case DebugPosOp::waypoints:
                return out.success(bots.assign(p.bot, std::make_unique<WaypointProbe>(pos)).empty() ? "ok" : "failed");
            }
        }
    );
    cmd.overload<OpParams>().text("debug").required("op").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OpParams const& p) {
            auto* sp = bots.player(p.bot);
            if (!sp) return out.error("no such bot in the world");
            switch (p.op) {
            case DebugOp::pillar:
                return out.success(bots.assign(p.bot, std::make_unique<PillarProbe>()).empty() ? "ok" : "failed");
            case DebugOp::sneak:
                return out.success(fmt::format("sneak {}", sp->simulateSneaking()));
            case DebugOp::jump:
                return out.success(fmt::format("jump {}", sp->simulateJump()));
            case DebugOp::unsneak:
                return out.success(fmt::format("unsneak {}", sp->simulateStopSneaking()));
            }
        }
    );
    cmd.overload<BridgeParams>().text("debug").text("bridge").required("bot").required("dir").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BridgeParams const& p) {
            out.success(bots.assign(p.bot, std::make_unique<BridgeProbe>(p.dir)).empty() ? "ok" : "failed");
        }
    );
    cmd.overload<TraceParams>().text("debug").text("trace").required("bot").required("every").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, TraceParams const& p) {
            out.success(bots.setTrace(p.bot, p.every).empty() ? "ok" : "failed");
        }
    );
}

} // namespace karakuri
