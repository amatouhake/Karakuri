#pragma once

// Ingredient allocation for crafting (docs/DESIGN.md 8.1: same rules, direct means). Pure: the mod
// turns a game recipe into Requirements (one per ingredient slot, each accepting a set of item
// names) and asks whether the inventory covers them. Greedy: the scarcest choices are taken first,
// which is enough for vanilla recipes (alternatives are things like "any planks").

#include "core/Inventory.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace karakuri {

struct Requirement {
    std::vector<std::string> accepts; // item names that satisfy this slot
    int                      count{1};
};

// Items to take from the inventory for `times` crafts, or nothing if the inventory falls short.
inline std::optional<ItemCounts> allocate(std::vector<Requirement> const& reqs, ItemCounts inv, int times = 1) {
    // Requirements with fewer alternatives first, so a flexible slot does not eat a scarce item.
    std::vector<Requirement const*> order;
    for (auto const& r : reqs) order.push_back(&r);
    std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->accepts.size() < b->accepts.size(); });

    ItemCounts take;
    for (auto const* r : order) {
        int need = r->count * times;
        // Prefer the alternative the inventory has most of.
        std::vector<std::string> alts = r->accepts;
        std::stable_sort(alts.begin(), alts.end(), [&](auto const& a, auto const& b) { return inv[a] > inv[b]; });
        for (auto const& name : alts) {
            int use = std::min(need, inv[name]);
            if (use <= 0) continue;
            inv[name] -= use;
            take[name] += use;
            need -= use;
            if (need == 0) break;
        }
        if (need > 0) return std::nullopt;
    }
    return take;
}

// Number of crafts needed for `wanted` items when one craft yields `perCraft`.
inline int craftsFor(int wanted, int perCraft) { return perCraft <= 0 ? 0 : (wanted + perCraft - 1) / perCraft; }

} // namespace karakuri
