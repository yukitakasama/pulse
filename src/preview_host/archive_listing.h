// archive_listing.h — archive contents payload for the preview host.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

struct ArchiveListingEntry {
    std::wstring path;
    uint64_t size = 0;
    uint64_t packed = 0;
    SYSTEMTIME time{};
    bool has_time = false;
    bool dir = false;
    bool encrypted = false;
};

// Shared bounded tree serialization for native readers and optional tools.
std::wstring SerializeArchiveEntries(std::vector<ArchiveListingEntry> entries,
    const std::wstring& format, bool incomplete);

// Builds the folder tree of the archive's contents as the payload of
// ipc::PreviewContentKind::Archive (drawn by ui/archive_preview.cpp):
//   PULSEARC \t 1 \t FORMAT \t packed_total|- \t incomplete(0|1)
// then one pre-order row per node, folders first in natural order:
//   depth \t flags(d folder, e encrypted, - none) \t size \t packed|- \t
//   YYYY-MM-DD HH:MM|- \t name
// Folder sizes are totals; a folder without its own record takes the newest
// time inside. Rows are capped to stay under ipc::kPreviewMaxArchiveChars.
// ZIP is read from its central directory here (fast for any size, and decodes
// the legacy code-page names Chinese archivers write); 7z / RAR / tar and the
// compressed tar family and single-file gzip/xz/bzip2 streams go through the system's libarchive (archiveint.dll,
// Windows 10 1803+), loaded on demand from System32 only. bytes_read reports
// how much of the file was consumed. UDF physical/metadata partitions use the
// bounded native reader; ISO/UDF means an incomplete ISO compatibility fallback.
// udf-* errors distinguish invalid, unsupported, limited and cancelled reads. Raw
// streams are counted within 64 MiB / 1.2 s; unsupported, damaged, or oversized
// streams return a compressed-stream-* error instead of a false TAR listing.
// Returns false when the file is not a
// readable archive so the caller keeps its previous fallback.
bool MakeArchiveListing(const std::wstring& path, std::wstring& text,
                        uint32_t& bytes_read, std::wstring* error);

} // namespace pulse::preview
