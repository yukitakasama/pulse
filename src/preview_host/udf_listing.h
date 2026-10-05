#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {
struct UdfEntry {
    std::wstring path;
    uint64_t size = 0;
    SYSTEMTIME time{};
    bool has_time = false;
    bool directory = false;
};
struct UdfLimits {
    uint64_t max_bytes = 64ull * 1024 * 1024;
    size_t max_entries = 100000;
    uint32_t max_depth = 64;
    uint64_t max_ms = 1200;
    const std::atomic_bool* cancelled = nullptr;
};
struct UdfListing {
    std::vector<UdfEntry> entries;
    uint64_t bytes_read = 0;
    bool incomplete = false;
    // Stable reasons: udf-invalid, udf-unsupported-partition/allocation/volume,
    // udf-limit, udf-cancelled. Nonempty on failure or partial traversal.
    std::wstring reason;
};
// Reads ECMA-167/UDF directory metadata only. Physical and metadata partition
// maps, FE/EFE, short/long/embedded ADs; unsupported layouts are never complete.
// file remains owned by caller. Limits are checked before every physical read.
bool ReadUdfListing(HANDLE file, uint64_t file_size, UdfListing& out,
                    const UdfLimits& limits = {});
} // namespace pulse::preview
