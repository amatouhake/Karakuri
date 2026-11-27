#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep

#include "mod/Mod.h"

#include "mod/BotManager.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/entity/ActorHurtEvent.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/mod/RegisterHelper.h"

#include "mc/server/SimulatedPlayer.h"
#include "mc/world/actor/player/Player.h"

#include <vector>

namespace karakuri {

namespace {
std::vector<ll::event::ListenerPtr> gListeners;
} // namespace

Mod& Mod::getInstance() {
    static Mod m;
    return m;
}

bool Mod::load() { return true; }

bool Mod::enable() {
    // LeviLamina drops a mod's command overloads when it is disabled, so register on every enable.
    registerCommands();
    auto& bus   = ll::event::EventBus::getInstance();
    auto& bots  = BotManager::instance();
    gListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([&bots](auto& ev) {
        bots.tick(ev.level());
    }));
    gListeners.push_back(bus.emplaceListener<ll::event::ActorHurtEvent>([&bots](ll::event::ActorHurtEvent& ev) {
        auto& a = ev.self();
        if (a.isPlayer() && static_cast<Player&>(a).isSimulated() && ev.source().isEntitySource())
            bots.onHurt(static_cast<SimulatedPlayer&>(a));
    }));
    gListeners.push_back(bus.emplaceListener<ll::event::PlayerDieEvent>([&bots](ll::event::PlayerDieEvent& ev) {
        if (ev.self().isSimulated()) bots.onDeath(static_cast<SimulatedPlayer&>(ev.self()));
    }));
    return true;
}

bool Mod::disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& l : gListeners) bus.removeListener(l);
    gListeners.clear();
    BotManager::instance().removeAll();
    return true;
}

} // namespace karakuri

LL_REGISTER_MOD(karakuri::Mod, karakuri::Mod::getInstance());
