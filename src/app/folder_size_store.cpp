#include "folder_size_store.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <cwctype>
#include <iterator>

namespace pulse::app::folder_size {
std::wstring Key(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    while (path.size() > 7 && path.back() == L'\\') path.pop_back();
    for (auto& character : path) character = static_cast<wchar_t>(std::towlower(character));
    return path;
}
bool Within(const std::wstring& path, const std::wstring& parent) {
    return path.starts_with(parent) && (path.size() == parent.size() ||
        (!parent.empty() && parent.back() == L'\\') || path[parent.size()] == L'\\');
}
uint64_t NowUtcMs() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    const auto ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks / 10000 - 11644473600000ull;
}
Store::Entry* Store::Ensure(const std::wstring& key, const std::set<std::wstring>& protected_paths) {
    if (auto found = entries.find(key); found != entries.end()) { Touch(found->second); return &found->second; }
    if (entries.size() >= kCacheLimit) {
        auto victim = lru_.begin();
        while (victim != lru_.end() && protected_paths.contains(*victim)) ++victim;
        if (victim == lru_.end()) return nullptr;
        entries.erase(*victim); lru_.erase(victim);
    }
    lru_.push_back(key);
    auto& entry = entries[key];
    entry.position = std::prev(lru_.end());
    entry.revision = ++next_revision_;
    return &entry;
}
void Store::Touch(Entry& entry) { lru_.splice(lru_.end(), lru_, entry.position); }
void Store::MarkStale(Entry& entry, uint64_t now, uint64_t delay) {
    entry.revision = ++next_revision_; ++entry.request_epoch;
    entry.completed = 0; entry.not_before = now + delay;
    entry.value.verified = false;
    entry.value.state = entry.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
}
bool Store::InvalidateAncestors(const std::wstring& path, uint64_t now, uint64_t delay) {
    bool changed = false;
    std::wstring ancestor = path;
    for (;;) {
        if (auto found = entries.find(ancestor); found != entries.end()) {
            MarkStale(found->second, now, delay); changed = true;
        }
        if (ancestor.size() <= 7) break;
        const auto slash = ancestor.find_last_of(L'\\');
        if (slash == std::wstring::npos) break;
        ancestor.resize(slash == 6 ? 7 : slash);
    }
    return changed;
}
bool Store::Invalidate(const std::wstring& path, uint64_t now, uint64_t delay) {
    bool changed = InvalidateAncestors(path, now, delay);
    const auto prefix = path.back() == L'\\' ? path : path + L'\\';
    for (auto found = entries.lower_bound(prefix); found != entries.end() && found->first.starts_with(prefix); ++found) {
        if (found->first == path) continue;
        MarkStale(found->second, now, delay); changed = true;
    }
    return changed;
}
SavedValues Store::Snapshot() const {
    SavedValues values;
    values.reserve(entries.size());
    for (const auto& [path, entry] : entries) if (entry.value.has_value) values.emplace_back(path, entry.value);
    return values;
}
namespace {
bool Number(const std::wstring& text, uint64_t& value) {
    if (text.empty() || text.front() == L'-') return false;
    wchar_t* end = nullptr;
    value = wcstoull(text.c_str(), &end, 10);
    return end == text.c_str() + text.size();
}
}
SavedValues ReadValues(const std::wstring& file) {
    SavedValues values;
    std::wstring text;
    if (file.empty() || !ReadUtf8File(file, text)) return values;
    for (const auto& record : json::ExtractStringArray(text, L"folders")) {
        const auto first = record.find(L'\t');
        if (first == std::wstring::npos || first + 3 >= record.size() || record[first + 2] != L'\t') continue;
        FolderSizeValue value;
        if (!Number(record.substr(0, first), value.bytes)) continue;
        value.has_value = true; value.partial = record[first + 1] == L'1';
        value.state = FolderSizeState::Cached;
        size_t path_start = first + 3;
        // v2 stores provenance and scan timestamp; old records remain unknown-origin cache values.
        if (path_start + 2 < record.size() && record[path_start + 1] == L'\t' &&
            (record[path_start] == L'S' || record[path_start] == L'I' || record[path_start] == L'U')) {
            value.source = record[path_start] == L'S' ? FolderSizeSource::Scan :
                record[path_start] == L'I' ? FolderSizeSource::Index : FolderSizeSource::Unknown;
            const auto next = record.find(L'\t', path_start + 2);
            if (next == std::wstring::npos || !Number(record.substr(path_start + 2, next - path_start - 2), value.verified_at)) continue;
            path_start = next + 1;
        }
        const auto path = Key(record.substr(path_start));
        if (path.empty() || fs::IsVirtualPath(path)) continue;
        values.emplace_back(path, value);
        if (values.size() == kCacheLimit) break;
    }
    return values;
}
bool WriteValues(const std::wstring& file, const SavedValues& values) {
    if (file.empty()) return true;
    std::wstring text = L"{\"version\":2,\"folders\":[";
    bool first = true;
    for (const auto& [path, value] : values) {
        if (!value.has_value) continue;
        if (!first) text += L',';
        first = false;
        const wchar_t source = value.source == FolderSizeSource::Scan ? L'S' :
            value.source == FolderSizeSource::Index ? L'I' : L'U';
        text += L'"';
        json::Escape(std::to_wstring(value.bytes) + L"\t" + (value.partial ? L"1\t" : L"0\t") +
            source + L"\t" + std::to_wstring(value.verified_at) + L"\t" + path, text);
        text += L'"';
    }
    text += L"]}";
    return WriteUtf8FileAtomic(file, text);
}
} // namespace pulse::app::folder_size
