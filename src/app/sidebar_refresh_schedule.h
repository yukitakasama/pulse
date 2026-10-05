#pragma once
#include <cstdint>
#include <optional>

namespace pulse::app {
// UI-thread request coalescing; periodic ticks never queue behind a slow read.
class SidebarRefreshSchedule {
public:
    static constexpr uint64_t kIntervalMs = 30000;
    void Request(bool rebuild) {
        pending_ = true;
        rebuild_ = rebuild_ || rebuild;
    }
    void Tick(uint64_t now) {
        if (!busy_ && now >= next_refresh_) Request(false);
    }
    std::optional<bool> Begin() {
        if (busy_ || !pending_) return std::nullopt;
        const bool rebuild = rebuild_;
        pending_ = rebuild_ = false;
        busy_ = true;
        return rebuild;
    }
    void Complete(uint64_t now) {
        busy_ = false;
        next_refresh_ = now + kIntervalMs;
    }
    bool RebuildPending() const { return rebuild_; }
private:
    bool busy_ = false;
    bool pending_ = false;
    bool rebuild_ = false;
    uint64_t next_refresh_ = 0;
};
} // namespace pulse::app
