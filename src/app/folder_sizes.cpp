#include "folder_sizes.h"
#include "folder_size_scanner.h"
#include "folder_size_store.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../common/path_utils.h"
#include "../index/index_feed.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

namespace pulse::app {
namespace {
using folder_size::Key;
using folder_size::Within;
constexpr uint64_t kFreshMs = 60000;
constexpr size_t kWatchLimit = 16;
bool NetworkPath(const std::wstring& path) {
    return fs::IsUncPath(path) || path.starts_with(L"\\\\?\\unc\\");
}
std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return parent + (parent.back() == L'\\' ? L"" : L"\\") + name;
}
std::vector<std::wstring> MergeRoots(std::vector<std::wstring> roots) {
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return a.size() == b.size() ? a < b : a.size() < b.size();
    });
    std::vector<std::wstring> merged;
    for (auto& root : roots) {
        if (root.empty() || fs::IsVirtualPath(root)) continue;
        if (std::any_of(merged.begin(), merged.end(), [&](const auto& parent) { return Within(root, parent); })) continue;
        if (merged.size() == kWatchLimit) break;
        merged.push_back(std::move(root));
    }
    return merged;
}
}
struct FolderSizes::Impl {
    struct Watch {
        std::shared_ptr<fs::DirWatch> handle;
        uint64_t instance = 0, generation = 0;
        bool started = false;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    folder_size::Store store;
    std::map<std::wstring, bool> wanted;
    std::set<std::wstring> protected_paths, manual, blocked_auto;
    std::vector<std::wstring> order, roots;
    std::vector<FolderSizeRequest> last_visible;
    std::vector<std::wstring> last_roots;
    std::map<std::wstring, Watch> watches;
    std::map<std::wstring, UINT> drive_types;
    std::function<std::wstring()> cache_path;
    std::thread worker, index_worker;
    std::atomic<bool> stopping{false}, changed{false};
    bool index_enabled = true;
    uint64_t scope = 0, watch_sequence = 0, dirty_revision = 0, saved_revision = 0;
    FolderSizeStats stats;
    HANDLE index_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() { if (index_cancel) CloseHandle(index_cancel); }

