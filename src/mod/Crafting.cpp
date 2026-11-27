// Crafting, smelting and chests (docs/DESIGN.md 8.1). The rules are the player's: recipes and
// material use, a crafting table or furnace within reach, only containers within reach. The means
// are direct: items move between inventories server-side instead of through GUI slot clicks.

#include "mod/Jobs.h"

#include "core/Crafting.h"
#include "mod/Game.h"
#include "mod/Mod.h"

#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/server/SimulatedPlayer.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/crafting/Recipe.h"
#include "mc/world/item/crafting/RecipeIngredient.h"
#include "mc/world/item/crafting/Recipes.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/ChestBlockActor.h"
#include "mc/world/level/block/actor/FurnaceBlockActor.h"

#include <cmath>
#include <optional>
#include <set>

namespace karakuri {

namespace {

constexpr int kStationSearch = 16; // how far to look for a crafting table, furnace or chest

std::string fullName(std::string name) {
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    return name;
}

std::string shortName(std::string const& name) { return name.rfind("minecraft:", 0) == 0 ? name.substr(10) : name; }

Vec3 center(BlockPos const& p) { return {p.x + 0.5f, p.y + 0.5f, p.z + 0.5f}; }

// BlockActor declares no getContainer(); the container block actors we use expose it themselves.
Container* containerAt(BlockSource& region, BlockPos const& pos) {
    auto* be = region.getBlockEntity(pos);
    if (!be) return nullptr;
    switch (be->getType()) {
    case BlockActorType::Furnace:
        return static_cast<FurnaceBlockActor*>(be);
    case BlockActorType::Chest:
    case BlockActorType::BarrelBlock:
        return static_cast<ChestBlockActor*>(be)->getContainer();
    default:
        return nullptr;
    }
}

// ---- Inventory moves ---------------------------------------------------------------------------

int countIn(Container const& c, std::string const& name, int slots) {
    int n = 0;
    for (int i = 0; i < slots; ++i) {
        auto const& it = c.getItem(i);
        if (!it.isNull() && (name.empty() || it.getTypeName() == name)) n += it.mCount;
    }
    return n;
}

// Moves up to `count` items named `name` (any item when empty) from `from` to `to`, merging into
// matching stacks first. Returns how many moved.
int moveItems(Container& from, int fromSlots, Container& to, int toSlots, std::string const& name, int count) {
    int moved = 0;
    for (int i = 0; i < fromSlots && moved < count; ++i) {
        ItemStack src = from.getItem(i);
        if (src.isNull() || (!name.empty() && src.getTypeName() != name)) continue;
        int want = std::min<int>(src.mCount, count - moved);
        int max  = std::min<int>(src.getMaxStackSize(), to.getMaxStackSize());
        int left = want;
        for (int pass = 0; pass < 2 && left > 0; ++pass) { // pass 0: merge, pass 1: empty slots
            for (int j = 0; j < toSlots && left > 0; ++j) {
                ItemStack dst = to.getItem(j);
                if (pass == 0 && (dst.isNull() || !dst.sameItem(src, {}) || dst.mCount >= max)) continue;
                if (pass == 1 && !dst.isNull()) continue;
                int put = std::min(left, pass == 0 ? max - dst.mCount : max);
                if (pass == 0) {
                    dst.set(dst.mCount + put);
                    to.setItem(j, dst);
                } else {
                    ItemStack fresh = src;
                    fresh.set(put);
                    to.setItem(j, fresh);
                }
                to.setContainerChanged(j);
                left -= put;
            }
        }
        int done = want - left;
        if (done > 0) {
            from.removeItem(i, done);
            from.setContainerChanged(i);
            moved += done;
        }
        if (left > 0) break; // destination full
    }
    return moved;
}

void removeFromInventory(Player& p, ItemCounts const& take) {
    auto& inv = p.getInventory();
    for (auto const& [name, total] : take) {
        int need = total;
        for (int i = 0; i < inv.getContainerSize() && need > 0; ++i) {
            auto const& it = inv.getItem(i);
            if (it.isNull() || it.getTypeName() != name) continue;
            int n = std::min<int>(need, it.mCount);
            inv.removeItem(i, n);
            need -= n;
        }
    }
}

void giveToInventory(Player& p, std::string const& name, int aux, int count) {
    while (count > 0) {
        ItemStack st;
        st.reinit(name, 1, aux);
        int n = std::min<int>(count, st.getMaxStackSize());
        st.set(n);
        if (!p.add(st)) p.drop(st, false); // full: drop at the feet like a real player would
        count -= n;
    }
}

// ---- Stations ------------------------------------------------------------------------------------

std::optional<BlockPos> nearestBlock(Player& p, std::set<std::string> const& names, int radius) {
    auto&                   region = p.getDimensionBlockSource();
    auto                    f      = p.getFeetPos();
    BlockPos                c{(int)std::floor(f.x), (int)std::floor(f.y), (int)std::floor(f.z)};
    std::optional<BlockPos> best;
    int                     bestD = 1 << 30;
    for (int x = -radius; x <= radius; ++x)
        for (int y = -4; y <= 4; ++y)
            for (int z = -radius; z <= radius; ++z) {
                BlockPos q{c.x + x, c.y + y, c.z + z};
                int      d = x * x + y * y + z * z;
                if (d >= bestD || !region.getChunkAt(q)) continue;
                if (names.count(region.getBlock(q).getTypeName())) {
                    best  = q;
                    bestD = d;
                }
            }
    return best;
}

int hotbarSlotOf(Player const& p, std::string const& name) {
    auto& inv = p.getInventory();
    for (int i = 0; i < 9; ++i)
        if (!inv.getItem(i).isNull() && inv.getItem(i).getTypeName() == name) return i;
    return -1;
}

// A free spot next to the bot to put a station on: air with solid ground, not where the bot stands.
std::optional<BlockPos> spotNextTo(Player& p) {
    auto&    region = p.getDimensionBlockSource();
    auto     f      = p.getFeetPos();
    BlockPos feet{(int)std::floor(f.x), (int)std::floor(f.y + 0.5f), (int)std::floor(f.z)};
    for (int r = 1; r <= 2; ++r)
        for (int dx = -r; dx <= r; ++dx)
            for (int dz = -r; dz <= r; ++dz) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                BlockPos q{feet.x + dx, feet.y, feet.z + dz};
                if (isAir(region, q) && isSolid(region, {q.x, q.y - 1, q.z}) && withinReach(p, center(q))) return q;
            }
    return std::nullopt;
}

// Gets the bot within reach of a station block: finds one nearby, walks to it, or places one from
// the hotbar. Shared by crafting (crafting table) and smelting (furnace).
class StationAccess {
public:
    StationAccess(std::set<std::string> blocks, std::string item) : mBlocks(std::move(blocks)), mItem(std::move(item)) {}

