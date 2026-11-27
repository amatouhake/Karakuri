#include "mod/BotManager.h"

#include "mod/Game.h"
#include "mod/Mod.h"

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/EnumName.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/GameType.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

namespace karakuri {

namespace {
constexpr int   kRespawnDelay = 20;
constexpr float kThreatRadius = 6.0f;
} // namespace

BotManager& BotManager::instance() {
    static BotManager m;
    return m;
}

Bot* BotManager::find(std::string const& name) {
    auto it = mBots.find(name);
    return it == mBots.end() ? nullptr : &it->second;
}

SimulatedPlayer* BotManager::resolve(Bot const& bot) const {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto* p = level->getPlayer(bot.name);
    return p && p->isSimulated() ? static_cast<SimulatedPlayer*>(p) : nullptr;
}

std::vector<std::string> BotManager::names() const {
    std::vector<std::string> out;
    for (auto const& [name, _] : mBots) out.push_back(name);
    return out;
}

void BotManager::publishNames() {
    ll::command::CommandRegistrar::getServerInstance().setSoftEnumValues(
        ll::command::enum_name_v<BotName>,
        names()
    );
}

std::string BotManager::spawn(std::string const& name, Vec3 pos, int dimension) {
    if (name.empty() || name.size() > 16) return "bot names are 1 to 16 characters";
    if (find(name)) return fmt::format("bot {} already exists", name);
    auto level = ll::service::getLevel();
    if (!level) return "level not ready";
    if (level->getPlayer(name)) return fmt::format("a player named {} is online", name);

    SimulatedPlayer* sp = SimulatedPlayer::create(name, pos, dimension, {0, 0}).as_ptr();
    if (!sp) return "the engine refused to create the bot";
    sp->setPlayerGameType(GameType::Survival);

    auto& bot = mBots[name];
    bot.name  = name;
    publishNames();
    logger().info("{} spawned: {}", name, describe(*sp));
    return {};
}

std::string BotManager::remove(std::string const& name) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    if (auto* sp = resolve(*bot)) sp->simulateDisconnect();
    mBots.erase(name);
    publishNames();
    logger().info("{} removed", name);
    return {};
}

void BotManager::removeAll() {
    for (auto& [name, bot] : mBots) {
        if (auto* sp = resolve(bot)) sp->simulateDisconnect();
    }
    mBots.clear();
}

std::string BotManager::assign(std::string const& name, std::unique_ptr<Job> job) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    auto* sp = resolve(*bot);
    if (!sp) return fmt::format("{} is not in the world", name);
    if (bot->job) {
        bot->job->cancel(*sp);
        logger().info("{} job {} cancelled: replaced", name, bot->job->describe());
    }
    logger().info("{} job {} started", name, job->describe());
    bot->job = std::move(job);
    return {};
}

std::string BotManager::stop(std::string const& name) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    auto* sp = resolve(*bot);
    if (sp && bot->job) bot->job->cancel(*sp);
    if (sp && bot->reflex) bot->reflex->cancel(*sp);
    if (bot->job) bot->lastResult = fmt::format("{}: cancelled", bot->job->describe());
    bot->job.reset();
    bot->reflex.reset();
    bot->arbiter.reset();
    return {};
}

std::string BotManager::hold(std::string const& name, int slot) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    if (slot < 0 || slot > 8) return "hotbar slots are 0 to 8";
    auto* sp = resolve(*bot);
    if (!sp) return fmt::format("{} is not in the world", name);
    sp->setSelectedSlot(slot);
    return {};
}

std::string BotManager::setReflexes(std::string const& name, bool on) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    bot->reflexesEnabled = on;
    if (!on && bot->reflex) {
        if (auto* sp = resolve(*bot)) bot->reflex->cancel(*sp);
        bot->reflex.reset();
        bot->arbiter.reset();
    }
    return {};
}

std::string BotManager::status(std::string const& name) {
    auto* bot = find(name);
    if (!bot) return fmt::format("no bot named {}", name);
    auto* sp = resolve(*bot);
    return fmt::format(
        "{}: {} | job={} reflex={} reflexes={} | last: {}{}",
        name,
        sp ? describe(*sp) : std::string("not in world"),
        bot->job ? bot->job->describe() : "-",
        toString(bot->arbiter.current()),
        bot->reflexesEnabled ? "on" : "off",
        bot->lastResult,
        sp ? fmt::format(" | inv: {}", inventorySummary(*sp)) : ""
    );
}

void BotManager::onHurt(SimulatedPlayer& sp) {
    if (auto* bot = find(sp.getRealName())) bot->lastHurtTick = mTick;
}