    uint64_t Coverage(const std::wstring& path) const {
        for (const auto& [root, watch] : watches)
            if (Within(path, root) && watch.started && watch.handle->Armed() &&
                std::find(roots.begin(), roots.end(), root) != roots.end()) return watch.generation;
        return 0;
    }
    bool Reusable(const std::wstring& path, const folder_size::Store::Entry& entry) const {
        return entry.value.has_value && entry.value.source == FolderSizeSource::Scan &&
            !entry.value.partial && entry.value.verified && entry.value.verified_revision == entry.revision &&
            entry.watch_generation != 0 && Coverage(path) == entry.watch_generation;
    }
    void InvalidateLocked(const std::wstring& key, uint64_t delay = 200) {
        if (key.empty()) return;
        if (store.Invalidate(key, GetTickCount64(), delay)) { changed = true; ++dirty_revision; }
    }
    void Invalidate(const std::wstring& path) {
        const auto key = Key(path);
        { std::lock_guard lock(mutex); InvalidateLocked(key); }
        wake.notify_all();
    }
    UINT DriveType(const std::wstring& path) {
        const auto plain = pulse::path::StripExtendedPathPrefix(path);
        if (plain.size() < 3 || plain[1] != L':') return DRIVE_REMOTE;
        const auto root = plain.substr(0, 3);
        const auto found = drive_types.find(root);
        if (found != drive_types.end()) return found->second;
        return drive_types.emplace(root, GetDriveTypeW(root.c_str())).first->second;
    }
    void Save(const std::wstring& file) {
        if (file.empty()) return;
        folder_size::SavedValues values;
        uint64_t revision = 0;
        {
            std::lock_guard lock(mutex);
            if (dirty_revision == saved_revision) return;
            values = store.Snapshot(); revision = dirty_revision;
        }
        if (folder_size::WriteValues(file, values)) {
            std::lock_guard lock(mutex); saved_revision = revision;
        }
    }
    void UpdateWatches(const std::wstring& cache_key) {
        std::vector<std::shared_ptr<fs::DirWatch>> retired;
        std::vector<std::wstring> create;
        {
            std::lock_guard lock(mutex);
            for (auto it = watches.begin(); it != watches.end();) {
                if (std::find(roots.begin(), roots.end(), it->first) == roots.end()) {
                    retired.push_back(std::move(it->second.handle)); it = watches.erase(it);
                } else ++it;
            }
            for (const auto& root : roots) if (!watches.contains(root)) create.push_back(root);
        }
        for (auto& watch : retired) watch->Stop();
        for (const auto& root : create) {
            if (stopping || DriveType(root) == DRIVE_REMOTE) continue;
            auto watch = std::make_shared<fs::DirWatch>();
            uint64_t instance = 0;
            {
                std::lock_guard lock(mutex);
                if (std::find(roots.begin(), roots.end(), root) == roots.end()) continue;
                instance = ++watch_sequence;
                watches.emplace(root, Watch{watch, instance, instance});
            }
            const bool started = watch->Start(root, [this, root, instance, cache_key](bool gap, std::vector<fs::DirNotifyEvent> events) {
                {
                    std::lock_guard lock(mutex);
                    const auto found = watches.find(root);
                    if (stopping || found == watches.end() || found->second.instance != instance ||
                        std::find(roots.begin(), roots.end(), root) == roots.end()) return;
                    if (gap) {
                        ++stats.watch_gaps;
                        const bool initial = found->second.generation == found->second.instance;
                        found->second.generation = ++watch_sequence;
                        // The first notification reconciles a newly armed read;
                        // it is not a write burst that needs a debounce delay.
                        InvalidateLocked(root, initial ? 0 : 200);
                    } else {
                        std::set<std::wstring> paths;
                        for (const auto& event : events) {
                            const auto path = Key(ChildPath(root, event.name));
                            if (!cache_key.empty() && (path == cache_key || path == cache_key + L".tmp")) continue;
                            paths.insert(path);
                            if (!event.old_name.empty()) paths.insert(Key(ChildPath(root, event.old_name)));
                        }
                        for (const auto& path : paths) InvalidateLocked(path);
                    }
                }
                wake.notify_all();
            }, true);
            {
                std::lock_guard lock(mutex);
                const auto found = watches.find(root);
                if (found != watches.end() && found->second.instance == instance) found->second.started = started;
            }
        }
    }
    void RunIndex() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        index::IndexFeedConnection connection(index_cancel);
        uint64_t seen_scope = 0, next_poll = 0;
        size_t cursor = 0;
        while (!stopping) {
            std::vector<std::wstring> paths;
            std::vector<std::pair<uint64_t, uint64_t>> versions;
            uint64_t request_scope = 0;
            {
                std::unique_lock lock(mutex);
                if (seen_scope != scope) { seen_scope = scope; next_poll = 0; cursor = 0; }
                const auto now = GetTickCount64();
                if (now < next_poll) {
                    wake.wait_for(lock, std::chrono::milliseconds(std::min<uint64_t>(next_poll - now, 250)));
                    continue;
                }
                for (const auto& path : order) {
                    const auto demand = wanted.find(path);
                    const auto entry = store.entries.find(path);
                    if (demand == wanted.end() || !demand->second || entry == store.entries.end() ||
                        manual.contains(path) || NetworkPath(path) ||
                        (entry->second.value.source == FolderSizeSource::Scan && entry->second.value.has_value)) continue;
                    paths.push_back(path);
                }
                if (paths.empty()) { next_poll = now + 1000; continue; }
                cursor %= paths.size();
                std::rotate(paths.begin(), paths.begin() + cursor, paths.end());
                cursor = (cursor + index::kFolderSizeBatch) % paths.size();
                if (paths.size() > index::kFolderSizeBatch) paths.resize(index::kFolderSizeBatch);
                for (const auto& path : paths) {
                    const auto& entry = store.entries.at(path);
                    versions.emplace_back(entry.revision, entry.request_epoch);
                }
                if (!stopping && index_cancel) ResetEvent(index_cancel);
                request_scope = scope; ++stats.index_queries;
            }
            std::vector<index::IndexedFolderSize> values;
            const bool received = connection.FolderSizes(paths, values);
            {
                std::lock_guard lock(mutex);
                next_poll = GetTickCount64() + (received ? 1000 : 10000);
                if (stopping || scope != request_scope) continue;
                for (size_t i = 0; i < paths.size(); ++i) {
                    const auto found = store.entries.find(paths[i]);
                    const auto demand = wanted.find(paths[i]);
                    if (found == store.entries.end() || demand == wanted.end() || !demand->second || manual.contains(paths[i])) continue;
                    auto& entry = found->second;
                    if (entry.revision != versions[i].first || entry.request_epoch != versions[i].second ||
                        (entry.value.source == FolderSizeSource::Scan && entry.value.has_value)) continue;
                    if (received && i < values.size() && values[i].available) {
                        entry.value = {FolderSizeState::Indexed, values[i].bytes, true};
                        entry.value.source = FolderSizeSource::Index;
                        ++dirty_revision; changed = true;
                    } else if (entry.value.source == FolderSizeSource::Index && entry.value.has_value) {
                        entry.value.state = FolderSizeState::Cached; changed = true;
                    }
                }
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
    void Publish(folder_size::Scan& job) {
        const auto root = store.entries.find(job.path);
        const auto demand = wanted.find(job.path);
        if (root == store.entries.end() || demand == wanted.end() || !demand->second ||
            root->second.revision != job.revision || root->second.request_epoch != job.request_epoch) {
            ++stats.jobs_cancelled; return;
        }
        const auto now = GetTickCount64();
        const auto utc = folder_size::NowUtcMs();
        for (auto& subtree : job.completed) {
            const auto key = Key(subtree.path);
            auto* entry = store.Ensure(key, protected_paths);
            if (!entry) continue;
            if (!subtree.value.has_value && entry->value.has_value) {
                entry->value.state = FolderSizeState::Cached;
                entry->value.verified = false;
            } else {
                entry->value = subtree.value;
                entry->value.verified_revision = entry->revision;
                entry->value.verified_at = subtree.value.has_value ? utc : 0;
                entry->watch_generation = subtree.watch_generation;
                entry->value.verified = subtree.value.has_value && !subtree.value.partial &&
                    subtree.watch_generation != 0 && Coverage(key) == subtree.watch_generation;
            }
            entry->completed = now; entry->not_before = 0;
        }
        ++stats.jobs_completed; ++dirty_revision; changed = true;
    }
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto file = cache_path ? cache_path() : std::wstring();
        const auto cache_key = file.empty() ? std::wstring() : Key(file);
        const auto restored = folder_size::ReadValues(file);
        {
            std::lock_guard lock(mutex);
            for (const auto& [path, value] : restored) {
                auto* entry = store.Ensure(path, protected_paths);
                if (!entry || entry->value.source == FolderSizeSource::Scan) continue;
                if (!entry->value.has_value || value.source == FolderSizeSource::Scan) entry->value = value;
            }
            if (!restored.empty()) changed = true;
        }
        std::deque<std::unique_ptr<folder_size::Scan>> jobs;
        uint64_t last_save = GetTickCount64(), schedule_scope = 0;
        while (!stopping) {
            UpdateWatches(cache_key);
            std::vector<std::wstring> classify;
            {
                std::lock_guard lock(mutex);
                for (const auto& path : order)
                    if (wanted.at(path) && !manual.contains(path)) classify.push_back(path);
            }
            for (const auto& path : classify) {
                const auto type = DriveType(path);
                if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) {
                    std::lock_guard lock(mutex);
                    if (wanted.contains(path) && !manual.contains(path)) {
                        blocked_auto.insert(path); wanted[path] = false; changed = true;
                    }
                }
            }
            {
                std::unique_lock lock(mutex);
                std::erase_if(jobs, [&](const auto& job) {
                    const auto demand = wanted.find(job->path);
                    const auto entry = store.entries.find(job->path);
                    const bool cancelled = demand == wanted.end() || !demand->second || entry == store.entries.end() ||
                        entry->second.revision != job->revision || entry->second.request_epoch != job->request_epoch;
                    if (cancelled) ++stats.jobs_cancelled;
                    return cancelled;
                });
                const auto now = GetTickCount64();
                const bool reprioritize = schedule_scope != scope;
                std::map<std::wstring, size_t> priority;
                if (reprioritize) for (size_t i = 0; i < order.size(); ++i) priority.emplace(order[i], i);
                for (const auto& path : order) {
                    if (jobs.size() == 32 && !reprioritize) break;
                    if (!wanted.at(path)) continue;
                    auto& entry = store.entries.at(path);
                    if (now < entry.not_before || Reusable(path, entry)) continue;
                    if (entry.completed && now - entry.completed < kFreshMs) continue;
                    if (std::any_of(jobs.begin(), jobs.end(), [&](const auto& job) {
                        return Within(path, job->path) || Within(job->path, path);
                    })) continue;
                    if (jobs.size() == 32) {
                        const auto lowest = std::max_element(jobs.begin(), jobs.end(), [&](const auto& a, const auto& b) {
                            return priority.at(a->path) < priority.at(b->path);
                        });
                        if (priority.at((*lowest)->path) <= priority.at(path)) continue;
                        jobs.erase(lowest); ++stats.jobs_cancelled;
                    }
                    if (entry.value.source != FolderSizeSource::Index)
                        entry.value.state = entry.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
                    jobs.push_back(std::make_unique<folder_size::Scan>(path, entry.revision, entry.request_epoch));
                    ++stats.jobs_started; changed = true;
                }
                if (reprioritize) {
                    schedule_scope = scope;
                    std::stable_sort(jobs.begin(), jobs.end(), [&](const auto& a, const auto& b) {
                        return priority.at(a->path) < priority.at(b->path);
                    });
                }
                if (jobs.empty()) wake.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (!jobs.empty() && !stopping) {
                auto job = std::move(jobs.front()); jobs.pop_front();
                const auto before_entries = job->entries_scanned, before_hits = job->subtree_hits;
                job->Step(stopping, [&](const std::wstring& path) -> std::optional<uint64_t> {
                    const auto key = Key(path);
                    std::lock_guard lock(mutex);
                    const auto found = store.entries.find(key);
                    if (found == store.entries.end() || !Reusable(key, found->second)) return std::nullopt;
                    store.Touch(found->second);
                    return found->second.value.bytes;
                }, [&](const std::wstring& path) -> uint64_t {
                    // Only this worker changes the watch map structure.
                    if (watches.empty()) return 0;
                    const auto key = Key(path);
                    std::lock_guard lock(mutex); return Coverage(key);
                });
                const bool completed = job->done;
                {
                    std::lock_guard lock(mutex);
                    stats.entries_scanned += job->entries_scanned - before_entries;
                    stats.subtree_hits += job->subtree_hits - before_hits;
                    if (completed) Publish(*job);
                }
                if (!completed) {
                    jobs.push_back(std::move(job));
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(4), [&] { return stopping.load(); });
                }
            }
            if (GetTickCount64() - last_save >= 3000) { Save(file); last_save = GetTickCount64(); }
        }
        jobs.clear();
        std::vector<std::shared_ptr<fs::DirWatch>> retired;
        {
            std::lock_guard lock(mutex);
            for (auto& [path, watch] : watches) retired.push_back(std::move(watch.handle));
            watches.clear();
        }
        for (auto& watch : retired) watch->Stop();
        Save(file);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};

FolderSizes::FolderSizes() : impl_(std::make_unique<Impl>()) {}
FolderSizes::~FolderSizes() { Stop(); }
void FolderSizes::SetCachePath(std::function<std::wstring()> path) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->cache_path = std::move(path);
}
void FolderSizes::SetIndexEnabled(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->index_enabled = enabled;
}
void FolderSizes::Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> requested_roots) {
    std::vector<FolderSizeRequest> normalized;
    std::map<std::wstring, size_t> positions;
    for (const bool priority : {true, false}) for (const auto& request : visible) {
        if (request.visible != priority || request.path.empty() || fs::IsVirtualPath(request.path)) continue;
        const auto key = Key(request.path);
        const auto found = positions.find(key);
        if (found != positions.end()) { normalized[found->second].automatic |= request.automatic; continue; }
        if (normalized.size() == folder_size::kCacheLimit) continue;
        positions.emplace(key, normalized.size());
        normalized.push_back({key, request.automatic, priority});
    }
    for (auto& root : requested_roots) root = Key(root);
    requested_roots = MergeRoots(std::move(requested_roots));
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    if (state.stopping) return;
    const auto same = [](const auto& a, const auto& b) {
        return a.path == b.path && a.automatic == b.automatic && a.visible == b.visible;
    };
    if (requested_roots == state.last_roots &&
        std::equal(normalized.begin(), normalized.end(), state.last_visible.begin(), state.last_visible.end(), same)) return;
    state.last_visible = normalized; state.last_roots = requested_roots;
    // A retained parent watch covers downward navigation without a monitoring gap.
    std::vector<std::wstring> effective_roots = requested_roots;
    for (const auto& root : state.roots)
        if (std::any_of(requested_roots.begin(), requested_roots.end(), [&](const auto& requested) { return Within(requested, root); }))
            effective_roots.push_back(root);
    effective_roots = MergeRoots(std::move(effective_roots));
    for (const auto& root : state.roots)
        if (std::find(effective_roots.begin(), effective_roots.end(), root) == effective_roots.end()) state.InvalidateLocked(root, 0);
    state.roots = std::move(effective_roots);
    std::set<std::wstring> protected_paths;
    for (const auto& request : normalized) protected_paths.insert(request.path);
    bool cancelled = false;
    for (const auto& [path, automatic] : state.wanted) if (!protected_paths.contains(path)) {
        if (auto found = state.store.entries.find(path); found != state.store.entries.end()) ++found->second.request_epoch;
        state.manual.erase(path); cancelled |= automatic;
    }
    std::erase_if(state.blocked_auto, [&](const auto& path) { return !protected_paths.contains(path); });
    std::map<std::wstring, bool> wanted;
    std::vector<std::wstring> order;
    for (const auto& request : normalized) {
        auto* entry = state.store.Ensure(request.path, protected_paths);
        if (!entry) continue;
        if (!state.wanted.contains(request.path) && entry->value.source == FolderSizeSource::Scan &&
            entry->value.has_value && !state.Reusable(request.path, *entry)) state.store.MarkStale(*entry, GetTickCount64(), 0);
        wanted[request.path] = (request.automatic && !NetworkPath(request.path) &&
            !state.blocked_auto.contains(request.path)) || state.manual.contains(request.path);
        order.push_back(request.path);
    }
    state.protected_paths = std::move(protected_paths);
    state.wanted = std::move(wanted); state.order = std::move(order); ++state.scope;
    if (state.index_cancel) SetEvent(state.index_cancel);
    if (!state.worker.joinable() && !state.wanted.empty()) {
        state.worker = std::thread([&state] { state.Run(); });
        if (state.index_enabled) state.index_worker = std::thread([&state] { state.RunIndex(); });
    }
    if (cancelled && state.worker.joinable()) CancelSynchronousIo(state.worker.native_handle());
    state.wake.notify_all();
}
void FolderSizes::Calculate(const std::wstring& path) {
    auto& state = *impl_;
    const auto key = Key(path);
    std::lock_guard lock(state.mutex);
    const auto demand = state.wanted.find(key);
    if (demand == state.wanted.end()) return;
    demand->second = true; state.manual.insert(key);
    state.store.InvalidateAncestors(key, GetTickCount64(), 0);
    ++state.scope; state.changed = true;
    if (state.index_cancel) SetEvent(state.index_cancel);
    state.wake.notify_all();
}
FolderSizeValue FolderSizes::Get(const std::wstring& path) const {
    const auto key = Key(path);
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    const auto found = state.store.entries.find(key);
    auto value = found == state.store.entries.end() ? FolderSizeValue{} : found->second.value;
    const auto wanted = state.wanted.find(key);
    if (value.verified && !state.Reusable(key, found->second)) {
        value.verified = false;
        value.state = value.has_value ? FolderSizeState::Cached : FolderSizeState::Calculating;
    }
    if (wanted != state.wanted.end() && !wanted->second) {
        if (value.state == FolderSizeState::Calculating) value.state = FolderSizeState::Manual;
        if (value.state == FolderSizeState::Updating) value.state = FolderSizeState::Cached;
    } else if (wanted != state.wanted.end() && !value.has_value && value.state == FolderSizeState::Manual) {
        value.state = FolderSizeState::Calculating;
    }
    return value;
}
std::unordered_map<std::wstring, uint64_t> FolderSizes::KnownChildren(const std::wstring& parent) const {
    std::unordered_map<std::wstring, uint64_t> children;
    auto prefix = Key(parent);
    if (prefix.empty() || fs::IsVirtualPath(prefix)) return children;
    if (prefix.back() != L'\\') prefix += L'\\';
    auto& state = *impl_;
    std::lock_guard lock(state.mutex);
    for (auto found = state.store.entries.lower_bound(prefix); found != state.store.entries.end() && found->first.starts_with(prefix); ++found) {
        if (!found->second.value.has_value) continue;
        std::wstring_view rest(found->first); rest.remove_prefix(prefix.size());
        if (rest.empty() || rest.find(L'\\') != std::wstring_view::npos) continue;
        children.emplace(std::wstring(rest), found->second.value.bytes);
    }
    return children;
}
void FolderSizes::Invalidate(const std::wstring& path) { impl_->Invalidate(path); }
bool FolderSizes::TakeChanged() { return impl_->changed.exchange(false); }
FolderSizeStats FolderSizes::ReadStats() const {
    std::lock_guard lock(impl_->mutex);
    auto stats = impl_->stats; stats.cache_items = impl_->store.entries.size(); return stats;
}
void FolderSizes::Stop() {
    auto& state = *impl_;
    {
        std::lock_guard lock(state.mutex);
        state.stopping = true;
        if (state.index_cancel) SetEvent(state.index_cancel);
    }
    state.wake.notify_all();
    if (state.worker.joinable()) { CancelSynchronousIo(state.worker.native_handle()); state.worker.join(); }
    if (state.index_worker.joinable()) { CancelSynchronousIo(state.index_worker.native_handle()); state.index_worker.join(); }
}
} // namespace pulse::app
