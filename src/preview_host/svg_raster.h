// System Direct2D SVG rasterization for the isolated preview host.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {
// Rejects markup outside the documented Direct2D subset before silent omission.
// XmlLite is loaded dynamically; UTF-8/UTF-16 are checked without reading external resources.
bool IsSvgSupportedForDirect2D(const std::vector<unsigned char>& bytes, std::wstring& error);
// Returns premultiplied BGRA. Unsupported text, styles, filters, external resources
// and animation fail explicitly so callers can preserve the source and reason.
// Native SVG rendering requires ID2D1DeviceContext5 (Windows 10 1703 or newer).
bool RasterizeSvgFile(const std::wstring& path, UINT max_edge,
    std::vector<unsigned char>& pixels, UINT& width, UINT& height, UINT& stride,
    UINT& source_width, UINT& source_height, std::wstring* error);
} // namespace pulse::preview
