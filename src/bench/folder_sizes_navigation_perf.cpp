#ifdef PULSE_FOLDER_SIZE_REFACTOR_BASELINE
#include "../../bench_data/folder_size_refactor_baseline/folder_sizes.h"
#else
#include "../app/folder_sizes.h"
#endif
#include "../fs/fs_enum.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <string>

namespace {
using State = pulse::app::FolderSizeState;
bool WaitReady(pulse::app::FolderSizes& sizes, const std::wstring& path) {
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        const auto value = sizes.Get(path);
        if (value.state == State::Ready || value.state == State::Partial) return value.has_value;
        if (value.state == State::Unavailable) return false;
        Sleep(1);
    }
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const auto root = pulse::fs::NormalizePath(argv[1]);
    const auto child = pulse::fs::NormalizePath(argv[2]);
    if (!child.starts_with(root + L"\\") || pulse::fs::IsUncPath(root)) return 2;
    pulse::app::FolderSizes sizes;
    sizes.SetIndexEnabled(false);
    int failures = 0;
    uint64_t original = 0;
    const auto measure = [&](const char* label, const std::wstring& path) {
#ifndef PULSE_FOLDER_SIZE_REFACTOR_BASELINE
        const auto before = sizes.ReadStats();
#endif
        const auto start = std::chrono::steady_clock::now();
        sizes.Sync({{path}}, {path});
        const bool ready = WaitReady(sizes, path);
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto value = sizes.Get(path);
        std::printf("[NAV] %s ready=%d bytes=%llu partial=%d elapsed_ms=%.3f\n", label, ready,
            static_cast<unsigned long long>(value.bytes), value.partial, elapsed);
#ifndef PULSE_FOLDER_SIZE_REFACTOR_BASELINE
        const auto after = sizes.ReadStats();
        std::printf("[WORK] %s entries=%llu subtree_hits=%llu jobs=%llu verified=%d\n", label,
            static_cast<unsigned long long>(after.entries_scanned - before.entries_scanned),
            static_cast<unsigned long long>(after.subtree_hits - before.subtree_hits),
            static_cast<unsigned long long>(after.jobs_started - before.jobs_started), value.verified);
#endif
        if (!ready) ++failures;
        return value.bytes;
    };
    original = measure("cold-parent", root);
    const auto child_bytes = measure("enter-child", child);
    const auto returned = measure("return-parent", root);
    // Give the coordinator time to process the latest scope before checking
    // stability; a cached value must not be a one-frame false success.
    Sleep(350);
    const bool stable = WaitReady(sizes, root) && sizes.Get(root).bytes == original;
    std::printf("[%s] parent total survives navigation; child_bytes=%llu\n", stable ? "PASS" : "FAIL",
        static_cast<unsigned long long>(child_bytes));
    if (!stable) ++failures;
    sizes.Stop();
    pulse::app::FolderSizes reference;
    reference.SetIndexEnabled(false);
    reference.Sync({{root}, {child}}, {});
    const bool agrees = WaitReady(reference, root) && WaitReady(reference, child) &&
        reference.Get(root).bytes == original && reference.Get(child).bytes == child_bytes && returned == original;
    std::printf("[%s] reused totals equal fresh independent worker scans\n", agrees ? "PASS" : "FAIL");
    reference.Stop();
    return failures || !agrees ? 1 : 0;
}