    // Returns the station when within reach; otherwise a step to return (running or failed).
    std::variant<BlockPos, JobStep> step(SimulatedPlayer& sp) {
        if (mInner) {
            auto r = mInner->step(sp);
            if (!r.finished()) return JobStep::running();
            mInner.reset();
            if (r.state == JobState::Failed) return JobStep::fail(fmt::format("could not reach a {}: {}", shortName(mItem), r.reason));
        }
        auto pos = nearestBlock(sp, mBlocks, kStationSearch);
        if (pos && withinReach(sp, center(*pos))) return *pos;
        if (pos) {
            if (mApproached++ > 2) return JobStep::fail(fmt::format("{} out of reach", shortName(mItem)));
            mInner = makeApproach(*pos, kReach - 1.0f);
            return JobStep::running();
        }
        if (mPlaced++ > 0) return JobStep::fail(fmt::format("placing a {} failed", shortName(mItem)));
        int slot = hotbarSlotOf(sp, mItem);
        if (slot < 0)
            return JobStep::fail(fmt::format(
                "needs a {} within {} blocks, or one in the hotbar to place",
                shortName(mItem),
                kStationSearch
            ));
        auto spot = spotNextTo(sp);
        if (!spot) return JobStep::fail(fmt::format("no free spot to place a {}", shortName(mItem)));
        sp.setSelectedSlot(slot);
        mInner = makePlace(*spot);
        return JobStep::running();
    }

