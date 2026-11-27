// Karakuri spike: SimulatedPlayer feasibility probe (docs/DESIGN.md, section 7 step 0).
// Driven from the server console with `kspike <action> [args]`. Every result line starts
// with "[KSPIKE]" so the log can be grepped afterwards.

#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/entity/ActorHurtEvent.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/scripting/modules/gametest/ScriptNavigationResult.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"

#include <atomic>
#include <string>

namespace karakuri_spike {

using namespace ll::chrono_literals;

struct Mod {
    static Mod& getInstance() {
        static Mod i;
        return i;
    }
    Mod() : self(*ll::mod::NativeMod::current()) {}
    ll::mod::NativeMod& getSelf() const { return self; }
    bool                load() { return true; }
    bool                enable();
    bool                disable() { return true; }
    ll::mod::NativeMod& self;
};

// Command parameters need external linkage for pfr reflection.
struct Params {
    std::string action;
    int         x{0};
    int         y{0};
    int         z{0};
};

namespace {

constexpr char const* kBotName = "kbot";

ll::io::Logger& log() { return Mod::getInstance().getSelf().getLogger(); }

template <class... Args>
void report(fmt::format_string<Args...> f, Args&&... args) {
    log().info("[KSPIKE] {}", fmt::format(f, std::forward<Args>(args)...));
}

SimulatedPlayer* bot() {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto* p = level->getPlayer(kBotName);
    if (!p || !p->isSimulated()) return nullptr;
    return static_cast<SimulatedPlayer*>(p);
}

std::string blockName(BlockSource& region, BlockPos const& pos) {
    return std::string(region.getBlock(pos).getTypeName());
}

float hunger(SimulatedPlayer& sp) {
    auto ref = sp.getAttribute(Player::HUNGER());
    return ref.mPtr ? ref.mPtr->mCurrentValue : -1.0f;
}

std::string describe(SimulatedPlayer& sp) {
    auto pos = sp.getFeetPos();
    return fmt::format(
        "pos=({:.2f},{:.2f},{:.2f}) dim={} hp={} hunger={:.1f} onGround={} alive={} gamemode={} sleeping={} "
        "selectedSlot={} held={}",
        pos.x,
        pos.y,
        pos.z,
        (int)sp.getDimensionId(),
        sp.getHealth(),
        hunger(sp),
        sp.isOnGround(),
        sp.isAlive(),
        (int)sp.getPlayerGameType(),
        sp.isSleeping(),
        sp.getSelectedItemSlot(),
        sp.getSelectedItem().getTypeName()
    );
}

// Count items of a given type name in the bot's inventory.
int countItem(SimulatedPlayer& sp, std::string_view typeName) {
    int   total = 0;
    auto& inv   = sp.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (!it.isNull() && it.getTypeName() == typeName) total += it.mCount;
    }
    return total;
}

std::string inventorySummary(SimulatedPlayer& sp) {
    std::string out;
    auto&       inv = sp.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (it.isNull()) continue;
        out += fmt::format("{}:{}x{} ", i, it.getTypeName(), (int)it.mCount);
    }
    return out.empty() ? "(empty)" : out;
}

std::atomic_bool gWatching{false};

