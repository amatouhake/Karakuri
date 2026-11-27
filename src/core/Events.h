#pragma once

// Thread-safe event log (docs/DESIGN.md 5.5 "passed up"). The server thread appends; agent-facing
// threads (MCP) read or block until something they care about arrives. Pure: no game types.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace karakuri {

struct Event {
    uint64_t    id{0};
    int64_t     tick{0};
    std::string bot;
    std::string kind;   // job_started, job_finished, reflex, hurt, died, respawned, spawned, removed
    uint64_t    jobId{0};
    std::string state;  // job_finished: done / failed / cancelled
    std::string detail; // human-readable
};

class EventLog {
public:
    explicit EventLog(size_t capacity = 512) : mCapacity(capacity) {}

    uint64_t push(Event e) {
        std::lock_guard lock(mMutex);
        e.id = mNext++;
        mEvents.push_back(std::move(e));
        while (mEvents.size() > mCapacity) mEvents.pop_front();
        mCv.notify_all();
        return mNext - 1;
    }

    // Events with id > after that match, oldest first, at most `max`.
    std::vector<Event> since(uint64_t after, std::function<bool(Event const&)> const& match = {}, size_t max = 100) {
        std::lock_guard lock(mMutex);
        return collect(after, match, max);
    }

    // Like since(), but blocks until at least one matching event exists, the timeout passes, or
    // the log is shut down.
    std::vector<Event> wait(
        uint64_t                                 after,
        std::chrono::milliseconds                timeout,
        std::function<bool(Event const&)> const& match = {},
        size_t                                   max   = 100
    ) {
        std::unique_lock lock(mMutex);
        std::vector<Event> out;
        mCv.wait_for(lock, timeout, [&] {
            out = collect(after, match, max);
            return !out.empty() || mShutdown;
        });
        return out;
    }

    [[nodiscard]] uint64_t lastId() {
        std::lock_guard lock(mMutex);
        return mNext - 1;
    }

    void shutdown() {
        std::lock_guard lock(mMutex);
        mShutdown = true;
        mCv.notify_all();
    }

    void reopen() {
        std::lock_guard lock(mMutex);
        mShutdown = false;
    }

private:
    std::vector<Event> collect(uint64_t after, std::function<bool(Event const&)> const& match, size_t max) const {
        std::vector<Event> out;
        for (auto const& e : mEvents) {
            if (e.id <= after || (match && !match(e))) continue;
            out.push_back(e);
            if (out.size() >= max) break;
        }
        return out;
    }

    size_t                  mCapacity;
    std::mutex              mMutex;
    std::condition_variable mCv;
    std::deque<Event>       mEvents;
    uint64_t                mNext{1};
    bool                    mShutdown{false};
};

} // namespace karakuri
