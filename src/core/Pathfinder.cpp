#include "core/Pathfinder.h"

#include <algorithm>
#include <cmath>

namespace karakuri::path {

char const* toString(Move m) {
    switch (m) {
    case Move::Walk:
        return "walk";
    case Move::Ascend:
        return "ascend";
    case Move::Descend:
        return "descend";
    case Move::Bridge:
        return "bridge";
    case Move::Pillar:
        return "pillar";
    case Move::DigDown:
        return "dig-down";
    }
    return "?";
}

char const* toString(SearchState s) {
    switch (s) {
    case SearchState::Running:
        return "running";
    case SearchState::Found:
        return "found";
    case SearchState::Partial:
        return "partial";
    case SearchState::NoPath:
        return "no path";
    }
    return "?";
}

bool canOccupy(Cell const& c) { return c.kind == CellKind::Air || c.kind == CellKind::Passable; }
bool canStandOn(Cell const& c) { return c.kind == CellKind::Solid || c.kind == CellKind::Partial; }
bool canBreak(Cell const& c) {
    return c.breakTicks >= 0
        && (c.kind == CellKind::Solid || c.kind == CellKind::Partial || c.kind == CellKind::Obstacle);
}

namespace {

constexpr Pos kDirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

bool liquidOrDanger(Cell const& c) { return c.kind == CellKind::Water || c.kind == CellKind::Danger; }

// `aboveCleared`: the block above is broken first, so a falling block there does not matter.
bool safeToBreakImpl(WorldView const& w, Pos p, bool aboveCleared) {
    if (!aboveCleared && w.cell(p.up()).falls) return false;
    static constexpr Pos around[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto const& d : around) {
        auto c = w.cell(p + d);
        if (liquidOrDanger(c) || c.kind == CellKind::Unknown) return false;
    }
    return true;
}

} // namespace

bool safeToBreak(WorldView const& w, Pos p) { return safeToBreakImpl(w, p, false); }

Search::Search(Pos start, Goal goal, Options opt) : mGoal(goal), mOpt(opt), mStart(start), mBest(start), mEnd(start) {
    mNodes[start] = Node{};
    mOpen.push({h(start) * mOpt.heuristicWeight, start});
}

static float distTo(Pos p, Goal const& g) {
    float ey = g.fromEyes ? 1.62f : 0.5f;
    float dx = (p.x + 0.5f) - (g.target.x + 0.5f);
    float dy = (p.y + ey) - (g.target.y + 0.5f);
    float dz = (p.z + 0.5f) - (g.target.z + 0.5f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool  Search::reached(Pos p) const { return distTo(p, mGoal) <= mGoal.range + 1e-3f; }
float Search::h(Pos p) const { return std::max(0.0f, distTo(p, mGoal) - mGoal.range) * Costs::walk; }

void Search::relax(Pos from, Step step, float cost, int scaffold) {
    if (scaffold > mOpt.scaffoldBlocks) return;
    float g2 = mNodes[from].g + cost;
    auto  it = mNodes.find(step.to);
    if (it != mNodes.end() && (it->second.closed || it->second.g <= g2)) return;
    Node n;
    n.g            = g2;
    n.scaffoldUsed = scaffold;
    n.parent       = from;
    n.hasParent    = true;
    n.via          = std::move(step);
    Pos to         = n.via.to;
    mNodes[to]     = std::move(n);
    mOpen.push({g2 + h(to) * mOpt.heuristicWeight, to});
}

void Search::expand(WorldView const& w, Pos p) {
    auto const& here = mNodes[p];
    int         sc   = here.scaffoldUsed;
    // The search does not change the world, so a block this path placed under the feet (bridge,
    // pillar) is only known from how the node was reached.
    bool placedFloor = here.hasParent && here.via.place && *here.via.place == p.down();
    Cell standing    = placedFloor ? Cell{CellKind::Solid, -1, false} : w.cell(p.down());
    // Cost of making `c` free for the body, appending to `breaks`; < 0 when impossible.
    auto clear = [&](Pos c, std::vector<Pos>& breaks, bool aboveCleared) -> float {
        auto cell = w.cell(c);
        if (canOccupy(cell)) return 0;
        if (!mOpt.allowDig || !canBreak(cell) || !safeToBreakImpl(w, c, aboveCleared)) return -1;
        breaks.push_back(c);
        return cell.breakTicks + Costs::breakExtra;
    };

    for (auto const& d : kDirs) {
        Pos  n     = p + d;
        Cell floor = w.cell(n.down());

        // Walk (breaking whatever is in the way), or bridge over a hole.
        {
            Step s{p, n, Move::Walk, {}, std::nullopt};
            float head = clear(n.up(), s.breaks, false);
            float feet = head < 0 ? -1 : clear(n, s.breaks, true);
            if (head >= 0 && feet >= 0) {
                if (canStandOn(floor)) {
                    relax(p, s, Costs::walk + head + feet, sc);
                } else if (canOccupy(floor) && canStandOn(standing)) {
                    s.move  = Move::Bridge;
                    s.place = n.down();
                    relax(p, s, Costs::walk + Costs::place + head + feet, sc + 1);
                }
            }
        }

        // Ascend one block: jump onto `n`.
        if (canStandOn(w.cell(n))) {
            Step  s{p, n.up(), Move::Ascend, {}, std::nullopt};
            float a = clear(p.up(2), s.breaks, false);
            float b = a < 0 ? -1 : clear(n.up(2), s.breaks, false);
            float c = b < 0 ? -1 : clear(n.up(), s.breaks, true);
            if (a >= 0 && b >= 0 && c >= 0) relax(p, s, Costs::ascend + a + b + c, sc);
        }

        // Descend: walk off the edge and fall up to maxFall blocks (no digging on the way).
        if (canOccupy(w.cell(n)) && canOccupy(w.cell(n.up())) && canOccupy(floor)) {
            for (int j = 2; j <= mOpt.maxFall + 1; ++j) {
                Cell c = w.cell(n.down(j));
                if (canStandOn(c)) {
                    relax(p, Step{p, n.down(j - 1), Move::Descend, {}, std::nullopt}, Costs::walk + (j - 1) * Costs::fallPerBlock, sc);
                    break;
                }
                if (!canOccupy(c)) break;
            }
        }
    }

    // Pillar: jump and place a block where the feet were.
    {
        Step  s{p, p.up(), Move::Pillar, {}, p};
        float c = clear(p.up(2), s.breaks, false);
        if (c >= 0) relax(p, s, Costs::pillar + c, sc + 1);
    }

    // Dig down onto the block below the one we stand on.
    if (mOpt.allowDig && !placedFloor) {
        Pos  below = p.down();
        Cell b     = w.cell(below);
        if (canBreak(b) && canStandOn(w.cell(p.down(2))) && safeToBreakImpl(w, below, true)) {
            relax(p, Step{p, below, Move::DigDown, {below}, std::nullopt}, b.breakTicks + Costs::breakExtra + Costs::walk, sc);
        }
    }
}

SearchState Search::step(WorldView const& world, int budget) {
    if (mState != SearchState::Running) return mState;
    while (budget-- > 0) {
        if (mOpen.empty() || mExpanded >= mMaxNodes) {
            mEnd   = mBest;
            mState = mBest == mStart ? SearchState::NoPath : SearchState::Partial;
            return mState;
        }
        Pos p = mOpen.top().p;
        mOpen.pop();
        auto& node = mNodes[p];
        if (node.closed) continue;
        node.closed = true;
        ++mExpanded;
        if (reached(p)) {
            mEnd   = p;
            mState = SearchState::Found;
            return mState;
        }
        if (float hp = h(p); hp < mBestH) {
            mBestH = hp;
            mBest  = p;
        }
        expand(world, p);
    }
    return mState;
}

std::vector<Step> Search::path() const {
    std::vector<Step> out;
    if (mState != SearchState::Found && mState != SearchState::Partial) return out;
    for (Pos p = mEnd;;) {
        auto const& n = mNodes.at(p);
        if (!n.hasParent) break;
        out.push_back(n.via);
        p = n.parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

} // namespace karakuri::path
