#pragma once
#include "image_pack.h"

namespace pulse::preview {
bool RawPackAvailable();
// Embedded previews only unless explicitly requested for a large image view.
bool RawPackDecode(const std::wstring& path, UINT cap, bool allow_full_decode, ImagePackFrame& frame);
bool RawPackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows, size_t max_rows);
} // namespace pulse::preview
