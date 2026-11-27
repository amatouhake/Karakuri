#pragma once

// Owns the bots: spawning, removal, the per-tick driver (observe -> L0 reflexes -> job step),
// death and respawn. Server thread only.

#include "core/Reflex.h"
#include "mod/Jobs.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Level;
class SimulatedPlayer;

namespace karakuri {

struct Bot {
    std::string          name;
    std::unique_ptr<Job> job;    // the task given by a command or agent
    std::unique_ptr<Job> reflex; // an L0 reflex that has taken over from `job`
    ReflexArbiter        arbiter;
    bool                 reflexesEnabled{true};
    std::string          lastResult{"-"};
    int64_t              lastHurtTick{-100000};
    int64_t              respawnAt{-1};
    int64_t              reflexRetryAt{0};
    bool                 respawnPointSet{false};
};

class BotManager {
public:
    static BotManager& instance();

    // Returns an error message, or an empty string on success.
    std::string spawn(std::string const& name, Vec3 pos, int dimension);
    std::string remove(std::string const& name);
    void        removeAll();

    std::string assign(std::string const& name, std::unique_ptr<Job> job);
    std::string stop(std::string const& name);
    std::string status(std::string const& name);
    std::string hold(std::string const& name, int slot);
    std::string setReflexes(std::string const& name, bool on);
    std::vector<std::string> names() const;

    void tick(Level& level);
    void onHurt(SimulatedPlayer& sp);
    void onDeath(SimulatedPlayer& sp);

    [[nodiscard]] int64_t now() const { return mTick; }

private:
    Bot*             find(std::string const& name);
    SimulatedPlayer* resolve(Bot const& bot) const;
    void             tickBot(Bot& bot, SimulatedPlayer& sp);
    void             finishJob(Bot& bot, std::unique_ptr<Job>& slot, JobStep const& r, char const* kind);
    void             publishNames();

    std::map<std::string, Bot> mBots;
    int64_t                    mTick{0};
};

} // namespace karakuri