    void pause(SimulatedPlayer& sp) {
        if (mInner) mInner->pause(sp);
    }

private:
    std::set<std::string> mBlocks;
    std::string           mItem;
    std::unique_ptr<Job>  mInner;
    int                   mApproached{0};
    int                   mPlaced{0};
};

// ---- Recipes ---------------------------------------------------------------------------------------

struct CraftRecipe {
    std::string              id;
    std::string              result;
    int                      resultAux{0};
    int                      perCraft{1};
    bool                     needsTable{false};
    std::vector<Requirement> reqs;
};

std::vector<std::string> acceptedNames(RecipeIngredient const& ing) {
    std::vector<std::string> names;
    ing.forEachItemUntil([&](Item const& item, short) {
        auto const& n = item.mFullName->getString();
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        return false;
    });
    return names;
}

std::vector<CraftRecipe> recipesFor(std::string const& result) {
    std::vector<CraftRecipe> out;
    auto                     level = ll::service::getLevel();
    if (!level) return out;
    level->getRecipes().forEachRecipeFor(HashedString("crafting_table"), [&](Recipe const& r) {
        if (r.isMultiRecipe()) return;
        auto const& results = r.getResultItems();
        if (results.empty() || results[0].isNull() || results[0].getTypeName() != result) return;
        CraftRecipe c;
        c.id        = r.mRecipeId;
        c.result    = result;
        c.resultAux = results[0].getAuxValue();
        c.perCraft  = std::max<int>(1, results[0].mCount);
        int slots   = 0;
        for (auto const& ing : *r.mMyIngredients) {
            if (ing.isNull()) continue;
            ++slots;
            auto names = acceptedNames(ing);
            if (names.empty()) return; // an ingredient we cannot read: skip the recipe
            int  count = std::max<int>(1, ing.mStackSize);
            auto same  = std::find_if(c.reqs.begin(), c.reqs.end(), [&](Requirement const& q) { return q.accepts == names; });
            if (same != c.reqs.end()) same->count += count;
            else c.reqs.push_back({names, count});
        }
        c.needsTable = r.isShapeless() ? slots > 4 : (r.mWidth > 2 || r.mHeight > 2);
        out.push_back(std::move(c));
    });
    // Recipes that fit the 2x2 inventory grid first.
    std::stable_sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return !a.needsTable && b.needsTable; });
    return out;
}

std::string describeNeeds(CraftRecipe const& r) {
    std::string s;
    for (auto const& q : r.reqs) {
        if (!s.empty()) s += ", ";
        s += std::to_string(q.count) + " ";
        if (q.accepts.size() == 1) {
            s += shortName(q.accepts[0]);
            continue;
        }
        // "any planks" when every alternative shares a suffix such as _planks or _log.
        auto        first  = shortName(q.accepts[0]);
        auto        cut    = first.rfind('_');
        std::string suffix = cut == std::string::npos ? "" : first.substr(cut);
        bool        shared = !suffix.empty();
        for (auto const& a : q.accepts) shared = shared && a.ends_with(suffix);
        s += shared ? "any " + suffix.substr(1) : first + " (or similar)";
    }
    return s;
}

// ---- Jobs --------------------------------------------------------------------------------------------

class CraftJob final : public Job {
public:
    CraftJob(std::string item, int count) : mItem(fullName(std::move(item))), mCount(std::max(1, count)) {}

