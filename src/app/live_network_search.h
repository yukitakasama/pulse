#pragma once
#include "../index/network_index.h"
#include <atomic>
#include <memory>
#include <unordered_map>

namespace pulse {
struct LiveNetworkSearch {
    uint64_t session_id = 0;
    std::wstring key;
    std::wstring folder;
    std::mutex mutex;
    index::LiveNetworkMatches matches;
    std::atomic<bool> cancel{false};
    std::atomic<uint32_t> latest_id{0};
};

// UI-thread registry; workers retain their own state until cancellation completes.
class LiveNetworkSearchSessions {
public:
    ~LiveNetworkSearchSessions() {
        for (const auto& [session, live] : sessions_) {
            (void)session;
            live->cancel = true;
        }
    }
    std::shared_ptr<LiveNetworkSearch> Find(uint64_t session) const {
        const auto found = sessions_.find(session);
        return found == sessions_.end() ? nullptr : found->second;
    }
    void Drop(uint64_t session) {
        if (const auto live = Find(session)) live->cancel = true;
        sessions_.erase(session);
    }
    void Set(const std::shared_ptr<LiveNetworkSearch>& live) {
        Drop(live->session_id);
        sessions_[live->session_id] = live;
    }
    bool Owns(const std::shared_ptr<LiveNetworkSearch>& live) const {
        return live && !live->cancel && Find(live->session_id) == live;
    }
private:
    std::unordered_map<uint64_t, std::shared_ptr<LiveNetworkSearch>> sessions_;
};
} // namespace pulse
