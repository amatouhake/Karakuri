#pragma once

// Pure job bookkeeping shared by every skill. No game types here, so it is unit tested.

#include <string>
#include <utility>

namespace karakuri {

enum class JobState { Running, Done, Failed };

struct JobStep {
    JobState    state{JobState::Running};
    std::string reason;

    static JobStep running() { return {}; }
    static JobStep done(std::string why = {}) { return {JobState::Done, std::move(why)}; }
    static JobStep fail(std::string why) { return {JobState::Failed, std::move(why)}; }

    [[nodiscard]] bool finished() const { return state != JobState::Running; }
};

inline char const* toString(JobState s) {
    switch (s) {
    case JobState::Running:
        return "running";
    case JobState::Done:
        return "done";
    case JobState::Failed:
        return "failed";
    }
    return "?";
}

// Declares a job stuck when the distance to its goal has not shrunk by `minGain` within `window`
// ticks. The engine's navigation can stop short without reporting it, so every movement job
// watches its own progress.
class ProgressWatch {
public:
    ProgressWatch(int window = 60, double minGain = 0.5) : mWindow(window), mMinGain(minGain) {}

    void reset(double distance) {
        mBest  = distance;
        mTicks = 0;
    }

    // Returns true once the job should give up.
    bool update(double distance) {
        if (distance < mBest - mMinGain) {
            mBest  = distance;
            mTicks = 0;
            return false;
        }
        return ++mTicks >= mWindow;
    }

private:
    int    mWindow;
    double mMinGain;
    double mBest{1e18};
    int    mTicks{0};
};

// Counts down an action cooldown in ticks (attacks and item use are limited by the engine).
class Cooldown {
public:
    explicit Cooldown(int ticks) : mTicks(ticks) {}
    [[nodiscard]] bool ready() const { return mLeft <= 0; }
    void               trigger() { mLeft = mTicks; }
    void               tick() {
        if (mLeft > 0) --mLeft;
    }

private:
    int mTicks;
    int mLeft{0};
};

} // namespace karakuri
