#include "mod/Mcp.h"

#include "mod/BotManager.h"
#include "mod/Game.h"
#include "mod/Mod.h"
#include "mod/World.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <future>
#include <memory>
#include <thread>

namespace karakuri::mcp {

using json = nlohmann::json;

namespace {

constexpr char const* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

std::unique_ptr<httplib::Server>   gServer;
std::thread                        gThread;
std::shared_ptr<std::atomic<bool>> gAlive;

// ---------------------------------------------------------------------------------------------
// Running work on the server thread.

struct Failure {
    std::string message;
};

// Runs `fn` on the server thread and waits for its result. `fn` returns json, or throws Failure.
// `fn` must own what it uses (capture by value): it may run after this call has given up.
json onGame(std::function<json()> fn, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();
    auto alive   = gAlive;
    ll::thread::ServerThreadExecutor::getDefault().execute([promise, alive, fn = std::move(fn)] {
        if (!alive || !*alive) {
            promise->set_value(json{{"error", "server is stopping"}});
            return;
        }
        try {
            promise->set_value(fn());
        } catch (Failure const& f) {
            promise->set_value(json{{"error", f.message}});
        } catch (std::exception const& e) {
            promise->set_value(json{{"error", std::string("internal error: ") + e.what()}});
        }
    });
    // Poll so a shutdown (which runs on the server thread) is never blocked by a waiting request.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        if (!*alive) return json{{"error", "server is stopping"}};
        if (std::chrono::steady_clock::now() > deadline) return json{{"error", "the server thread did not answer in time"}};
    }
    return future.get();
}

void need(bool ok, std::string const& message) {
    if (!ok) throw Failure{message};
}

std::string str(json const& a, char const* key) {
    need(a.contains(key) && a[key].is_string(), std::string("missing string argument: ") + key);
    return a[key].get<std::string>();
}

int integer(json const& a, char const* key) {
    need(a.contains(key) && a[key].is_number(), std::string("missing number argument: ") + key);
    return (int)std::floor(a[key].get<double>());
}

int optInt(json const& a, char const* key, int def) { return a.contains(key) && a[key].is_number() ? (int)a[key].get<double>() : def; }

BlockPos blockArg(json const& a) { return {integer(a, "x"), integer(a, "y"), integer(a, "z")}; }

SimulatedPlayer& bot(std::string const& name) {
    auto* sp = BotManager::instance().player(name);
    need(sp != nullptr, "no bot named " + name + " in the world");
    return *sp;
}

double round1(double v) { return std::round(v * 10) / 10; }
double round2(double v) { return std::round(v * 100) / 100; }
json   posJson(Vec3 const& v) { return {{"x", round2(v.x)}, {"y", round2(v.y)}, {"z", round2(v.z)}}; }

// ---------------------------------------------------------------------------------------------
// Tools that run on the server thread.

json botStatus(std::string const& name) {
    auto i = BotManager::instance().info(name);
    need(i.has_value(), "no bot named " + name);
    json out{
        {"name", i->name},
        {"job", i->job.empty() ? json(nullptr) : json{{"id", i->jobId}, {"description", i->job}}},
        {"reflex", i->reflex},
        {"reflexes_enabled", i->reflexesEnabled},
        {"last_result", i->lastResult},
    };
    auto* sp = i->player;
    if (!sp) {
        out["in_world"] = false;
        return out;
    }
    out["in_world"]      = true;
    out["position"]      = posJson(sp->getFeetPos());
    out["dimension"]     = (int)sp->getDimensionId();
    out["health"]        = sp->getHealth();
    out["hunger"]        = round1(hunger(*sp));
    out["alive"]         = sp->isAlive();
    out["on_ground"]     = sp->isOnGround();
    out["selected_slot"] = sp->getSelectedItemSlot();
    out["is_night"]      = BotManager::instance().isNight();
    if (auto level = ll::service::getLevel()) out["time_of_day"] = level->getTime() % 24000;
    json inv             = json::array();
    auto& container      = sp->getInventory();
    for (int s = 0; s < container.getContainerSize(); ++s) {
        auto const& it = container.getItem(s);
        if (it.isNull()) continue;
        inv.push_back({{"slot", s}, {"item", it.getTypeName()}, {"count", (int)it.mCount}});
    }
    out["inventory"] = inv;
    return out;
}

json findBlocks(json const& a) {
    auto& sp     = bot(str(a, "bot"));
    auto  target = str(a, "block");
    if (target.find(':') == std::string::npos) target = "minecraft:" + target;
    int radius = std::clamp(optInt(a, "radius", 8), 1, 16);
    int limit  = std::clamp(optInt(a, "limit", 10), 1, 50);
    auto& region = sp.getDimensionBlockSource();
    auto  feet   = sp.getFeetPos();
    BlockPos c{(int)std::floor(feet.x), (int)std::floor(feet.y), (int)std::floor(feet.z)};
    struct Hit {
        BlockPos p;
        float    d;
    };
    std::vector<Hit> hits;
    for (int x = -radius; x <= radius; ++x)
        for (int y = -radius; y <= radius; ++y)
            for (int z = -radius; z <= radius; ++z) {
                BlockPos p{c.x + x, c.y + y, c.z + z};
                if (!region.getChunkAt(p)) continue;
                if (region.getBlock(p).getTypeName() != target) continue;
                hits.push_back({p, std::sqrt((float)(x * x + y * y + z * z))});
            }
    std::sort(hits.begin(), hits.end(), [](Hit const& l, Hit const& r) { return l.d < r.d; });
    json out = json::array();
    for (size_t k = 0; k < hits.size() && (int)k < limit; ++k)
        out.push_back({{"x", hits[k].p.x}, {"y", hits[k].p.y}, {"z", hits[k].p.z}, {"distance", round1(hits[k].d)}});
    return {{"block", target}, {"radius", radius}, {"found", hits.size()}, {"nearest", out}};
}

json nearbyEntities(json const& a) {
    auto& sp     = bot(str(a, "bot"));
    float radius = (float)std::clamp(optInt(a, "radius", 16), 1, 48);
    auto  center = sp.getPosition();
    AABB  box{center - Vec3{radius, radius, radius}, center + Vec3{radius, radius, radius}};
    json  out = json::array();
    for (auto* e : sp.getDimensionBlockSource().getEntities(box, 0.0f)) {
        if (!e || e == &sp || !e->isAlive()) continue;
        float d = e->getPosition().distanceTo(center);
        if (d > radius) continue;
        json j{
            {"type", e->getTypeName()},
            {"position", posJson(e->getFeetPos())},
            {"distance", round1(d)},
            {"hostile", e->hasCategory(ActorCategory::Monster)},
            {"health", e->getHealth()},
        };
        if (e->isPlayer()) j["name"] = static_cast<Player*>(e)->getRealName();
        out.push_back(std::move(j));
    }
    std::sort(out.begin(), out.end(), [](json const& l, json const& r) { return l["distance"] < r["distance"]; });
    return {{"entities", out}};
}

json getBlock(json const& a) {
    auto& sp     = bot(str(a, "bot"));
    auto  p      = blockArg(a);
    auto& region = sp.getDimensionBlockSource();
    if (!region.getChunkAt(p)) return {{"loaded", false}};
    auto const& b = region.getBlock(p);
    static constexpr char const* kinds[] = {"unknown", "air", "passable", "solid", "partial", "obstacle", "water", "danger"};
    return {{"loaded", true}, {"block", b.getTypeName()}, {"kind", kinds[(int)classify(b)]}};
}

// ---------------------------------------------------------------------------------------------
// Tool table.

struct Tool {
    char const*                       name;
    char const*                       description;
    json                              schema;
    std::function<json(json const&)> run; // runs on the HTTP thread
};

json obj(json props, std::vector<std::string> required) {
    return {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}

json const kBot  = {{"type", "string"}, {"description", "Bot name"}};
json const kInt  = {{"type", "integer"}};
json const kWait = {{"type", "number"},
                    {"description",
                     "Seconds to wait for the job to finish before returning (0 = return at once, max 45; longer jobs "
                     "keep running: follow them with wait_for_event and the returned event_cursor)"}};

json eventJson(Event const& e) {
    json j{{"id", e.id}, {"tick", e.tick}, {"bot", e.bot}, {"kind", e.kind}, {"detail", e.detail}};
    if (e.jobId) j["job_id"] = e.jobId;
    if (!e.state.empty()) j["state"] = e.state;
    if (!e.inventoryChange.empty()) {
        json c = json::object();
        for (auto const& [name, d] : e.inventoryChange) c[name] = d;
        j["inventory_change"] = c;
    }
    return j;
}

// Starts a job on the server thread; optionally waits for its job_finished event.
json startJob(json const& a, std::function<std::unique_ptr<Job>(json const&)> make) {
    auto name   = a.value("bot", "");
    auto start  = onGame([=]() -> json {
        need(!name.empty(), "missing string argument: bot");
        bot(name);
        uint64_t id  = 0;
        auto     err = BotManager::instance().assign(name, make(a), &id);
        need(err.empty(), err);
        return {{"job_id", id}};
    });
    if (start.contains("error")) return start;
    uint64_t id   = start["job_id"];
    // MCP clients time requests out (often around 60 s), so never hold one longer than this.
    double   wait = std::clamp(a.value("wait_seconds", 0.0), 0.0, 45.0);
    json     out{{"bot", name}, {"job_id", id}, {"state", "running"}};
    if (wait > 0) {
        auto evs = BotManager::instance().events().wait(
            0,
            std::chrono::milliseconds((int)(wait * 1000)),
            [id](Event const& e) { return e.kind == "job_finished" && e.jobId == id; },
            1
        );
        if (!evs.empty()) {
            auto ev       = eventJson(evs[0]);
            out["state"]  = evs[0].state;
            out["detail"] = evs[0].detail;
            out["inventory_change"] = ev.value("inventory_change", json::object());
        }
    }
    out["event_cursor"] = BotManager::instance().events().lastId();
    return out;
}

std::unique_ptr<Job> chestJob(json const& a, bool put) {
    std::optional<BlockPos> pos;
    if (a.contains("x") && a.contains("y") && a.contains("z")) pos = blockArg(a);
    return makeChest(put, a.value("item", ""), optInt(a, "count", 0), pos);
}

std::vector<Tool> const& tools() {
    static std::vector<Tool> const t = {
        {"list_bots", "List the bots by name.", obj(json::object(), {}), [](json const&) {
             return onGame([] { return json{{"bots", BotManager::instance().names()}}; });
         }},
        {"spawn_bot",
         "Spawn a survival bot at a position (overworld). Bots respawn on their own after dying.",
         obj({{"name", kBot}, {"x", kInt}, {"y", kInt}, {"z", kInt}}, {"name", "x", "y", "z"}),
         [](json const& a) {
             return onGame([=] {
                 auto p   = blockArg(a);
                 auto err = BotManager::instance().spawn(str(a, "name"), Vec3{p.x + 0.5f, (float)p.y, p.z + 0.5f}, 0);
                 need(err.empty(), err);
                 return botStatus(str(a, "name"));
             });
         }},
        {"remove_bot", "Remove a bot from the world.", obj({{"bot", kBot}}, {"bot"}), [](json const& a) {
             return onGame([=] {
                 auto err = BotManager::instance().remove(str(a, "bot"));
                 need(err.empty(), err);
                 return json{{"removed", str(a, "bot")}};
             });
         }},
        {"bot_status",
         "A bot's position, health, hunger (0-20), current job, last job result, reflex state and inventory.",
         obj({{"bot", kBot}}, {"bot"}),
         [](json const& a) { return onGame([=] { return botStatus(str(a, "bot")); }); }},
        {"goto",
         "Move a bot so its feet are in block (x,y,z). It plans its own path: walks, climbs, drops up to 3 blocks, digs "
         "through, bridges and pillars with plain blocks (dirt, cobblestone, planks...) from its hotbar. Replaces the "
         "current job.",
         obj({{"bot", kBot}, {"x", kInt}, {"y", kInt}, {"z", kInt}, {"wait_seconds", kWait}}, {"bot", "x", "y", "z"}),
         [](json const& a) { return startJob(a, [](json const& a) { return makePathGoto(blockArg(a)); }); }},
        {"mine",
         "Break block (x,y,z), walking within reach first, with the best tool in the hotbar. The drop stays on the "
         "ground unless the bot stands on it: call collect_items afterwards. Replaces the current job.",
         obj({{"bot", kBot}, {"x", kInt}, {"y", kInt}, {"z", kInt}, {"wait_seconds", kWait}}, {"bot", "x", "y", "z"}),
         [](json const& a) { return startJob(a, [](json const& a) { return makeMine(blockArg(a)); }); }},
        {"place",
         "Place the held block at (x,y,z), walking within reach first. The spot must be empty with a solid "
         "neighbor. Replaces the current job.",
         obj({{"bot", kBot}, {"x", kInt}, {"y", kInt}, {"z", kInt}, {"wait_seconds", kWait}}, {"bot", "x", "y", "z"}),
         [](json const& a) { return startJob(a, [](json const& a) { return makePlace(blockArg(a)); }); }},
        {"hold", "Select hotbar slot 0-8.", obj({{"bot", kBot}, {"slot", kInt}}, {"bot", "slot"}), [](json const& a) {
             return onGame([=] {
                 auto err = BotManager::instance().hold(str(a, "bot"), integer(a, "slot"));
                 need(err.empty(), err);
                 return botStatus(str(a, "bot"));
             });
         }},
        {"eat", "Eat food from the hotbar (only works when not full).", obj({{"bot", kBot}, {"wait_seconds", kWait}}, {"bot"}),
         [](json const& a) { return startJob(a, [](json const&) { return makeEat(); }); }},
        {"collect_items",
         "Walk over dropped items near the bot (radius max 16) to pick them up. Mining leaves drops on the ground; "
         "the result's inventory_change says what was picked up.",
         obj({{"bot", kBot}, {"radius", kInt}, {"wait_seconds", kWait}}, {"bot"}),
         [](json const& a) {
             return startJob(a, [](json const& a) { return makeCollect((float)std::clamp(optInt(a, "radius", 8), 1, 16)); });
         }},
        {"craft",
         "Craft items by name (e.g. oak_planks, stick, crafting_table, wooden_pickaxe, furnace, iron_pickaxe) from "
         "the inventory, using the game's recipes. 3x3 recipes need a crafting table within reach: the bot walks to one "
         "within 16 blocks or places one from its hotbar. inventory_change in the result shows what was made and used.",
         obj({{"bot", kBot}, {"item", {{"type", "string"}}}, {"count", kInt}, {"wait_seconds", kWait}}, {"bot", "item"}),
         [](json const& a) {
             return startJob(a, [](json const& a) { return makeCraft(str(a, "item"), optInt(a, "count", 1)); });
         }},
        {"smelt",
         "Smelt items (e.g. raw_iron, oak_log, sand) in a furnace within 16 blocks, or one placed from the hotbar. "
         "Fuel (coal, charcoal, logs, planks, sticks) comes from the inventory. Takes about 10 seconds per item; the "
         "result is taken back into the inventory.",
         obj({{"bot", kBot}, {"item", {{"type", "string"}}}, {"count", kInt}, {"wait_seconds", kWait}}, {"bot", "item"}),
         [](json const& a) {
             return startJob(a, [](json const& a) { return makeSmelt(str(a, "item"), optInt(a, "count", 1)); });
         }},
        {"chest_put",
         "Put items from the inventory into a chest: the nearest one within 16 blocks, or the one at x,y,z. Omit item "
         "for any item, omit count for all.",
         obj({{"bot", kBot}, {"item", {{"type", "string"}}}, {"count", kInt}, {"x", kInt}, {"y", kInt}, {"z", kInt},
              {"wait_seconds", kWait}},
             {"bot"}),
         [](json const& a) { return startJob(a, [](json const& a) { return chestJob(a, true); }); }},
        {"chest_take",
         "Take items from a chest into the inventory: the nearest one within 16 blocks, or the one at x,y,z. Omit item "
         "for any item, omit count for all.",
         obj({{"bot", kBot}, {"item", {{"type", "string"}}}, {"count", kInt}, {"x", kInt}, {"y", kInt}, {"z", kInt},
              {"wait_seconds", kWait}},
             {"bot"}),
         [](json const& a) { return startJob(a, [](json const& a) { return chestJob(a, false); }); }},
        {"sleep",
         "Sleep in a bed within 16 blocks (or place one from the inventory) until morning. Only works at night; "
         "sleeping skips the night. Monsters nearby prevent sleeping.",
         obj({{"bot", kBot}, {"wait_seconds", kWait}}, {"bot"}),
         [](json const& a) { return startJob(a, [](json const&) { return makeSleep(); }); }},
        {"shelter",
         "Wall the bot in with scaffold blocks from the inventory (sides at feet and head height, a corner and a "
         "roof; up to 10 blocks). Finishes when closed (the bot stays inside; goto digs back out), or fails naming "
         "the gaps it could not close.",
         obj({{"bot", kBot}, {"wait_seconds", kWait}}, {"bot"}),
         [](json const& a) { return startJob(a, [](json const&) { return makeShelter(); }); }},
        {"attack", "Attack the nearest monster within 16 blocks, with the best weapon in the hotbar.", obj({{"bot", kBot}, {"wait_seconds", kWait}}, {"bot"}),
         [](json const& a) { return startJob(a, [](json const&) { return makeAttack(std::nullopt); }); }},
        {"stop", "Cancel the current job.", obj({{"bot", kBot}}, {"bot"}), [](json const& a) {
             return onGame([=] {
                 auto err = BotManager::instance().stop(str(a, "bot"));
                 need(err.empty(), err);
                 return botStatus(str(a, "bot"));
             });
         }},
        {"set_reflexes",
         "Turn the automatic reflexes on or off (fight back when hurt with a monster near; eat when hunger <= 6). "
         "Reflexes pause the current job and resume it afterwards.",
         obj({{"bot", kBot}, {"enabled", {{"type", "boolean"}}}}, {"bot", "enabled"}),
         [](json const& a) {
             return onGame([=] {
                 need(a.contains("enabled") && a["enabled"].is_boolean(), "missing boolean argument: enabled");
                 auto err = BotManager::instance().setReflexes(str(a, "bot"), a["enabled"].get<bool>());
                 need(err.empty(), err);
                 return botStatus(str(a, "bot"));
             });
         }},
        {"find_blocks",
         "Find the nearest blocks of a type (e.g. oak_log, stone, iron_ore) around a bot, within a cube of the given "
         "radius (max 16).",
         obj({{"bot", kBot}, {"block", {{"type", "string"}}}, {"radius", kInt}, {"limit", kInt}}, {"bot", "block"}),
         [](json const& a) { return onGame([=] { return findBlocks(a); }); }},
        {"nearby_entities", "Mobs, players and items near a bot (radius max 48), nearest first.",
         obj({{"bot", kBot}, {"radius", kInt}}, {"bot"}),
         [](json const& a) { return onGame([=] { return nearbyEntities(a); }); }},
        {"get_block", "The block at (x,y,z) in the bot's dimension and how the pathfinder sees it.",
         obj({{"bot", kBot}, {"x", kInt}, {"y", kInt}, {"z", kInt}}, {"bot", "x", "y", "z"}),
         [](json const& a) { return onGame([=] { return getBlock(a); }); }},
        {"wait_for_event",
         "Wait for bot events newer than `after` (an event id or event_cursor; 0 = everything kept). Kinds: "
         "job_started, job_finished (state done/failed/cancelled, with inventory_change: items gained or used), "
         "reflex (none/flee/fight/eat), hurt, died, respawned, spawned, removed, and the world events night and day. "
         "Returns as soon as something matches, or after timeout_seconds (max 45) with an empty list.",
         obj({{"after", kInt}, {"bot", kBot}, {"kinds", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"timeout_seconds", {{"type", "number"}}}},
             {}),
         [](json const& a) {
             uint64_t                 after = a.contains("after") && a["after"].is_number() ? a["after"].get<uint64_t>() : 0;
             std::string              botName = a.value("bot", "");
             std::vector<std::string> kinds;
             if (a.contains("kinds") && a["kinds"].is_array())
                 for (auto const& k : a["kinds"])
                     if (k.is_string()) kinds.push_back(k.get<std::string>());
             double timeout = std::clamp(a.value("timeout_seconds", 20.0), 0.0, 45.0);
             auto   evs     = BotManager::instance().events().wait(
                 after,
                 std::chrono::milliseconds((int)(timeout * 1000)),
                 [&](Event const& e) {
                     if (!botName.empty() && !e.bot.empty() && e.bot != botName) return false; // world events match any bot
                     return kinds.empty() || std::find(kinds.begin(), kinds.end(), e.kind) != kinds.end();
                 },
                 50
             );
             json list = json::array();
             for (auto const& e : evs) list.push_back(eventJson(e));
             uint64_t cursor = evs.empty() ? BotManager::instance().events().lastId() : evs.back().id;
             return json{{"events", list}, {"cursor", cursor}};
         }},
    };
    return t;
}

// ---------------------------------------------------------------------------------------------
// JSON-RPC.

json rpcError(json const& id, int code, std::string const& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

json rpcResult(json const& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

// Returns null for notifications.
json handle(json const& msg) {
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method"))
        return rpcError(msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr), -32600, "invalid request");
    auto method = msg["method"].get<std::string>();
    if (!msg.contains("id")) return nullptr; // notification (e.g. notifications/initialized)
    auto const& id     = msg["id"];
    auto        params = msg.value("params", json::object());

    if (method == "initialize") {
        std::string asked   = params.value("protocolVersion", "");
        std::string version = kProtocolVersions[0];
        for (auto const* v : kProtocolVersions)
            if (asked == v) version = v;
        return rpcResult(id, {
            {"protocolVersion", version},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "karakuri"}, {"version", "0.1.0"}}},
            {"instructions",
             "Karakuri drives survival bots on a Minecraft Bedrock server. Jobs (goto, mine, place, craft, smelt, chest_put, "
             "chest_take, collect_items, eat, attack) run "
             "in the background: pass wait_seconds to wait for the result, or call wait_for_event with the returned "
             "event_cursor. Use bot_status, find_blocks, nearby_entities and get_block to look around. Coordinates "
             "are block coordinates; y is up. Bots fight back when hurt, flee at low health and eat when hungry on "
             "their own (reflexes). Nights bring monsters: sleep in a bed or take shelter."},
        });
    }
    if (method == "ping") return rpcResult(id, json::object());
    if (method == "tools/list") {
        json list = json::array();
        for (auto const& t : tools()) list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
        return rpcResult(id, {{"tools", list}});
    }
    if (method == "tools/call") {
        auto name = params.value("name", "");
        auto args = params.value("arguments", json::object());
        for (auto const& t : tools()) {
            if (name != t.name) continue;
            json r;
            try {
                r = t.run(args);
            } catch (Failure const& f) {
                r = json{{"error", f.message}};
            }
            bool isError = r.is_object() && r.contains("error");
            return rpcResult(id, {{"content", json::array({{{"type", "text"}, {"text", r.dump()}}})}, {"isError", isError}});
        }
        return rpcError(id, -32602, "unknown tool: " + name);
    }
    return rpcError(id, -32601, "method not found: " + method);
}

