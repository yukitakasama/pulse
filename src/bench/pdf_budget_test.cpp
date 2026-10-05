#include "../preview_host/pdf_raster.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Raster {
    std::vector<unsigned char> pixels;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0, pages = 0;
    std::wstring error;
};
}

int wmain(int argc, wchar_t** argv) {
    const auto directory = std::filesystem::path(L"bench_data") /
        (L"pdf-budget-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto path = directory / L"two-pages.pdf";
    const std::string red = "1 0 0 rg 20 20 160 60 re f\n";
    const std::string blue = "0 0 1 rg 20 20 160 60 re f\n";
    const std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R >>",
        "<< /Length " + std::to_string(red.size()) + " >>\nstream\n" + red + "endstream",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 6 0 R >>",
        "<< /Length " + std::to_string(blue.size()) + " >>\nstream\n" + blue + "endstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 7\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
        char line[32]; std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    { std::ofstream file(path, std::ios::binary); file << pdf; Check(file.good(), "write isolated two-page fixture"); }
    if (argc == 2) {
        Raster rejected;
        const bool ok = pulse::preview::RasterizePdfFile(argv[1], 256, rejected.pixels,
            rejected.width, rejected.height, rejected.stride, rejected.source_width,
            rejected.source_height, &rejected.error, 0, &rejected.pages, true);
        Check(!ok && rejected.error == L"pdf-thumbnail-budget", "complex thumbnail reports budget failure");
        Check(rejected.pixels.empty() && rejected.width == 0 && rejected.height == 0 &&
            rejected.stride == 0 && rejected.source_width == 0 && rejected.source_height == 0 &&
            rejected.pages == 0, "cancelled thumbnail publishes no partial raster or page count");
    }
    for (UINT page = 0; page < 2; ++page) {
        Raster full, grid;
        auto render = [&](Raster& r, bool thumbnail) {
            return pulse::preview::RasterizePdfFile(path.wstring(), 256, r.pixels,
                r.width, r.height, r.stride, r.source_width, r.source_height,
                &r.error, page, &r.pages, thumbnail);
        };
        Check(render(full, false), "full page renders");
        Check(render(grid, true), "budgeted thumbnail renders");
        Check(grid.pages == 2 && full.pages == 2 && grid.width == 256 && grid.height == 128,
            "thumbnail retains dimensions and actual page count");
        Check(!grid.pixels.empty() && grid.pixels == full.pixels,
            "progressive thumbnail matches synchronous pixels");
        const auto center = static_cast<size_t>(64) * grid.stride + 128 * 4;
        Check(center + 3 < grid.pixels.size() && grid.pixels[center + (page ? 0 : 2)] > 240 &&
            grid.pixels[center + 1] < 20, "requested page has expected color");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
    return failures ? 1 : 0;
}