// Logs the bot's state every `everyTicks` ticks for `totalTicks` ticks.
void watch(int everyTicks, int totalTicks, std::string tag) {
    if (gWatching.exchange(true)) {
        report("watch already running");
        return;
    }
    ll::coro::keepThis([=]() -> ll::coro::CoroTask<> {
        for (int t = 0; t <= totalTicks; t += everyTicks) {
            if (auto* sp = bot()) report("{} t={} {}", tag, t, describe(*sp));
            else report("{} t={} bot missing", tag, t);
            co_await ll::chrono::ticks(everyTicks);
        }
        gWatching = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

// Breaks the block at pos and measures how many ticks it takes until it turns to air.
void timedBreak(BlockPos pos) {
    auto* sp = bot();
    if (!sp) return report("break: no bot");
    auto& region = sp->getDimensionBlockSource();
    auto  before = blockName(region, pos);
    auto  invBefore = inventorySummary(*sp);
    sp->simulateLookAt(Vec3(pos) + Vec3{0.5f, 0.5f, 0.5f}, sim::LookDuration::Instant);
    bool started = sp->simulateDestroyBlock(pos, ScriptModuleMinecraft::ScriptFacing::Up);
    report("break start pos=({},{},{}) block={} started={} held={}", pos.x, pos.y, pos.z, before, started,
           sp->getSelectedItem().getTypeName());
    ll::coro::keepThis([=]() -> ll::coro::CoroTask<> {
        for (int t = 1; t <= 400; ++t) {
            co_await 1_tick;
            auto* b = bot();
            if (!b) co_return;
            auto now = blockName(b->getDimensionBlockSource(), pos);
            if (now != before) {
                report("break done ticks={} {} -> {} destroying={}", t, before, now, b->isSimulatingDestroyingBlock());
                co_await 20_tick;
                if (auto* b2 = bot()) report("break inventory before=[{}] after=[{}]", invBefore, inventorySummary(*b2));
                co_return;
            }
        }
        report("break timeout after 400 ticks, block still {}", before);
        if (auto* b = bot()) b->simulateStopDestroyingBlock();
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void navigate(Vec3 target, float speed) {
    auto* sp = bot();
    if (!sp) return report("nav: no bot");
    auto res = sp->simulateNavigateToLocation(target, speed);
    report("nav request target=({:.1f},{:.1f},{:.1f}) isFullPath={} pathLen={}", target.x, target.y, target.z,
           (bool)res.mIsFullPath, res.mPath->size());
    for (size_t i = 0; i < res.mPath->size() && i < 40; ++i) {
        auto const& p = (*res.mPath)[i];
        report("nav path[{}]=({},{},{})", i, p.x, p.y, p.z);
    }
    watch(20, 400, "nav");
}

void useHeld(int holdTicks) {
    auto* sp = bot();
    if (!sp) return report("use: no bot");
    auto before = describe(*sp);
    bool ok     = sp->simulateUseItem();
    report("use start ok={} before: {}", ok, before);
    ll::coro::keepThis([=]() -> ll::coro::CoroTask<> {
        co_await ll::chrono::ticks(holdTicks);
        if (auto* b = bot()) {
            b->simulateStopUsingItem();
            report("use after {} ticks: {} inv=[{}]", holdTicks, describe(*b), inventorySummary(*b));
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}


void handle(Params const& p, CommandOutput& out) {
    auto level = ll::service::getLevel();
    if (!level) return out.error("no level");

    auto const& a = p.action;
    if (a == "spawn") {
        if (bot()) return out.error("bot already exists");
        Vec3 pos{(float)p.x + 0.5f, (float)p.y, (float)p.z + 0.5f};
        auto sp = SimulatedPlayer::create(kBotName, pos, 0, {0, 0});
        if (!sp) return report("spawn failed");
        sp->setPlayerGameType(GameType::Survival);
        report("spawned xuid={} isSimulated={} {}", sp->getXuid(), sp->isSimulated(), describe(*sp));
        return;
    }
    auto* sp = bot();
    if (!sp) return out.error("no bot; run `kspike spawn x y z`");

    if (a == "status") {
        report("status {} inv=[{}]", describe(*sp), inventorySummary(*sp));
    } else if (a == "watch") {
        watch(p.x > 0 ? p.x : 20, p.y > 0 ? p.y : 200, "watch");
    } else if (a == "break") {
        timedBreak({p.x, p.y, p.z});
    } else if (a == "nav") {
        navigate({(float)p.x + 0.5f, (float)p.y, (float)p.z + 0.5f}, 1.0f);
    } else if (a == "stop") {
        sp->simulateStopMoving();
        sp->simulateStopDestroyingBlock();
        sp->simulateStopUsingItem();
        sp->simulateStopInteracting();
        report("stopped");
    } else if (a == "slot") {
        sp->setSelectedSlot(p.x);
        report("slot {} -> {}", p.x, describe(*sp));
    } else if (a == "use") {
        useHeld(p.x > 0 ? p.x : 40);
    } else if (a == "useon") {
        BlockPos pos{p.x, p.y, p.z};
        sp->simulateLookAt(Vec3(pos) + Vec3{0.5f, 0.5f, 0.5f}, sim::LookDuration::Instant);
        bool ok = sp->simulateInteract(pos, ScriptModuleMinecraft::ScriptFacing::Up);
        report("interact block=({},{},{}) {} ok={} after: {}", p.x, p.y, p.z,
               blockName(sp->getDimensionBlockSource(), pos), ok, describe(*sp));
    } else if (a == "attack") {
        // Attack whatever the bot is looking at.
        bool ok = sp->simulateAttack();
        report("attack ok={}", ok);
    } else if (a == "jump") {
        report("jump ok={}", sp->simulateJump());
    } else if (a == "drop") {
        report("drop ok={} inv=[{}]", sp->simulateDropSelectedItem(), inventorySummary(*sp));
    } else if (a == "respawn") {
        report("respawn ok={} {}", sp->simulateRespawn(), describe(*sp));
    } else if (a == "look") {
        sp->simulateLookAt(Vec3{(float)p.x + 0.5f, (float)p.y + 0.5f, (float)p.z + 0.5f}, sim::LookDuration::Instant);
        report("look at ({},{},{})", p.x, p.y, p.z);
    } else if (a == "block") {
        BlockPos pos{p.x, p.y, p.z};
        report("block ({},{},{}) = {}", p.x, p.y, p.z, blockName(sp->getDimensionBlockSource(), pos));
    } else if (a == "despawn") {
        sp->simulateDisconnect();
        report("despawned");
    } else {
        return out.error("unknown action");
    }
    out.success("ok");
}

} // namespace

bool Mod::enable() {
    auto& bus = ll::event::EventBus::getInstance();
    auto& cmd = ll::command::CommandRegistrar::getServerInstance().getOrCreateCommand(
        "kspike",
        "Karakuri SimulatedPlayer probe",
        CommandPermissionLevel::GameDirectors
    );
    cmd.overload<Params>().required("action").optional("x").optional("y").optional("z").execute(
        [](CommandOrigin const&, CommandOutput& out, Params const& p) { handle(p, out); }
    );
    bus.emplaceListener<ll::event::ActorHurtEvent>([](ll::event::ActorHurtEvent& ev) {
        auto& self = ev.self();
        if (!self.isPlayer() || !static_cast<Player&>(self).isSimulated()) return;
        report("hurt damage={:.1f} cause={} hpBefore={}", ev.damage(), (int)ev.source().mCause, self.getHealth());
    });
    bus.emplaceListener<ll::event::PlayerDieEvent>([](ll::event::PlayerDieEvent& ev) {
        auto& pl = ev.self();
        if (!pl.isSimulated()) return;
        report("died cause={} {}", (int)ev.source().mCause, describe(static_cast<SimulatedPlayer&>(pl)));
    });
    return true;
}

} // namespace karakuri_spike

LL_REGISTER_MOD(karakuri_spike::Mod, karakuri_spike::Mod::getInstance());