bool localOrigin(httplib::Request const& req) {
    if (!req.has_header("Origin")) return true;
    auto o = req.get_header_value("Origin");
    return o.rfind("http://localhost", 0) == 0 || o.rfind("http://127.0.0.1", 0) == 0 || o.rfind("http://[::1]", 0) == 0;
}

} // namespace

Config loadConfig(std::filesystem::path const& dir) {
    Config cfg;
    auto   file = dir / "config.json";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (std::ifstream in(file); in) {
        auto j = json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("mcp") && j["mcp"].is_object()) {
            auto const& m = j["mcp"];
            cfg.enabled   = m.value("enabled", cfg.enabled);
            cfg.host      = m.value("host", cfg.host);
            cfg.port      = m.value("port", cfg.port);
        }
    } else if (std::ofstream outFile(file); outFile) {
        outFile << json{{"mcp", {{"enabled", cfg.enabled}, {"host", cfg.host}, {"port", cfg.port}}}}.dump(4) << "\n";
    }
    return cfg;
}

bool start(Config const& cfg) {
    if (!cfg.enabled || gServer) return false;
    gAlive  = std::make_shared<std::atomic<bool>>(true);
    BotManager::instance().events().reopen();
    gServer = std::make_unique<httplib::Server>();
    gServer->Post("/mcp", [](httplib::Request const& req, httplib::Response& res) {
        if (!localOrigin(req)) {
            res.status = 403;
            return;
        }
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.set_content(rpcError(nullptr, -32700, "parse error").dump(), "application/json");
            return;
        }
        json reply;
        if (body.is_array()) {
            reply = json::array();
            for (auto const& m : body)
                if (auto r = handle(m); !r.is_null()) reply.push_back(std::move(r));
            if (reply.empty()) reply = nullptr;
        } else {
            reply = handle(body);
        }
        if (reply.is_null()) {
            res.status = 202;
            return;
        }
        res.set_content(reply.dump(), "application/json");
    });
    gServer->Get("/mcp", [](httplib::Request const&, httplib::Response& res) { res.status = 405; });
    if (!gServer->bind_to_port(cfg.host, cfg.port)) {
        logger().error("MCP server could not listen on {}:{}", cfg.host, cfg.port);
        gServer.reset();
        return false;
    }
    gThread = std::thread([] { gServer->listen_after_bind(); });
    logger().info("MCP server listening on http://{}:{}/mcp", cfg.host, cfg.port);
    return true;
}

void stop() {
    if (!gServer) return;
    *gAlive = false;
    BotManager::instance().events().shutdown(); // wake waiting requests
    gServer->stop();
    if (gThread.joinable()) gThread.join();
    gServer.reset();
    logger().info("MCP server stopped");
}

} // namespace karakuri::mcp
