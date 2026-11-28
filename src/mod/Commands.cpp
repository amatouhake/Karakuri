// `/karakuri` (alias `/kr`): typed subcommands with bot-name completion. Jobs run asynchronously;
// their results go to the server log and to `/kr status`.

#include "mod/BotManager.h"
#include "mod/Mod.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/EnumName.h"
#include "ll/api/command/Optional.h"
#include "ll/api/command/SoftEnum.h"

#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandPositionFloat.h"
#include "mc/world/actor/Actor.h"

#include <cmath>

namespace karakuri {

using ll::command::Optional;
using ll::command::SoftEnum;

// Parameter structs need external linkage: the command layer reflects over them with pfr.
namespace cmd {

struct NewBot {
    std::string                    name;
    Optional<CommandPositionFloat> pos;
};
struct OneBot {
    SoftEnum<BotName> bot;
};
struct BotAt {
    SoftEnum<BotName>    bot;
    CommandPositionFloat pos;
};
enum class Toggle : int { on, off };
struct BotToggle {
    SoftEnum<BotName> bot;
    Toggle            state;
};
struct BotItem {
    SoftEnum<BotName> bot;
    std::string       item;
    Optional<int>     count;
};
enum class ChestOp : int { put, take };
struct BotChest {
    SoftEnum<BotName> bot;
    ChestOp           op;
    std::string       item; // "all" for any item
    Optional<int>     count;
};
struct BotSlot {
    SoftEnum<BotName> bot;
    int               slot;
};
} // namespace cmd

namespace {
using namespace cmd;

void reply(CommandOutput& out, std::string const& err, std::string const& ok) {
    if (err.empty()) out.success(ok);
    else out.error(err);
}

BlockPos toBlock(Vec3 const& v) {
    return {(int)std::floor(v.x), (int)std::floor(v.y), (int)std::floor(v.z)};
}

} // namespace

void registerCommands() {
    auto& reg = ll::command::CommandRegistrar::getServerInstance();
    reg.tryRegisterSoftEnum(ll::command::enum_name_v<BotName>, {});
    auto& cmd = reg.getOrCreateCommand("karakuri", "Karakuri bots", CommandPermissionLevel::GameDirectors).alias("kr");
    auto& bots = BotManager::instance();

    cmd.overload<NewBot>().text("spawn").required("name").optional("pos").execute(
        [&bots](CommandOrigin const& origin, CommandOutput& out, NewBot const& p, Command const& c) {
            Vec3 pos;
            if (p.pos.has_value()) {
                pos = origin.getExecutePosition(c.mVersion, *p.pos);
            } else if (auto* e = origin.getEntity()) {
                pos = e->getFeetPos();
            } else {
                return out.error("give a position when running from the console");
            }
            int dim = 0;
            if (auto* e = origin.getEntity()) dim = (int)e->getDimensionId();
            reply(out, bots.spawn(p.name, pos, dim), "spawned " + p.name);
        }
    );
    cmd.overload<OneBot>().text("remove").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.remove(p.bot), "removed " + std::string(p.bot));
        }
    );
    cmd.overload().text("list").execute([&bots](CommandOrigin const&, CommandOutput& out) {
        auto names = bots.names();
        if (names.empty()) return out.success("no bots");
        std::string all;
        for (auto const& n : names) all += (all.empty() ? "" : ", ") + n;
        out.success(fmt::format("{} bot(s): {}", names.size(), all));
    });
    cmd.overload<OneBot>().text("status").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            out.success(bots.status(p.bot));
        }
    );
    cmd.overload<OneBot>().text("stop").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.stop(p.bot), "stopped " + std::string(p.bot));
        }
    );
    cmd.overload<BotToggle>().text("reflexes").required("bot").required("state").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BotToggle const& p) {
            bool on = p.state == Toggle::on;
            reply(out, bots.setReflexes(p.bot, on), fmt::format("reflexes {} for {}", on ? "on" : "off", p.bot));
        }
    );
    cmd.overload<BotAt>().text("goto").required("bot").required("pos").execute(
        [&bots](CommandOrigin const& origin, CommandOutput& out, BotAt const& p, Command const& c) {
            auto pos = toBlock(origin.getExecutePosition(c.mVersion, p.pos));
            reply(out, bots.assign(p.bot, makePathGoto(pos)), "goto started");
        }
    );
    cmd.overload<BotAt>().text("mine").required("bot").required("pos").execute(
        [&bots](CommandOrigin const& origin, CommandOutput& out, BotAt const& p, Command const& c) {
            auto pos = toBlock(origin.getExecutePosition(c.mVersion, p.pos));
            reply(out, bots.assign(p.bot, makeMine(pos)), "mine started");
        }
    );
    cmd.overload<BotAt>().text("place").required("bot").required("pos").execute(
        [&bots](CommandOrigin const& origin, CommandOutput& out, BotAt const& p, Command const& c) {
            auto pos = toBlock(origin.getExecutePosition(c.mVersion, p.pos));
            reply(out, bots.assign(p.bot, makePlace(pos)), "place started");
        }
    );
    cmd.overload<BotSlot>().text("hold").required("bot").required("slot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BotSlot const& p) {
            reply(out, bots.hold(p.bot, p.slot), fmt::format("{} holds slot {}", p.bot, p.slot));
        }
    );
    cmd.overload<OneBot>().text("eat").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.assign(p.bot, makeEat()), "eat started");
        }
    );
    cmd.overload<BotItem>().text("craft").required("bot").required("item").optional("count").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BotItem const& p) {
            reply(out, bots.assign(p.bot, makeCraft(p.item, p.count.has_value() ? *p.count : 1)), "craft started");
        }
    );
    cmd.overload<BotItem>().text("smelt").required("bot").required("item").optional("count").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BotItem const& p) {
            reply(out, bots.assign(p.bot, makeSmelt(p.item, p.count.has_value() ? *p.count : 1)), "smelt started");
        }
    );
    cmd.overload<BotChest>().text("chest").required("bot").required("op").required("item").optional("count").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, BotChest const& p) {
            auto job = makeChest(
                p.op == ChestOp::put,
                p.item == "all" ? "" : p.item,
                p.count.has_value() ? *p.count : 0,
                std::nullopt
            );
            reply(out, bots.assign(p.bot, std::move(job)), "chest job started");
        }
    );
    cmd.overload<OneBot>().text("sleep").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.assign(p.bot, makeSleep()), "sleep started");
        }
    );
    cmd.overload<OneBot>().text("shelter").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.assign(p.bot, makeShelter()), "shelter started");
        }
    );
    cmd.overload<OneBot>().text("collect").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.assign(p.bot, makeCollect()), "collect started");
        }
    );
    cmd.overload<OneBot>().text("attack").required("bot").execute(
        [&bots](CommandOrigin const&, CommandOutput& out, OneBot const& p) {
            reply(out, bots.assign(p.bot, makeAttack(std::nullopt)), "attack started");
        }
    );
    registerDebugCommands(cmd);
}

} // namespace karakuri
