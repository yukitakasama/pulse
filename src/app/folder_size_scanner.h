#pragma once
#include "folder_sizes.h"
#include <windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace pulse::app::folder_size {
struct CompletedSubtree {
    std::wstring path;
    FolderSizeValue value;
    uint64_t watch_generation = 0;
};
class Scan {
public:
    using Lookup = std::function<std::optional<uint64_t>(const std::wstring&)>;
    using Coverage = std::function<uint64_t(const std::wstring&)>;
    Scan(std::wstring path, uint64_t revision, uint64_t request_epoch);
    ~Scan();
    void Step(const std::atomic<bool>& stopping, const Lookup& lookup, const Coverage& coverage);
    std::wstring path;
    uint64_t revision = 0, request_epoch = 0;
    uint64_t entries_scanned = 0, subtree_hits = 0;
    FolderSizeValue result{FolderSizeState::Calculating};
    std::deque<CompletedSubtree> completed;
    bool done = false;
private:
    struct Frame {
        std::wstring path;
        HANDLE find = INVALID_HANDLE_VALUE;
        WIN32_FIND_DATAW data{};
        uint64_t bytes = 0, watch_generation = 0;
        bool started = false, partial = false, read_any = false;
    };
    std::vector<Frame> stack_;
    void FinishFrame();
};
} // namespace pulse::app::folder_size