    std::string describe() const override { return fmt::format("craft {} {}", mCount, shortName(mItem)); }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        mTable.pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        if (++mTicks > 20 * 60) return JobStep::fail("timed out");
        auto recipes = recipesFor(mItem);
        if (recipes.empty()) return JobStep::fail(fmt::format("no crafting recipe makes {}", shortName(mItem)));

        auto                    inv = inventoryCounts(sp);
        CraftRecipe const*      pick = nullptr;
        std::optional<ItemCounts> take;
        int                     crafts = 0;
        for (auto const& r : recipes) {
            int want = craftsFor(mCount, r.perCraft);
            for (int n = want; n >= 1; --n) {
                if (auto t = allocate(r.reqs, inv, n)) {
                    pick   = &r;
                    take   = std::move(t);
                    crafts = n;
                    break;
                }
            }
            if (pick) break;
        }
        if (!pick) return JobStep::fail(fmt::format("missing ingredients; {} needs {}", shortName(mItem), describeNeeds(recipes[0])));

        if (pick->needsTable) {
            auto r = mTable.step(sp);
            if (auto* s = std::get_if<JobStep>(&r)) return *s;
        }
        removeFromInventory(sp, *take);
        giveToInventory(sp, pick->result, pick->resultAux, crafts * pick->perCraft);
        sp.sendInventory(false);
        int made = crafts * pick->perCraft;
        return made >= mCount ? JobStep::done(fmt::format("crafted {}", made))
                              : JobStep::done(fmt::format("crafted {} of {} (ran out of ingredients)", made, mCount));
    }

private:
    std::string   mItem;
    int           mCount;
    StationAccess mTable{{"minecraft:crafting_table"}, "minecraft:crafting_table"};
};

// Fuel value in items smelted per fuel item.
float fuelValue(std::string const& n) {
    if (n == "minecraft:coal" || n == "minecraft:charcoal") return 8;
    if (n == "minecraft:coal_block") return 80;
    if (n == "minecraft:blaze_rod") return 12;
    if (n.size() > 7 && (n.ends_with("_planks") || n.ends_with("_log") || n.ends_with("_wood") || n.ends_with("_stem")))
        return 1.5f;
    if (n == "minecraft:stick") return 0.5f;
    return 0;
}

class SmeltJob final : public Job {
public:
    SmeltJob(std::string item, int count) : mItem(fullName(std::move(item))), mCount(std::max(1, count)) {}

    std::string describe() const override { return fmt::format("smelt {} {}", mCount, shortName(mItem)); }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        mFurnace.pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        if (++mTicks > 20 * (30 + 12 * mCount)) return finish(sp, "timed out");
        if (!mLoaded) {
            if (countIn(sp.getInventory(), mItem, sp.getInventory().getContainerSize()) <= 0 && !mPos)
                return JobStep::fail(fmt::format("no {} in the inventory", shortName(mItem)));
            auto r = mFurnace.step(sp);
            if (auto* s = std::get_if<JobStep>(&r)) return *s;
            mPos = std::get<BlockPos>(r);
            return load(sp);
        }
        if (mTicks % 10 != 0) return JobStep::running();
        auto* c = container(sp);
        if (!c) return JobStep::fail("the furnace is gone");
        auto const& in  = c->getItem(0);
        auto const& out = c->getItem(2);
        int         got = out.isNull() ? 0 : out.mCount;
        if (got >= mLoadedCount || (in.isNull() && got > 0 && mTicks - mLastChange > 20 * 12)) return finish(sp, "");
        if (got != mLastOut) {
            mLastOut    = got;
            mLastChange = mTicks;
        }
        if (mTicks - mLastChange > 20 * 20) return finish(sp, "the furnace stopped (out of fuel?)");
        return JobStep::running();
    }

