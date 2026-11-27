// Unit tests for the pure core (no game types). Run: xmake build KarakuriTests && xmake run KarakuriTests

#include "core/Events.h"
#include "core/Job.h"
#include "core/Reflex.h"

#include <cstdio>

void pathfinderTests();

namespace {
int gFailures = 0;
}

void check(bool ok, char const* what) {
    if (!ok) {
        ++gFailures;
        std::printf("FAIL: %s\n", what);
    }
}

namespace {

using namespace karakuri;

void progressWatch() {
    ProgressWatch w(3, 0.5);
    w.reset(10);
    check(!w.update(9), "progress resets the window");
    check(!w.update(9), "no gain tick 1");
    check(!w.update(8.8), "gain below minGain is no progress (tick 2)");
    check(w.update(8.8), "stuck after window ticks without progress");
    w.reset(5);
    check(!w.update(5), "reset clears the stuck state");
}

void cooldown() {
    Cooldown c(2);
    check(c.ready(), "cooldown starts ready");
    c.trigger();
    check(!c.ready(), "not ready right after trigger");
    c.tick();
    c.tick();
    check(c.ready(), "ready after its ticks");
}

void reflexEat() {
    ReflexArbiter a;
    BodyState     s;
    s.hasFood = true;
    s.hunger  = 10;
    check(a.choose(s) == Reflex::None, "no eating while fairly fed");
    s.hunger = 6;
    check(a.choose(s) == Reflex::Eat, "eat at the start threshold");
    s.hunger = 12;
    check(a.choose(s) == Reflex::Eat, "keep eating until the stop threshold (hysteresis)");
    s.hunger = 17;
    check(a.choose(s) == Reflex::None, "stop eating at the stop threshold");
    s.hunger  = 2;
    s.hasFood = false;
    check(a.choose(s) == Reflex::None, "cannot eat without food");
}

void reflexFight() {
    ReflexConfig cfg;
    cfg.fightCalmTicks = 3;
    ReflexArbiter a(cfg);
    BodyState     s;
    s.threatNear = true;
    check(a.choose(s) == Reflex::None, "a nearby mob alone does not start a fight");
    s.ticksSinceHurt = 5;
    check(a.choose(s) == Reflex::Fight, "fight back when hurt with a threat near");
    s.threatNear     = false;
    s.ticksSinceHurt = 500;
    check(a.choose(s) == Reflex::Fight, "calm tick 1 keeps fighting");
    check(a.choose(s) == Reflex::Fight, "calm tick 2 keeps fighting");
    check(a.choose(s) == Reflex::None, "stop after calm ticks");

    s.threatNear     = true;
    s.ticksSinceHurt = 1;
    s.hasFood        = true;
    s.hunger         = 2;
    check(a.choose(s) == Reflex::Fight, "fighting wins over eating");
}

void events() {
    EventLog log(3);
    log.push({.bot = "a", .kind = "job_started", .jobId = 1});
    log.push({.bot = "b", .kind = "hurt"});
    auto id3 = log.push({.bot = "a", .kind = "job_finished", .jobId = 1, .state = "done"});
    check(id3 == 3 && log.lastId() == 3, "event ids count up");
    auto onlyA = log.since(0, [](Event const& e) { return e.bot == "a"; });
    check(onlyA.size() == 2 && onlyA[1].state == "done", "filter by bot");
    log.push({.bot = "c", .kind = "died"});
    check(log.since(0).size() == 3 && log.since(0).front().id == 2, "capacity drops the oldest");
    auto none = log.wait(4, std::chrono::milliseconds(10));
    check(none.empty(), "wait times out with nothing new");
    auto got = log.wait(3, std::chrono::milliseconds(10));
    check(got.size() == 1 && got[0].kind == "died", "wait returns what is already there");
    log.shutdown();
    check(log.wait(4, std::chrono::seconds(5)).empty(), "shutdown wakes waiters");
}

void reach() {
    check(withinReach(0, 0, 0, 3, 4, 0, 5), "distance 5 is within reach 5");
    check(!withinReach(0, 0, 0, 3, 4, 0.1f, 5), "just over reach is out");
}

} // namespace

int main() {
    progressWatch();
    cooldown();
    reflexEat();
    reflexFight();
    reach();
    events();
    pathfinderTests();
    if (gFailures == 0) std::printf("all tests passed\n");
    return gFailures == 0 ? 0 : 1;
}
