#pragma once

// Thin helpers over game types. Everything here runs on the server thread.

#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/scripting/modules/minecraft/ScriptFacing.h"
#include "mc/world/level/BlockPos.h"

#include "core/Inventory.h"

#include <string>

class Actor;
class BlockSource;
class Player;
class SimulatedPlayer;

namespace karakuri {

// Survival reach used by skills. Bedrock's own default for simulated block breaking is 5.5.
inline constexpr float kReach = 5.5f;

float hunger(Player const& p);
int   findFoodSlot(Player const& p); // -1 when there is none; prefers the selected slot
bool  isFood(Player const& p, int slot);

std::string blockName(BlockSource& region, BlockPos const& pos);
bool        isAir(BlockSource& region, BlockPos const& pos);
bool        isSolid(BlockSource& region, BlockPos const& pos);

// Nearest living monster within `radius` of the player, or nullptr.
Actor* nearestMonster(Player& p, float radius);

// The face of `block` that points toward `eye`.
ScriptModuleMinecraft::ScriptFacing faceToward(BlockPos const& block, Vec3 const& eye);

bool withinReach(Player const& p, Vec3 const& target, float reach = kReach);

std::string describe(SimulatedPlayer& sp);
std::string inventorySummary(Player const& p);
ItemCounts  inventoryCounts(Player const& p);

} // namespace karakuri
