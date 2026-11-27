#pragma once

// Inventory change between two snapshots (item name -> total count). Pure, so it is unit tested.
// Job results report it so an agent sees what a job picked up or used without asking again.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace karakuri {

using ItemCounts = std::map<std::string, int>;
using ItemChange = std::vector<std::pair<std::string, int>>; // name, delta (never 0), sorted by name

inline ItemChange diffCounts(ItemCounts const& before, ItemCounts const& after) {
    ItemChange out;
    auto       b = before.begin();
    auto       a = after.begin();
    while (b != before.end() || a != after.end()) {
        if (a == after.end() || (b != before.end() && b->first < a->first)) {
            out.emplace_back(b->first, -b->second);
            ++b;
        } else if (b == before.end() || a->first < b->first) {
            out.emplace_back(a->first, a->second);
            ++a;
        } else {
            if (int d = a->second - b->second; d != 0) out.emplace_back(a->first, d);
            ++a;
            ++b;
        }
    }
    return out;
}

// "+2 oak_log, -1 dirt" (the minecraft: namespace is dropped).
inline std::string formatChange(ItemChange const& change) {
    std::string out;
    for (auto const& [name, d] : change) {
        std::string_view n = name;
        if (n.rfind("minecraft:", 0) == 0) n.remove_prefix(10);
        if (!out.empty()) out += ", ";
        out += (d > 0 ? "+" : "") + std::to_string(d) + " " + std::string(n);
    }
    return out;
}

} // namespace karakuri
