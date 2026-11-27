// Pathfinder tests on ASCII maps. Each map is a list of horizontal layers from y=0 upward; in a
// layer, rows are z and characters are x. Outside the map everything is Unknown.
//   #  stone (breakable, 30 ticks)    B  bedrock (unbreakable)   .  air    g  grass (passable)
//   L  lava    W  water    F  fence    S  sand (falls)

#include "core/Pathfinder.h"

#include <cstdio>
#include <string>
#include <vector>

void check(bool ok, char const* what);

namespace {

using namespace karakuri::path;

class MapWorld final : public WorldView {
public:
    explicit MapWorld(std::vector<std::vector<std::string>> layers) : mLayers(std::move(layers)) {}

    Cell cell(Pos p) const override {
        if (p.y < 0 || p.y >= (int)mLayers.size()) return {};
        auto const& layer = mLayers[p.y];
        if (p.z < 0 || p.z >= (int)layer.size()) return {};
        auto const& row = layer[p.z];
        if (p.x < 0 || p.x >= (int)row.size()) return {};
        switch (row[p.x]) {
        case '#':
            return {CellKind::Solid, 30, false};
        case 'B':
            return {CellKind::Solid, -1, false};
        case 'S':
            return {CellKind::Solid, 10, true};
        case 'g':
            return {CellKind::Passable, 0, false};
        case 'L':
            return {CellKind::Danger, -1, false};
        case 'W':
            return {CellKind::Water, -1, false};
        case 'F':
            return {CellKind::Obstacle, 30, false};
        default:
            return {CellKind::Air, 0, false};
        }
    }

private:
    std::vector<std::vector<std::string>> mLayers;
};

struct Result {
    SearchState       state;
    std::vector<Step> path;
};

Result run(MapWorld const& w, Pos start, Goal goal, Options opt = {}, int maxNodes = 40000) {
    Search s(start, goal, opt);
    s.setMaxNodes(maxNodes);
    while (s.step(w, 100) == SearchState::Running) {}
    return {s.state(), s.path()};
}

int count(std::vector<Step> const& path, Move m) {
    int n = 0;
    for (auto const& s : path) n += s.move == m;
    return n;
}

int breaks(std::vector<Step> const& path) {
    int n = 0;
    for (auto const& s : path) n += (int)s.breaks.size();
    return n;
}

bool touches(std::vector<Step> const& path, Pos p) {
    for (auto const& s : path) {
        if (s.to == p || s.to.up() == p) return true;
        for (auto const& b : s.breaks)
            if (b == p) return true;
    }
    return false;
}

// A 1-wide corridor (z = 1) with walls at z = 0 and z = 2 so the search cannot walk around.
std::vector<std::string> corridor(std::string const& middle, char wall = 'B') {
    return {std::string(middle.size(), wall), middle, std::string(middle.size(), wall)};
}

void walk() {
    MapWorld w({corridor("BBBBBBBB"), corridor("........"), corridor("........"), corridor("BBBBBBBB")});
    auto     r = run(w, {0, 1, 1}, {{7, 1, 1}});
    check(r.state == SearchState::Found, "walk: found");
    check(r.path.size() == 7 && count(r.path, Move::Walk) == 7, "walk: seven walk steps");
}

void stepUpAndDrop() {
    // Floor at y=0; a block at x=3 (y=1) to climb; then a 3-block drop at x=6.
    MapWorld deep({
        corridor("BBBBBBBBB"), // y0: landing floor
        corridor("BBBBBB..."), // y1
        corridor("BBBBBB..."), // y2
        corridor("BBBBBB..."), // y3: top floor ends at x=5
        corridor("...#....."), // y4: feet level, a step at x=3
        corridor("........."), // y5
        corridor(".........", '.'),
    });
    auto r = run(deep, {0, 4, 1}, {{8, 1, 1}}, {.allowDig = false});
    check(r.state == SearchState::Found, "step+drop: found");
    check(count(r.path, Move::Ascend) == 1, "step+drop: one ascend");
    check(count(r.path, Move::Descend) >= 1, "step+drop: descends");
}

void dropTooHigh() {
    MapWorld w({
        corridor("BBBBBBBB"), // y0: landing 5 below
        corridor("BBBB...."),
        corridor("BBBB...."),
        corridor("BBBB...."),
        corridor("BBBB...."),
        corridor("BBBB...."), // y5: top floor ends at x=3
        corridor("........"),
        corridor("........"),
        corridor("........", '.'),
    });
    auto r = run(w, {0, 6, 1}, {{7, 1, 1}}, {.allowDig = false});
    check(r.state != SearchState::Found, "drop of 5 is refused");
}

void digThroughWall() {
    MapWorld w({corridor("BBBBBBB"), corridor("...#..."), corridor("...#..."), corridor("BBBBBBB")});
    auto     dig = run(w, {0, 1, 1}, {{6, 1, 1}});
    check(dig.state == SearchState::Found, "wall: found with digging");
    check(breaks(dig.path) == 2, "wall: breaks the two wall blocks");
    auto noDig = run(w, {0, 1, 1}, {{6, 1, 1}}, {.allowDig = false});
    check(noDig.state != SearchState::Found, "wall: no path without digging");
}

void bedrockWall() {
    MapWorld w({corridor("BBBBBBB"), corridor("...B..."), corridor("...B..."), corridor("BBBBBBB")});
    auto     r = run(w, {0, 1, 1}, {{6, 1, 1}});
    check(r.state != SearchState::Found, "bedrock wall cannot be dug");
}

void pillarUp() {
    MapWorld w({corridor("BBBBB"), corridor("....."), corridor("....."), corridor("....."), corridor("....."), corridor(".....", '.')});
    auto     with = run(w, {2, 1, 1}, {{2, 4, 1}}, {.scaffoldBlocks = 5});
    check(with.state == SearchState::Found, "pillar: found with blocks");
    check(count(with.path, Move::Pillar) == 3, "pillar: three pillar steps");
    auto without = run(w, {2, 1, 1}, {{2, 4, 1}}, {.scaffoldBlocks = 2});
    check(without.state != SearchState::Found, "pillar: not enough blocks");
}

void bridgeGap() {
    MapWorld w({
        corridor("BBBBBBBBBB"), // y0 deep floor (far below)
        corridor("BBB......."),
        corridor("BBB......."),
        corridor("BBB......."),
        corridor("BBB......."),
        corridor("BBB...BBBB"), // y5: gap x=3..5, 5 deep: no descend
        corridor(".........."),
        corridor(".........."),
        corridor("..........", '.'),
    });
    auto with = run(w, {0, 6, 1}, {{9, 6, 1}}, {.allowDig = false, .scaffoldBlocks = 5});
    check(with.state == SearchState::Found, "bridge: found with blocks");
    check(count(with.path, Move::Bridge) == 3, "bridge: three bridge steps");
    auto none = run(w, {0, 6, 1}, {{9, 6, 1}}, {.allowDig = false, .scaffoldBlocks = 0});
    check(none.state != SearchState::Found, "bridge: no path without blocks");
}

void lavaDetour() {
    // Open floor, lava pool in the middle of the straight line.
    std::vector<std::string> floor   = {"BBBBBBB", "BBBBBBB", "BBBBBBB", "BBBBBBB", "BBBBBBB"};
    std::vector<std::string> feet    = {".......", ".......", "...L...", ".......", "......."};
    std::vector<std::string> head    = {".......", ".......", ".......", ".......", "......."};
    MapWorld                 w({floor, feet, head, head});
    auto                     r = run(w, {0, 1, 2}, {{6, 1, 2}});
    check(r.state == SearchState::Found, "lava: found");
    check(!touches(r.path, {3, 1, 2}), "lava: never enters the lava cell");
}

void noDigNextToLava() {
    // The only way is through a wall block that touches lava: digging it is unsafe.
    MapWorld w({corridor("BBBBBBB"), corridor("...#..."), corridor("...#..."), {"BBBBBBB", "BBBLBBB", "BBBBBBB"}});
    auto     r = run(w, {0, 1, 1}, {{6, 1, 1}});
    check(r.state != SearchState::Found, "no digging into a block next to lava");
}

void noDigUnderSand() {
    // A sand column over the wall reaching past the top of the map: every block of it has sand
    // or unknown space above, so none of it may be broken.
    MapWorld w({corridor("BBBBBBB"), corridor("...#..."), corridor("...S..."), corridor("...S..."), corridor("...S...")});
    auto     r = run(w, {0, 1, 1}, {{6, 1, 1}});
    check(r.state != SearchState::Found, "no digging under falling blocks");
}

void reachGoal() {
    MapWorld w({corridor("BBBBBBBBBB"), corridor(".........#"), corridor(".........."), corridor("BBBBBBBBBB")});
    auto     r = run(w, {0, 1, 1}, {{9, 1, 1}, 4.5f, true}, {.allowDig = false});
    check(r.state == SearchState::Found, "reach goal: found");
    check(!r.path.empty() && r.path.back().to.x <= 8, "reach goal: stops within reach, not on the block");
}

void unreachableAndBudget() {
    MapWorld box({corridor("BBBBBBB"), corridor("B.BBB.B"), corridor("B.BBB.B"), corridor("BBBBBBB")});
    auto     r = run(box, {1, 1, 1}, {{5, 1, 1}});
    check(r.state == SearchState::NoPath, "sealed: no path");

    MapWorld open({corridor(std::string(60, 'B')), corridor(std::string(60, '.')), corridor(std::string(60, '.'))});
    auto     part = run(open, {0, 1, 1}, {{59, 1, 1}}, {}, 20);
    check(part.state == SearchState::Partial, "budget: partial result");
    check(!part.path.empty() && part.path.back().to.x > 5, "budget: partial path heads toward the goal");
}

void unknownIsImpassable() {
    MapWorld w({corridor("BBBB"), corridor("...."), corridor("....")});
    auto     r = run(w, {0, 1, 1}, {{8, 1, 1}});
    check(r.state != SearchState::Found, "unknown cells are not entered");
}

} // namespace

void pathfinderTests() {
    walk();
    stepUpAndDrop();
    dropTooHigh();
    digThroughWall();
    bedrockWall();
    pillarUp();
    bridgeGap();
    lavaDetour();
    noDigNextToLava();
    noDigUnderSand();
    reachGoal();
    unreachableAndBudget();
    unknownIsImpassable();
}
