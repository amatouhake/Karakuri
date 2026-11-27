#include "mod/Game.h"

#include "core/Reflex.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include <cmath>

namespace karakuri {

float hunger(Player const& p) {
    auto ref = p.getAttribute(Player::HUNGER());
    return ref.mPtr ? ref.mPtr->mCurrentValue : -1.0f;
}

bool isFood(Player const& p, int slot) {
    auto const& it = p.getInventory().getItem(slot);
    if (it.isNull()) return false;
    Item const* item = it.mItem.get();
    return item && (item->isFood() || item->getFood() != nullptr);
}

int findFoodSlot(Player const& p) {
    int selected = p.getSelectedItemSlot();
    if (isFood(p, selected)) return selected;
    // Only the hotbar can be selected.
    for (int i = 0; i < 9; ++i) {
        if (isFood(p, i)) return i;
    }
    return -1;
}

std::string blockName(BlockSource& region, BlockPos const& pos) {
    return std::string(region.getBlock(pos).getTypeName());
}

bool isAir(BlockSource& region, BlockPos const& pos) { return region.getBlock(pos).isAir(); }

bool isSolid(BlockSource& region, BlockPos const& pos) {
    auto const& b = region.getBlock(pos);
    return !b.isAir() && b.getMaterial().mSolid;
}

Actor* nearestMonster(Player& p, float radius) {
    auto   center = p.getPosition();
    AABB   box{center - Vec3{radius, radius, radius}, center + Vec3{radius, radius, radius}};
    Actor* best   = nullptr;
    float  bestD2 = radius * radius;
    for (auto* a : p.getDimensionBlockSource().getEntities(box, 0.0f)) {
        if (!a || a == &p || !a->isAlive() || !a->hasCategory(ActorCategory::Monster)) continue;
        float d2 = a->getPosition().distanceToSqr(center);
        if (d2 <= bestD2) {
            bestD2 = d2;
            best   = a;
        }
    }
    return best;
}

ScriptModuleMinecraft::ScriptFacing faceToward(BlockPos const& block, Vec3 const& eye) {
    using F  = ScriptModuleMinecraft::ScriptFacing;
    float dx = eye.x - (block.x + 0.5f), dy = eye.y - (block.y + 0.5f), dz = eye.z - (block.z + 0.5f);
    float ax = std::abs(dx), ay = std::abs(dy), az = std::abs(dz);
    if (ay >= ax && ay >= az) return dy > 0 ? F::Up : F::Down;
    if (ax >= az) return dx > 0 ? F::East : F::West;
    return dz > 0 ? F::South : F::North;
}

bool withinReach(Player const& p, Vec3 const& target, float reach) {
    auto eye = p.getEyePos();
    return karakuri::withinReach(eye.x, eye.y, eye.z, target.x, target.y, target.z, reach);
}

std::string inventorySummary(Player const& p) {
    std::string out;
    auto&       inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (it.isNull()) continue;
        out += fmt::format("{}:{}x{} ", i, it.getTypeName(), (int)it.mCount);
    }
    return out.empty() ? "(empty)" : out;
}

ItemCounts inventoryCounts(Player const& p) {
    ItemCounts out;
    auto&      inv = p.getInventory();
    for (int i = 0; i < inv.getContainerSize(); ++i) {
        auto const& it = inv.getItem(i);
        if (!it.isNull()) out[it.getTypeName()] += it.mCount;
    }
    return out;
}

std::string describe(SimulatedPlayer& sp) {
    auto pos = sp.getFeetPos();
    return fmt::format(
        "pos=({:.1f},{:.1f},{:.1f}) dim={} hp={} hunger={:.0f} alive={} slot={} held={}",
        pos.x,
        pos.y,
        pos.z,
        (int)sp.getDimensionId(),
        sp.getHealth(),
        hunger(sp),
        sp.isAlive(),
        sp.getSelectedItemSlot(),
        sp.getSelectedItem().isNull() ? "-" : sp.getSelectedItem().getTypeName()
    );
}

} // namespace karakuri
