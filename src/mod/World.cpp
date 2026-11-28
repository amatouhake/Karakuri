#include "mod/World.h"

#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/material/Material.h"

#include <array>
#include <string_view>

namespace karakuri {

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Blocks that hurt or trap: never entered, never stood on.
constexpr std::array kDanger{
    "minecraft:fire",          "minecraft:soul_fire",   "minecraft:magma",          "minecraft:cactus",
    "minecraft:sweet_berry_bush", "minecraft:powder_snow", "minecraft:wither_rose",   "minecraft:campfire",
    "minecraft:soul_campfire", "minecraft:web",         "minecraft:pointed_dripstone",
};

// Plain full blocks used for bridging and pillaring.
constexpr std::array kScaffold{
    "minecraft:dirt",       "minecraft:cobblestone", "minecraft:cobbled_deepslate", "minecraft:netherrack",
    "minecraft:stone",      "minecraft:andesite",    "minecraft:diorite",           "minecraft:granite",
    "minecraft:deepslate",  "minecraft:tuff",        "minecraft:blackstone",        "minecraft:end_stone",
};

bool isScaffold(ItemStack const& it) {
    if (it.isNull()) return false;
    auto const& name = it.getTypeName();
    if (endsWith(name, "_planks")) return true;
    for (auto const* s : kScaffold)
        if (name == s) return true;
    return false;
}

} // namespace

path::CellKind classify(Block const& b) {
    using K = path::CellKind;
    if (b.isAir()) return K::Air;
    auto const& name = b.getTypeName();
    auto const& mat  = b.getMaterial();
    if (mat.mLiquid) return contains(name, "lava") ? K::Danger : K::Water;
    for (auto const* d : kDanger)
        if (name == d) return K::Danger;
    if (!mat.mBlocksMotion) return K::Passable;
    if (b.getBlockType().mSolid) return K::Solid;
    if (contains(name, "fence") || contains(name, "_wall") || contains(name, "pane") || contains(name, "iron_bars")
        || contains(name, "door"))
        return K::Obstacle;
    return K::Partial;
}

int bestToolSlot(Player const& p, Block const& b) {
    auto& inv  = p.getInventory();
    int   best = p.getSelectedItemSlot();
    float top  = b.getDestroySpeed(inv.getItem(best));
    for (int i = 0; i < 9; ++i) {
        float s = b.getDestroySpeed(inv.getItem(i));
        if (s > top) {
            top  = s;
            best = i;
        }
    }
    return best;
}

float breakTicks(Player& p, Block const& b) {
    float progress = p.getDestroyProgress(b); // per tick, with the held item
    if (progress <= 0) return -1;
    auto& inv   = p.getInventory();
    float held  = b.getDestroySpeed(p.getSelectedItem());
    float best  = b.getDestroySpeed(inv.getItem(bestToolSlot(p, b)));
    float ratio = (held > 0 && best > held) ? held / best : 1.0f;
    return ratio / progress;
}

int bestWeaponSlot(Player const& p) {
    auto& inv  = p.getInventory();
    int   best = p.getSelectedItemSlot();
    int   top  = 1; // the bare hand
    for (int i = 0; i < 9; ++i) {
        auto const& it = inv.getItem(i);
        if (it.isNull()) continue;
        Item const* item = it.mItem.get();
        if (item && item->getAttackDamage() > top) {
            top  = item->getAttackDamage();
            best = i;
        }
    }
    return best;
}

int harvestSlot(Player const& p, Block const& b) {
    int fastest = bestToolSlot(p, b);
    if (!b.getBlockType().mRequiresCorrectToolForDrops) return fastest;
    auto& inv  = p.getInventory();
    auto  gets = [&](int slot) {
        auto const& it = inv.getItem(slot);
        Item const* item = it.isNull() ? nullptr : it.mItem.get();
        return item && item->canDestroySpecial(b);
    };
    if (gets(fastest)) return fastest;
    int   best = -1;
    float top  = -1;
    for (int i = 0; i < 9; ++i) {
        if (!gets(i)) continue;
        float s = b.getDestroySpeed(inv.getItem(i));
        if (s > top) {
            top  = s;
            best = i;
        }
    }
    return best;
}

int scaffoldSlot(Player const& p) {
    auto& inv = p.getInventory();
    if (isScaffold(p.getSelectedItem())) return p.getSelectedItemSlot();
    for (int i = 0; i < 9; ++i)
        if (isScaffold(inv.getItem(i))) return i;
    return -1;
}

int scaffoldCount(Player const& p) {
    auto& inv = p.getInventory();
    int   n   = 0;
    for (int i = 0; i < 9; ++i)
        if (isScaffold(inv.getItem(i))) n += inv.getItem(i).mCount;
    return n;
}

path::Cell BlockWorld::cell(path::Pos p) const {
    BlockPos bp{p.x, p.y, p.z};
    if (!mRegion.getChunkAt(bp)) return {};
    auto const& b = mRegion.getBlock(bp);
    path::Cell  c;
    c.kind  = classify(b);
    c.falls = b.getBlockType().mFalling && c.kind == path::CellKind::Solid;
    if (c.kind == path::CellKind::Solid || c.kind == path::CellKind::Partial || c.kind == path::CellKind::Obstacle)
        c.breakTicks = breakTicks(mPlayer, b);
    return c;
}

} // namespace karakuri