private:
    Container* container(SimulatedPlayer& sp) {
        if (!mPos) return nullptr;
        return containerAt(sp.getDimensionBlockSource(), *mPos);
    }

    JobStep load(SimulatedPlayer& sp) {
        auto* c = container(sp);
        if (!c) return JobStep::fail("could not open the furnace");
        if (!withinReach(sp, center(*mPos))) return JobStep::fail("furnace out of reach");
        auto const& in = c->getItem(0);
        if (!in.isNull() && in.getTypeName() != mItem) return JobStep::fail(fmt::format("the furnace is busy with {}", shortName(in.getTypeName())));
        auto const& out = c->getItem(2);
        if (!out.isNull()) return JobStep::fail(fmt::format("the furnace output holds {}; take it first", shortName(out.getTypeName())));

        auto& inv     = sp.getInventory();
        int   invSize = inv.getContainerSize();
        mLoadedCount  = moveItemsToSlot(inv, invSize, *c, 0, mItem, mCount);
        if (mLoadedCount <= 0) return JobStep::fail(fmt::format("no {} in the inventory", shortName(mItem)));

        // Fuel: enough for what was loaded, counting fuel already in the furnace.
        auto const& fuel     = c->getItem(1);
        float       have     = fuel.isNull() ? 0 : fuelValue(fuel.getTypeName()) * fuel.mCount;
        float       needMore = mLoadedCount - have;
        if (needMore > 0) {
            std::string best;
            for (int i = 0; i < invSize; ++i) {
                auto const& it = inv.getItem(i);
                if (it.isNull() || it.getTypeName() == mItem) continue;
                if (!fuel.isNull() && it.getTypeName() != fuel.getTypeName()) continue; // must stack with what is there
                if (fuelValue(it.getTypeName()) > fuelValue(best)) best = it.getTypeName();
            }
            if (best.empty() && have <= 0) {
                moveItemsToSlot(*c, 1, inv, -1, mItem, mLoadedCount); // give the input back
                sp.sendInventory(false);
                return JobStep::fail("no fuel in the inventory (coal, charcoal, logs, planks or sticks)");
            }
            if (!best.empty()) moveItemsToSlot(inv, invSize, *c, 1, best, (int)std::ceil(needMore / fuelValue(best)));
        }
        sp.sendInventory(false);
        mLoaded     = true;
        mLastChange = mTicks;
        logger().info("{} loaded the furnace at ({},{},{}) with {} {}", sp.getRealName(), mPos->x, mPos->y, mPos->z, mLoadedCount, shortName(mItem));
        return JobStep::running();
    }

    // Moves into one specific slot (to >= 0), or back into any inventory slot (to < 0, from slot 0).
    static int moveItemsToSlot(Container& from, int fromSlots, Container& to, int toSlot, std::string const& name, int count) {
        if (toSlot < 0) return moveItems(from, fromSlots, to, to.getContainerSize(), name, count);
        int        moved = 0;
        ItemStack  dst   = to.getItem(toSlot);
        for (int i = 0; i < fromSlots && moved < count; ++i) {
            ItemStack src = from.getItem(i);
            if (src.isNull() || src.getTypeName() != name) continue;
            int max  = src.getMaxStackSize();
            int room = dst.isNull() ? max : (dst.sameItem(src, {}) ? max - dst.mCount : 0);
            int put  = std::min({room, (int)src.mCount, count - moved});
            if (put <= 0) break;
            if (dst.isNull()) {
                dst = src;
                dst.set(put);
            } else {
                dst.set(dst.mCount + put);
            }
            from.removeItem(i, put);
            moved += put;
        }
        if (moved > 0) {
            to.setItem(toSlot, dst);
            to.setContainerChanged(toSlot);
        }
        return moved;
    }

    JobStep finish(SimulatedPlayer& sp, std::string const& problem) {
        auto* c = container(sp);
        int   taken = 0;
        if (c && withinReach(sp, center(*mPos))) {
            auto const& out = c->getItem(2);
            std::string name = out.isNull() ? "" : out.getTypeName();
            if (!name.empty()) taken = moveItems(*c, 3, sp.getInventory(), sp.getInventory().getContainerSize(), name, 64);
            // Only slot 2 should be taken; moveItems scanned slots 0-2 for that name, which is fine
            // unless input and output are the same item (they never are for vanilla recipes).
            sp.sendInventory(false);
        }
        if (taken == 0) return JobStep::fail(problem.empty() ? "nothing came out" : problem);
        if (!problem.empty()) return JobStep::done(fmt::format("took {} ({})", taken, problem));
        return JobStep::done(fmt::format("took {}", taken));
    }

    std::string             mItem;
    int                     mCount;
    StationAccess           mFurnace{{"minecraft:furnace", "minecraft:lit_furnace"}, "minecraft:furnace"};
    std::optional<BlockPos> mPos;
    bool                    mLoaded{false};
    int                     mLoadedCount{0};
    int                     mLastOut{0};
    int                     mLastChange{0};
};

