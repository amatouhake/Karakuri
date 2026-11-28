#pragma once

// L0 reflex arbitration (docs/DESIGN.md 5.2). Pure: the mod fills BodyState from the game each
// tick and acts on the chosen reflex. Hysteresis keeps a reflex from flickering on and off.

namespace karakuri {

struct BodyState {
    float health{20};
    float hunger{20};
    bool  hasFood{false};
    bool  threatNear{false};      // a hostile mob within fighting range
    int   ticksSinceHurt{100000}; // ticks since the bot last took damage from an entity
    bool  armed{true};            // holds or can switch to a weapon better than the hand
};

enum class Reflex { None, Flee, Fight, Eat };

inline char const* toString(Reflex r) {
    switch (r) {
    case Reflex::None:
        return "none";
    case Reflex::Flee:
        return "flee";
    case Reflex::Fight:
        return "fight";
    case Reflex::Eat:
        return "eat";
    }
    return "?";
}

struct ReflexConfig {
    float eatStartHunger{6};    // start eating at or below this (a full bar is 20)
    float eatStopHunger{17};    // keep eating until at least this
    int   fightAfterHurt{100};  // fight back if hurt by an entity within this many ticks
    int   fightCalmTicks{40};   // stop fighting after no threat for this long
    float fleeHealth{6};        // run from a nearby threat at or below this health (20 = full)
    float fleeHealthUnarmed{14}; // ...or this, without a weapon
    float fleeStopHealth{12};   // ...until health is back to this, or
    int   fleeCalmTicks{100};   // ...no threat has been near for this long
};

class ReflexArbiter {
public:
    explicit ReflexArbiter(ReflexConfig cfg = {}) : mCfg(cfg) {}

    Reflex choose(BodyState const& s) {
        // Fleeing wins over everything: at low health a fight is likely lost.
        if (mCurrent == Reflex::Flee) {
            mCalm = s.threatNear ? 0 : mCalm + 1;
            if (s.health < mCfg.fleeStopHealth && mCalm < mCfg.fleeCalmTicks) return mCurrent;
            mCalm = 0;
            mCurrent = Reflex::None; // fall through: maybe fight or eat now
        } else if (s.threatNear && s.health <= (s.armed ? mCfg.fleeHealth : mCfg.fleeHealthUnarmed)) {
            mCalm = 0;
            return mCurrent = Reflex::Flee;
        }

        // Fighting wins over eating: eating takes 1.6 s with no defense.
        bool provoked = s.threatNear && s.ticksSinceHurt <= mCfg.fightAfterHurt;
        if (mCurrent == Reflex::Fight) {
            mCalm = s.threatNear ? 0 : mCalm + 1;
            if (mCalm < mCfg.fightCalmTicks) return mCurrent;
        } else if (provoked) {
            mCalm = 0;
            return mCurrent = Reflex::Fight;
        }

        if (mCurrent == Reflex::Eat) {
            if (s.hasFood && s.hunger < mCfg.eatStopHunger) return mCurrent;
        } else if (s.hasFood && s.hunger <= mCfg.eatStartHunger) {
            return mCurrent = Reflex::Eat;
        }
        return mCurrent = Reflex::None;
    }

    [[nodiscard]] Reflex current() const { return mCurrent; }
    void                 reset() {
        mCurrent = Reflex::None;
        mCalm    = 0;
    }

private:
    ReflexConfig mCfg;
    Reflex       mCurrent{Reflex::None};
    int          mCalm{0};
};

// Same-rules reach check (docs/DESIGN.md 8.1). The engine does not enforce reach for simulated
// players (seen in spike 0: it broke and placed blocks 10 blocks away), so skills must.
inline bool withinReach(float eyeX, float eyeY, float eyeZ, float tx, float ty, float tz, float reach) {
    float dx = tx - eyeX, dy = ty - eyeY, dz = tz - eyeZ;
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

} // namespace karakuri
