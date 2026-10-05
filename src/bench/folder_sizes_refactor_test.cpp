#include "../app/folder_sizes.h"
#include <windows.h>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace pulse::app {
bool RunFolderSizeIndexClientTest(const fs::path& fixture, std::ofstream& log);
int RunFolderSizeFakeIndexServer(const wchar_t* pipe_name);
}
namespace {
using pulse::app::FolderSizes;
using State = pulse::app::FolderSizeState;
using Source = pulse::app::FolderSizeSource;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
bool Wait(const std::function<bool()>& predicate, DWORD timeout = 6000) {
    const auto deadline = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(5); } while (GetTickCount64() < deadline);
    return predicate();
}
void File(const fs::path& path, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::string(size, 'x');
    if (!out) throw std::runtime_error("fixture write failed");
}
bool Exact(FolderSizes& sizes, const fs::path& path, uint64_t bytes) {
    const auto value = sizes.Get(path.wstring());
    return value.state == State::Ready && value.has_value && value.bytes == bytes &&
           value.source == Source::Scan && !value.partial;
}
class Watchdog {
public:
    Watchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, std::chrono::seconds(90), [this] { return done_; })) {
            std::fprintf(stderr, "[FAIL] isolated folder-size test exceeded 90 seconds\n");
            TerminateProcess(GetCurrentProcess(), 124);
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        condition_.notify_one(); thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    std::thread thread_;
};

void WatchedChanges(const fs::path& fixture) {
    const auto parent = fixture / L"watched";
    const auto child = parent / L"child";
    fs::create_directories(child);
    File(parent / L"a.bin", 10); File(child / L"b.bin", 20);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{parent.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 30) && sizes.Get(parent.wstring()).verified; }),
          "watched complete scan records verified source and total");
    const auto initial = sizes.Get(parent.wstring());
    Check(initial.verified_at != 0 && initial.source == Source::Scan,
          "verified scan records its verification timestamp");
    const auto before_reuse = sizes.ReadStats();
    sizes.Calculate(parent.wstring());
    Check(Wait([&] { return Exact(sizes, parent, 30) && sizes.ReadStats().jobs_completed > before_reuse.jobs_completed; }),
          "recalculating under unchanged watch returns the same real total");
    Check(sizes.ReadStats().subtree_hits > before_reuse.subtree_hits,
          "unchanged verified child subtree is reused during parent calculation");
    File(parent / L"a.bin", 40);
    Check(Wait([&] { return Exact(sizes, parent, 60); }), "watch observes larger file without explicit invalidation");
    File(parent / L"new.bin", 5);
    Check(Wait([&] { return Exact(sizes, parent, 65); }), "watch observes newly created file");
    fs::remove(child / L"b.bin");
    Check(Wait([&] { return Exact(sizes, parent, 45); }), "watch observes deletion without retaining obsolete child bytes");
    fs::rename(parent / L"new.bin", child / L"moved.bin");
    sizes.Sync({{parent.wstring()}, {child.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 45) && Exact(sizes, child, 5); }),
          "rename across subtrees invalidates both source and destination totals");
    const auto renamed = parent / L"renamed";
    fs::rename(child, renamed);
    sizes.Sync({{parent.wstring()}, {renamed.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 45) && Exact(sizes, renamed, 5); }),
          "renamed directory receives current subtree total");
    auto quiet_count = sizes.ReadStats().jobs_started;
    auto quiet_since = GetTickCount64();
    Check(Wait([&] {
        const auto count = sizes.ReadStats().jobs_started;
        if (count != quiet_count) { quiet_count = count; quiet_since = GetTickCount64(); }
        return Exact(sizes, parent, 45) && GetTickCount64() - quiet_since >= 300;
    }), "watch rename notifications settle before measuring hot Sync");
    const auto before_hot = sizes.ReadStats();
    for (unsigned repeat = 0; repeat < 30; ++repeat)
        sizes.Sync({{parent.wstring()}, {renamed.wstring()}}, {parent.wstring()});
    Sleep(80);
    Check(sizes.ReadStats().jobs_started == before_hot.jobs_started && Exact(sizes, parent, 45),
          "unchanged hot Sync does not create repeated scans");

    // Leaving watch coverage must revoke authority of cached subtree results.
    sizes.Sync({}, {});
    Check(Wait([&] { return !sizes.Get(parent.wstring()).verified; }),
          "leaving watch coverage revokes verification of retained totals");
    File(renamed / L"moved.bin", 70);
    sizes.Sync({{parent.wstring()}}, {parent.wstring()});
    const auto returned = sizes.Get(parent.wstring());
    Check(!(returned.verified && returned.bytes == 45), "returning after watch gap cannot label old total verified");
    Check(Wait([&] { return Exact(sizes, parent, 110); }), "watch gap forces updated real total instead of stale subtree reuse");
    sizes.Stop();
}