class ChestJob final : public Job {
public:
    ChestJob(bool put, std::string item, int count, std::optional<BlockPos> pos)
    : mPut(put),
      mItem(item.empty() ? "" : fullName(std::move(item))),
      mCount(count <= 0 ? 1 << 20 : count),
      mPos(pos) {}

    std::string describe() const override {
        return fmt::format("{} {} {}", mPut ? "put" : "take", mCount >= (1 << 20) ? std::string("all") : std::to_string(mCount), mItem.empty() ? "items" : shortName(mItem));
    }

    void pause(SimulatedPlayer& sp) override {
        Job::pause(sp);
        if (mWalk) mWalk->pause(sp);
    }

    JobStep step(SimulatedPlayer& sp) override {
        if (++mTicks > 20 * 60) return JobStep::fail("timed out");
        if (mWalk) {
            auto r = mWalk->step(sp);
            if (!r.finished()) return JobStep::running();
            mWalk.reset();
            if (r.state == JobState::Failed) return JobStep::fail("could not reach the chest: " + r.reason);
        }
        if (!mPos) mPos = nearestBlock(sp, {"minecraft:chest", "minecraft:trapped_chest", "minecraft:barrel"}, kStationSearch);
        if (!mPos) return JobStep::fail(fmt::format("no chest within {} blocks", kStationSearch));
        if (!withinReach(sp, center(*mPos))) {
            if (mApproached++ > 2) return JobStep::fail("chest out of reach");
            mWalk = makeApproach(*mPos, kReach - 1.0f);
            return JobStep::running();
        }
        auto* c = containerAt(sp.getDimensionBlockSource(), *mPos);
        if (!c) return JobStep::fail("not a container");
        auto& inv   = sp.getInventory();
        int   moved = mPut ? moveItems(inv, inv.getContainerSize(), *c, c->getContainerSize(), mItem, mCount)
                           : moveItems(*c, c->getContainerSize(), inv, inv.getContainerSize(), mItem, mCount);
        sp.sendInventory(false);
        if (moved == 0) {
            return JobStep::fail(mPut ? (countIn(inv, mItem, inv.getContainerSize()) == 0 ? "nothing to put" : "the chest is full")
                                      : (countIn(*c, mItem, c->getContainerSize()) == 0 ? "the chest has none" : "the inventory is full"));
        }
        return JobStep::done(fmt::format("{} {} at ({},{},{})", mPut ? "put" : "took", moved, mPos->x, mPos->y, mPos->z));
    }

private:
    bool                    mPut;
    std::string             mItem;
    int                     mCount;
    std::optional<BlockPos> mPos;
    std::unique_ptr<Job>    mWalk;
    int                     mApproached{0};
};

} // namespace

std::unique_ptr<Job> makeCraft(std::string item, int count) { return std::make_unique<CraftJob>(std::move(item), count); }
std::unique_ptr<Job> makeSmelt(std::string item, int count) { return std::make_unique<SmeltJob>(std::move(item), count); }
std::unique_ptr<Job> makeChest(bool put, std::string item, int count, std::optional<BlockPos> pos) {
    return std::make_unique<ChestJob>(put, std::move(item), count, pos);
}

} // namespace karakuri
