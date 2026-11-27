#pragma once

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

namespace karakuri {

class Mod {
public:
    static Mod& getInstance();

    Mod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

inline ll::io::Logger& logger() { return Mod::getInstance().getSelf().getLogger(); }

// Soft enum of bot names, used for command completion.
enum class BotName : int {};
void registerCommands();

}
namespace ll::command { class CommandHandle; }
namespace karakuri {
void registerDebugCommands(ll::command::CommandHandle& cmd);

} // namespace karakuri