void BotManager::onDeath(SimulatedPlayer& sp) {
    auto* bot = find(sp.getRealName());
    if (!bot) return;
    if (bot->job) {
        bot->job->cancel(sp);
        bot->lastResult = fmt::format("{}: failed (died)", bot->job->describe());
        logger().info("{} job {} failed: died", bot->name, bot->job->describe());
    }
    bot->job.reset();
    bot->reflex.reset();
    bot->arbiter.reset();
    // A bot that dies before ever standing still (spawned in mid-air, killed by the landing)
    // gets its respawn point where it landed, if that spot is solid ground and open air.
    if (!bot->respawnPointSet) {
        BlockPos feet{sp.getFeetPos()};
        auto&    region = sp.getDimensionBlockSource();
        if (isAir(region, feet) && isSolid(region, {feet.x, feet.y - 1, feet.z})) {
            sp.setRespawnPosition(feet, sp.getDimensionId());
            bot->respawnPointSet = true;
            logger().info("{} respawn point set to ({},{},{})", bot->name, feet.x, feet.y, feet.z);
        }
    }
    bot->respawnAt = mTick + kRespawnDelay;
    logger().info("{} died: {}", bot->name, describe(sp));
}

void BotManager::finishJob(Bot& bot, std::unique_ptr<Job>& slot, JobStep const& r, char const* kind) {
    auto what = slot->describe();
    logger().info(
        "{} {} {} {}{}{}",
        bot.name,
        kind,
        what,
        toString(r.state),
        r.reason.empty() ? "" : ": ",
        r.reason
    );
    if (&slot == &bot.job) bot.lastResult = fmt::format("{}: {}{}{}", what, toString(r.state), r.reason.empty() ? "" : " (", r.reason.empty() ? "" : r.reason + ")");
    slot.reset();
}

void BotManager::tick(Level&) {
    ++mTick;
    for (auto& [name, bot] : mBots) {
        auto* sp = resolve(bot);
        if (!sp) continue;
        tickBot(bot, *sp);
    }
}

void BotManager::tickBot(Bot& bot, SimulatedPlayer& sp) {
    if (!sp.isAlive()) {
        if (bot.respawnAt >= 0 && mTick >= bot.respawnAt) {
            bot.respawnAt = -1;
            sp.simulateRespawn();
            logger().info("{} respawned: {}", bot.name, describe(sp));
        }
        return;
    }

    // The respawn point defaults to the spawn position, so a bot spawned in mid-air would fall to
    // its death on every respawn (docs/SPIKE-0.md). Pin it the first time the bot stands on solid
    // ground; the terrain may not be loaded yet when the bot is created.
    if (!bot.respawnPointSet && sp.isOnGround()) {
        BlockPos feet{sp.getFeetPos()};
        sp.setRespawnPosition(feet, sp.getDimensionId());
        bot.respawnPointSet = true;
        logger().info("{} respawn point set to ({},{},{})", bot.name, feet.x, feet.y, feet.z);
    }

    // L0: reflexes may take over from the job, which pauses and resumes afterwards.
    if (bot.reflexesEnabled) {
        BodyState s;
        s.health         = (float)sp.getHealth();
        s.hunger         = hunger(sp);
        s.hasFood        = findFoodSlot(sp) >= 0;
        s.threatNear     = nearestMonster(sp, kThreatRadius) != nullptr;
        s.ticksSinceHurt = (int)std::min<int64_t>(mTick - bot.lastHurtTick, 1 << 30);

        Reflex before = bot.arbiter.current();
        Reflex now    = bot.arbiter.choose(s);
        if (now != before) {
            if (bot.reflex) {
                bot.reflex->cancel(sp);
                bot.reflex.reset();
            }
            if (now != Reflex::None) {
                if (bot.job) bot.job->pause(sp);
                bot.reflex = now == Reflex::Fight ? makeAttack(std::nullopt, kThreatRadius) : makeEat();
            }
            logger().info("{} reflex {} -> {}", bot.name, toString(before), toString(now));
        }
    }

    if (bot.reflex) {
        auto r = bot.reflex->step(sp);
        if (r.finished()) {
            finishJob(bot, bot.reflex, r, "reflex");
            // A reflex that is still wanted starts again: soon after success (the next bread, the
            // next mob), later after a failure so a hopeless one does not spin every tick.
            bot.reflexRetryAt = mTick + (r.state == JobState::Done ? 1 : 20);
        }
        return;
    }
    if (bot.arbiter.current() != Reflex::None) {
        if (mTick >= bot.reflexRetryAt) {
            bot.reflex = bot.arbiter.current() == Reflex::Fight ? makeAttack(std::nullopt, kThreatRadius)
                                                                : makeEat();
        }
        return; // the job stays paused until the arbiter lets go
    }

    if (bot.job) {
        auto r = bot.job->step(sp);
        if (r.finished()) finishJob(bot, bot.job, r, "job");
    }
}

} // namespace karakuri
