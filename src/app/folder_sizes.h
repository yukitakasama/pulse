#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace pulse::app {
enum class FolderSizeState { Manual, Calculating, Ready, Updating, Partial, Unavailable, Cached, Indexed };
enum class FolderSizeSource { Unknown, Scan, Index };
struct FolderSizeValue {
    FolderSizeState state = FolderSizeState::Manual;
    uint64_t bytes = 0;
    bool has_value = false;
    // A lower bound remains incomplete while updating or restored from cache.
    bool partial = false;
    FolderSizeSource source = FolderSizeSource::Unknown;
    // Only a complete scan protected by an uninterrupted watch is reusable.
    bool verified = false;
    uint64_t verified_revision = 0;
    uint64_t verified_at = 0; // Unix milliseconds of the completed scan.
};
struct FolderSizeRequest {
    std::wstring path;
    bool automatic = true;
    bool visible = true;
};
struct FolderSizeStats {
    uint64_t jobs_started = 0, jobs_completed = 0, jobs_cancelled = 0;
    uint64_t entries_scanned = 0, subtree_hits = 0, index_queries = 0;
    uint64_t watch_gaps = 0, cache_items = 0;
};

// Window-thread methods only exchange in-memory state. Enumeration, watches and
// cache I/O belong to the background worker, including network paths.
class FolderSizes {
public:
    FolderSizes();
    ~FolderSizes();
    FolderSizes(const FolderSizes&) = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;
    void SetCachePath(std::function<std::wstring()> path);
    void SetIndexEnabled(bool enabled);
    void Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> watch_roots);
    void Calculate(const std::wstring& path);
    FolderSizeValue Get(const std::wstring& path) const;
    // Known totals of the direct subfolders of `parent`, keyed by lower-cased
    // name: what a Size sort of that folder can use right now (#58).
    std::unordered_map<std::wstring, uint64_t> KnownChildren(const std::wstring& parent) const;
    void Invalidate(const std::wstring& path);
    bool TakeChanged();
    FolderSizeStats ReadStats() const;
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
