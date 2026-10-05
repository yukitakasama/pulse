#pragma once
#include "archive_listing.h"
#include <string_view>

namespace pulse::preview {
bool IsArchivePackExtension(std::wstring_view extension);
bool ArchivePackAvailable();
bool ArchivePackListing(const std::wstring& path, std::wstring& text,
    uint32_t& bytes_read, std::wstring* error);

// Kept separate for deterministic parser regression tests. Nonzero exit status
// and truncated output are never accepted as a complete listing.
bool ParseArchivePackListing(std::string_view output, DWORD exit_code,
    std::wstring& text, std::wstring* error);
// Explicit tool path for validation and isolated integration tests.
bool ArchivePackListingWithTool(const std::wstring& tool, const std::wstring& path,
    std::wstring& text, uint32_t& bytes_read, std::wstring* error);
}
