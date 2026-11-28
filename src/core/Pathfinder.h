#pragma once

// Block-grid A* for one bot (docs/DESIGN.md 7, step 2). Pure: the world is read through
// WorldView, so the search is unit tested on ASCII maps. The search is resumable
// (`step(world, budget)`) so the mod can spread it over ticks; the world reference is only used
// during `step`.
//
// A node is the block the bot's feet occupy. Moves are the four horizontal neighbors plus
// straight up (pillar) and straight down (dig down), and swimming along a water surface (feet in
// water, head in air). No diagonals and no diving.

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace karakuri::path {

struct Pos {
    int x{0}, y{0}, z{0};

    friend bool operator==(Pos const&, Pos const&) = default;
    Pos         operator+(Pos const& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Pos         up(int n = 1) const { return {x, y + n, z}; }
    Pos         down(int n = 1) const { return {x, y - n, z}; }
};

struct PosHash {
    size_t operator()(Pos const& p) const noexcept {
        return std::hash<int64_t>{}(((int64_t)p.x * 73856093) ^ ((int64_t)p.y * 19349663) ^ ((int64_t)p.z * 83492791));
    }
};

enum class CellKind : uint8_t {
    Unknown,  // not loaded: never entered or broken
    Air,
    Passable, // no collision: grass, flowers, torches, snow layers
    Solid,    // full block: stand on it
    Partial,  // collides and can be stood on: slabs, stairs, leaves, glass, beds
    Obstacle, // collides but cannot be stood on or jumped onto: fences, walls, panes, doors
    Water,    // not handled yet: avoided
    Danger,   // lava, fire, magma, cactus, cobwebs...: never touched
};

struct Cell {
    CellKind kind{CellKind::Unknown};
    float    breakTicks{-1}; // ticks to break with the best tool at hand; < 0 = unbreakable
    bool     falls{false};   // sand, gravel: falls when unsupported
};

class WorldView {
public:
    virtual ~WorldView()            = default;
    virtual Cell cell(Pos p) const = 0;
};

enum class Move : uint8_t { Walk, Ascend, Descend, Bridge, Pillar, DigDown, Swim };
char const* toString(Move m);

struct Step {
    Pos                from, to;
    Move               move{Move::Walk};
    std::vector<Pos>   breaks; // blocks to break before moving, in order
    std::optional<Pos> place;  // block to place (Bridge: under `to`; Pillar: at `from`)
};

struct Goal {
    Pos   target;
    float range{0};          // done when the distance from the feet (or eyes) to `target` <= range
    bool  fromEyes{false};   // measure from eye height (for "within reach of a block")
};

struct Options {
    bool  allowDig{true};
    int   scaffoldBlocks{0}; // blocks available for bridging and pillaring
    int   maxFall{3};
    int   maxWaterFall{20}; // falls into water do no damage
    float heuristicWeight{1.2f};
};

// Cost unit: ticks (walking one block takes about 4.6 ticks).
struct Costs {
    static constexpr float walk       = 5;
    static constexpr float ascend     = 8;
    static constexpr float fallPerBlock = 2;
    static constexpr float place      = 10;
    static constexpr float pillar     = 15;
    static constexpr float breakExtra = 5; // aiming and reach overhead per broken block
    static constexpr float swim       = 12; // swimming is slow and leaves the bot exposed
};

enum class SearchState { Running, Found, Partial, NoPath };
char const* toString(SearchState s);

class Search {
public:
    Search(Pos start, Goal goal, Options opt = {});

    // Expands up to `budget` nodes. Returns Running until done.
    SearchState step(WorldView const& world, int budget);

    [[nodiscard]] SearchState state() const { return mState; }
    // The path to the goal (Found) or to the node closest to it (Partial).
    [[nodiscard]] std::vector<Step> path() const;
    [[nodiscard]] int               expanded() const { return mExpanded; }
    void                            setMaxNodes(int n) { mMaxNodes = n; }

private:
    struct Node {
        float g{0};
        int   scaffoldUsed{0};
        Pos   parent;
        bool  hasParent{false};
        Step  via;
        bool  closed{false};
    };
    struct Open {
        float f;
        Pos   p;
        bool  operator<(Open const& o) const { return f > o.f; }
    };

    bool  reached(Pos p) const;
    float h(Pos p) const;
    void  expand(WorldView const& w, Pos p);
    void  relax(Pos from, Step step, float cost, int scaffold);

    Goal                                   mGoal;
    Options                                mOpt;
    Pos                                    mStart;
    std::unordered_map<Pos, Node, PosHash> mNodes;
    std::priority_queue<Open>              mOpen;
    SearchState                            mState{SearchState::Running};
    Pos                                    mBest;
    float                                  mBestH{1e30f};
    Pos                                    mEnd;
    int                                    mExpanded{0};
    int                                    mMaxNodes{40000};
};

// Distance used by the goal test (from the feet cell, or from eye height when `fromEyes`).
float distanceToGoal(Pos p, Goal const& g);
inline bool goalReached(Pos p, Goal const& g) { return distanceToGoal(p, g) <= g.range + 1e-3f; }

// Cell predicates, shared with the executor.
bool canOccupy(Cell const& c);  // feet or head may be here
bool canStandOn(Cell const& c); // a floor
bool canBreak(Cell const& c);
// Breaking `p` is safe: no liquid or danger next to it and no falling block above it.
bool safeToBreak(WorldView const& w, Pos p);

} // namespace karakuri::path
