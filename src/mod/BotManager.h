#pragma once

// Owns the bots: spawning, removal, the per-tick driver (observe -> L0 reflexes -> job step),
// death and respawn. Server thread only.

#include "core/Events.h"
#include "core/Reflex.h"
#include "mod/Jobs.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"

#include <cstdint>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

class Level;
class SimulatedPlayer;

namespace karakuri {

struct Bot {
    std::string          name;
    std::unique_ptr<Job> job;    // the task given by a command or agent
    uint64_t             jobId{0};
    ItemCounts           invAtJobStart; // to report what the job picked up or used
    std::unique_ptr<Job> reflex; // an L0 reflex that has taken over from `job`
    ReflexArbiter        arbiter;
    bool                 reflexesEnabled{true};
    std::string          lastResult{"-"};
    int64_t              lastHurtTick{-100000};
    int64_t              lastHurtEventTick{-100000};
    int64_t              respawnAt{-1};
    int64_t              reflexRetryAt{0};
    bool                 respawnPointSet{false};
    int                  traceEvery{0}; // debug: log position every N ticks
};

class BotManager {
public:
    static BotManager& instance();

    // Returns an error message, or an empty string on success.
    std::string spawn(std::string const& name, Vec3 pos, int dimension);
    std::string remove(std::string const& name);
    void        removeAll();

    // `jobId` receives the id that job_started / job_finished events carry.
    std::string assign(std::string const& name, std::unique_ptr<Job> job, uint64_t* jobId = nullptr);
    std::string stop(std::string const& name);
    std::string status(std::string const& name);
    std::string hold(std::string const& name, int slot);
    std::string setReflexes(std::string const& name, bool on);
    std::vector<std::string> names() const;
    std::string              setTrace(std::string const& name, int every);
    SimulatedPlayer*         player(std::string const& name);

    void tick(Level& level);
    void onHurt(SimulatedPlayer& sp);
    void onDeath(SimulatedPlayer& sp);

    [[nodiscard]] int64_t now() const { return mTick; }
    EventLog&             events() { return mEvents; }

    struct Info {
        std::string      name;
        SimulatedPlayer* player{nullptr};
        std::string      job;
        uint64_t         jobId{0};
        std::string      reflex;
        bool             reflexesEnabled{true};
        std::string      lastResult;
    };
    std::optional<Info> info(std::string const& name);

private:
    Bot*             find(std::string const& name);
    SimulatedPlayer* resolve(Bot const& bot) const;
    void             tickBot(Bot& bot, SimulatedPlayer& sp);
    void             finishJob(Bot& bot, std::unique_ptr<Job>& slot, JobStep const& r, char const* kind);
    void             publishNames();
    void             emit(Bot const& bot, std::string kind, std::string detail, uint64_t jobId = 0, std::string state = {}, ItemChange change = {});

    std::map<std::string, Bot> mBots;
    int64_t                    mTick{0};
    uint64_t                   mNextJobId{1};
    EventLog                   mEvents;
};

} // namespace karakuri
