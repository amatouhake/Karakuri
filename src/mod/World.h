#pragma once

// Game-side adapter for the pathfinder: classifies blocks into path::Cell, and picks tools and
// scaffold blocks from the hotbar. Server thread only; never keep one across ticks.

#include "core/Pathfinder.h"

class Block;
class BlockSource;
class Player;

namespace karakuri {

class BlockWorld final : public path::WorldView {
public:
    BlockWorld(BlockSource& region, Player& player) : mRegion(region), mPlayer(player) {}
    path::Cell cell(path::Pos p) const override;

private:
    BlockSource& mRegion;
    Player&      mPlayer;
};

// Classification from the block alone (break time not filled in).
path::CellKind classify(Block const& b);

// The hotbar slot whose item breaks `b` fastest (the selected slot when nothing is better).
int bestToolSlot(Player const& p, Block const& b);
// Ticks to break `b` with the best hotbar tool; < 0 when it cannot be broken.
float breakTicks(Player& p, Block const& b);

// Hotbar slot holding a plain building block for bridging and pillaring, or -1.
int  scaffoldSlot(Player const& p);
int  scaffoldCount(Player const& p);

} // namespace karakuri
