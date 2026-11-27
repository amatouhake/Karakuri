// Unit tests for the pure core (no game types). Run: xmake build KarakuriTests && xmake run KarakuriTests

#include "core/Crafting.h"
#include "core/Events.h"
#include "core/Inventory.h"
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

void inventoryDiff() {
    ItemCounts before{{"minecraft:dirt", 5}, {"minecraft:oak_log", 1}, {"minecraft:stick", 2}};
    ItemCounts after{{"minecraft:dirt", 4}, {"minecraft:oak_log", 3}, {"minecraft:oak_sapling", 1}};
    auto       c = diffCounts(before, after);
    check(c.size() == 4, "diff lists every changed item");
    check(formatChange(c) == "-1 dirt, +2 oak_log, +1 oak_sapling, -2 stick", "diff is sorted and formatted");
    check(diffCounts(before, before).empty(), "no change, empty diff");
}

void crafting() {
    // Wooden pickaxe: 3 planks (any kind) + 2 sticks.
    std::vector<Requirement> pickaxe{{{"oak_planks", "birch_planks"}, 3}, {{"stick"}, 2}};
    auto mixed = allocate(pickaxe, {{"oak_planks", 2}, {"birch_planks", 2}, {"stick", 5}});
    check(mixed.has_value(), "pickaxe: mixed planks are accepted");
    check(mixed && (*mixed)["stick"] == 2 && (*mixed)["oak_planks"] + (*mixed)["birch_planks"] == 3, "pickaxe: takes 3 planks and 2 sticks");
    check(!allocate(pickaxe, {{"oak_planks", 3}, {"stick", 1}}).has_value(), "pickaxe: one stick short");
    check(!allocate(pickaxe, {{"oak_planks", 5}, {"stick", 4}}, 2).has_value(), "two pickaxes need 6 planks");
    check(allocate(pickaxe, {{"oak_planks", 6}, {"stick", 4}}, 2).has_value(), "two pickaxes with enough");

    // A flexible slot must not use up the item a strict slot needs.
    std::vector<Requirement> tricky{{{"coal", "charcoal"}, 1}, {{"coal"}, 1}};
    auto t = allocate(tricky, {{"coal", 1}, {"charcoal", 1}});
    check(t && (*t)["coal"] == 1 && (*t)["charcoal"] == 1, "strict slots are filled first");

    check(craftsFor(5, 4) == 2 && craftsFor(4, 4) == 1 && craftsFor(1, 1) == 1, "crafts needed round up");
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
    inventoryDiff();
    crafting();
    pathfinderTests();
    if (gFailures == 0) std::printf("all tests passed\n");
    return gFailures == 0 ? 0 : 1;
}