void PartialAndPersistence(const fs::path& fixture) {
    const auto parent = fixture / L"partial";
    const auto offline = parent / L"child" / L"offline";
    const auto cache = (fixture / L"partial-cache.json").wstring();
    fs::create_directories(offline);
    File(parent / L"readable.bin", 17);
    File(parent / L"child" / L"readable.bin", 23);
    File(offline / L"skipped.bin", 91);
    const bool attributed = SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE) != FALSE;
    Check(attributed, "create isolated offline-subtree fixture");
    if (!attributed) return;
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.SetCachePath([cache] { return cache; });
        sizes.Sync({{parent.wstring()}}, {parent.wstring()});
        Check(Wait([&] { return sizes.Get(parent.wstring()).state == State::Partial; }), "skipped offline subtree remains partial");
        const auto value = sizes.Get(parent.wstring());
        Check(value.has_value && value.bytes == 40 && value.partial && value.source == Source::Scan && !value.verified,
              "partial child still contributes its readable bytes to parent lower bound");
        sizes.Stop();
    }
    {
        FolderSizes loaded; loaded.SetIndexEnabled(false); loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{parent.wstring(), false}}, {});
        Check(Wait([&] { return loaded.Get(parent.wstring()).has_value; }), "partial cache reload produces a retained value");
        const auto value = loaded.Get(parent.wstring());
        Check(value.bytes == 40 && value.partial && !value.verified && value.state == State::Cached && value.source == Source::Scan,
              "restart preserves lower-bound provenance while revoking verification");
        Check(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY) != FALSE, "restore isolated offline directory");
        loaded.Calculate(parent.wstring());
        Check(Wait([&] { return Exact(loaded, parent, 131); }) && !loaded.Get(parent.wstring()).verified,
              "complete scan replaces partial cache; no-watch snapshot does not claim watch verification");
        loaded.Stop();
    }
    {
        FolderSizes loaded; loaded.SetIndexEnabled(false); loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{parent.wstring(), false}}, {});
        Check(Wait([&] { return loaded.Get(parent.wstring()).has_value; }) &&
              loaded.Get(parent.wstring()).bytes == 131 && !loaded.Get(parent.wstring()).partial &&
              !loaded.Get(parent.wstring()).verified && loaded.Get(parent.wstring()).state == State::Cached,
              "complete disk cache is retained but is not misreported as a new accurate scan");
        loaded.Stop();
    }
}

void Cancellation(const fs::path& fixture) {
    // Reuse the isolated workload generated by the real index-client checks.
    const auto large = fixture / L"index-estimate-workload";
    const auto empty = fixture / L"empty";
    // Several slices give the controller a real cancellation window even on a
    // warm local filesystem, without relying on the index worker's timing.
    for (unsigned folder = 0; folder < 64; ++folder)
        for (unsigned file = 32; file < 128; ++file)
            File(large / std::to_wstring(folder) / (std::to_wstring(file) + L".bin"), 13);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{large.wstring()}}, {});
    Check(Wait([&] { return sizes.ReadStats().entries_scanned > 0; }), "cancellation reaches a real in-progress scan");
    sizes.Sync({{empty.wstring()}}, {});
    Check(Wait([&] { return Exact(sizes, empty, 0); }), "new visible empty directory completes after cancelling previous scope");
    Check(sizes.ReadStats().jobs_cancelled > 0, "offscreen unfinished job is cancelled");
    const auto cancelled = sizes.Get(large.wstring());
    Check(cancelled.state != State::Ready && !cancelled.verified,
          "cancelled old epoch cannot publish a completed accurate result");
    File(large / L"after-cancel.bin", 27);
    sizes.Sync({{large.wstring()}}, {});
    Check(Wait([&] { return Exact(sizes, large, 64 * 128 * 13 + 27); }),
          "returning after cancellation rescans and includes newly created bytes");
    sizes.Stop();
}

void ChildCalculationCancelsAncestor(const fs::path& fixture) {
    const auto parent = fixture / L"index-estimate-workload";
    const auto child = parent / L"calculate-child";
    fs::create_directories(child);
    File(child / L"value.bin", 7);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{parent.wstring()}, {child.wstring()}}, {});
    bool in_flight = false;
    const auto deadline = GetTickCount64() + 6000;
    do {
        const auto stats = sizes.ReadStats();
        in_flight = stats.entries_scanned > 0 && stats.jobs_completed == 0 &&
                    sizes.Get(parent.wstring()).state != State::Ready;
        if (in_flight || stats.jobs_completed > 0) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    Check(in_flight, "child calculation fixture reaches an unfinished ancestor scan");
    const auto before = sizes.ReadStats();
    File(child / L"value.bin", 59);
    sizes.Calculate(child.wstring());
    Check(Wait([&] { return sizes.ReadStats().jobs_cancelled > before.jobs_cancelled; }),
          "manual child calculation cancels its in-flight ancestor epoch");
    Check(Wait([&] { return Exact(sizes, parent, 64 * 128 * 13 + 27 + 59) && Exact(sizes, child, 59); }),
          "ancestor and child publish new totals without an old parent result overwriting the child");
    Check(!sizes.Get(parent.wstring()).verified && !sizes.Get(child.wstring()).verified,
          "manual no-watch calculation remains a snapshot without reusable verification");
    sizes.Stop();
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--serve-index")
        return pulse::app::RunFolderSizeFakeIndexServer(argv[2]);
    Watchdog watchdog;
    const auto root = fs::current_path() / L"bench_data" /
        (L"folder_sizes_refactor_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--watch-only") {
            WatchedChanges(root);
            std::wprintf(L"[INFO] retained isolated watch fixtures: %ls\n", root.c_str());
            std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
            return failures ? 1 : 0;
        }
        fs::create_directories(root / L"nested" / L"child");
        fs::create_directories(root / L"empty");
        File(root / L"nested" / L"a.bin", 1234);
        File(root / L"nested" / L"child" / L"b.bin", 12333);
        std::ofstream index_log(root / L"index-results.log");
        Check(pulse::app::RunFolderSizeIndexClientTest(root, index_log), "real isolated index protocol regression group");
        index_log.close();
        WatchedChanges(root);
        PartialAndPersistence(root);
        Cancellation(root);
        ChildCalculationCancelsAncestor(root);
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    std::wprintf(L"[INFO] retained isolated fixtures and index assertion log: %ls\n", root.c_str());
    std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
