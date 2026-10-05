// pdf_raster.h — PDFium rasterization of PDF / PDF-compatible AI files for the
// preview host.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::preview {

// Renders one page (page_index, clamped; 0 = first) of a PDF document (Illustrator files saved with PDF
// compatibility are PDF documents too) into opaque 32bpp BGRA pixels on a white
// page, matching what the WIC path returns. max_edge caps the longest edge of
// the result; source_width/source_height report the page size at 96 DPI.
// pdfium.dll is loaded on first use from the host executable's directory, so
// preview host start-up for every other format pays nothing. Returns false and
// fills *error when the DLL is missing, the file is not a PDF (legacy
// PostScript-based AI), password protected, damaged or over the size budget.
// thumbnail enables a cooperative 2-second/128-MiB process-private budget.
// Budget failure returns no pixels/page count and error "pdf-thumbnail-budget".
bool RasterizePdfFile(const std::wstring& path, UINT max_edge,
                      std::vector<unsigned char>& pixels,
                      UINT& width, UINT& height, UINT& stride,
                      UINT& source_width, UINT& source_height,
                      std::wstring* error, UINT page_index = 0,
                      UINT* page_count = nullptr, bool thumbnail = false);

} // namespace pulse::preview
