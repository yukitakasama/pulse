#pragma once
#include "folder_sizes.h"
#include <list>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace pulse::app::folder_size {
inline constexpr size_t kCacheLimit = 4096;
std::wstring Key(std::wstring path);
bool Within(const std::wstring& path, const std::wstring& parent);
uint64_t NowUtcMs();
using SavedValues = std::vector<std::pair<std::wstring, FolderSizeValue>>;
SavedValues ReadValues(const std::wstring& file);
bool WriteValues(const std::wstring& file, const SavedValues& values);

// The coordinator holds its mutex while accessing this bounded memory store.
class Store {
public:
    struct Entry {
        FolderSizeValue value;
        uint64_t completed = 0, not_before = 0;
        uint64_t revision = 0, request_epoch = 0, watch_generation = 0;
        std::list<std::wstring>::iterator position;
    };
    Entry* Ensure(const std::wstring& key, const std::set<std::wstring>& protected_paths);
    void Touch(Entry& entry);
    void MarkStale(Entry& entry, uint64_t now, uint64_t delay);
    bool InvalidateAncestors(const std::wstring& path, uint64_t now, uint64_t delay);
    bool Invalidate(const std::wstring& path, uint64_t now, uint64_t delay);
    SavedValues Snapshot() const;
    std::map<std::wstring, Entry> entries;
private:
    std::list<std::wstring> lru_;
    uint64_t next_revision_ = 0;
};
} // namespace pulse::app::folder_size
