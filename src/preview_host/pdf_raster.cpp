// pdf_raster.cpp — see pdf_raster.h.
#include "pdf_raster.h"
#include "../../third_party/pdfium/include/fpdfview.h"
#include "../../third_party/pdfium/include/fpdf_progressive.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <psapi.h>
#include <cstdio>

namespace pulse::preview {
namespace {

struct PdfTrace {
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER start{}, frequency{};
    PdfTrace() {
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_PDF_TRACE", path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return;
        file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        Log("request_begin");
    }
    ~PdfTrace() {
        Log("request_released");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void Log(const char* stage) const {
        if (file == INVALID_HANDLE_VALUE || !frequency.QuadPart) return;
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        const BOOL sampled = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
        char line[224];
        const int count = std::snprintf(line, sizeof(line),
            "pid=%lu request=%lld elapsed_ms=%.3f stage=%s private=%llu working=%llu sampled=%d\r\n",
            GetCurrentProcessId(), start.QuadPart, elapsed, stage,
            static_cast<unsigned long long>(memory.PrivateUsage),
            static_cast<unsigned long long>(memory.WorkingSetSize), sampled != FALSE);
        if (count > 0 && count < static_cast<int>(sizeof(line))) {
            DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
            FlushFileBuffers(file);
        }
    }
};

// The preview host serves requests on one thread, so the library is set up
// once on first use and kept for the life of the process.
struct PdfApi {
    HMODULE module = nullptr;
#define PDF_FN(name) decltype(&name) name##_fn = nullptr
    PDF_FN(FPDF_InitLibraryWithConfig); PDF_FN(FPDF_LoadCustomDocument);
    PDF_FN(FPDF_CloseDocument); PDF_FN(FPDF_GetLastError);
    PDF_FN(FPDF_GetPageCount); PDF_FN(FPDF_LoadPage); PDF_FN(FPDF_ClosePage);
    PDF_FN(FPDF_GetPageWidthF); PDF_FN(FPDF_GetPageHeightF);
    PDF_FN(FPDF_RenderPageBitmap); PDF_FN(FPDFBitmap_CreateEx);
    PDF_FN(FPDF_RenderPageBitmap_Start); PDF_FN(FPDF_RenderPage_Continue); PDF_FN(FPDF_RenderPage_Close);
    PDF_FN(FPDFBitmap_FillRect); PDF_FN(FPDFBitmap_GetBuffer);
    PDF_FN(FPDFBitmap_GetStride); PDF_FN(FPDFBitmap_Destroy);
#undef PDF_FN
    bool ready = false;

    PdfApi() {
        wchar_t exe[32768]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        if (!n || n >= ARRAYSIZE(exe)) return;
        std::wstring dll(exe, n);
        const size_t slash = dll.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        dll.resize(slash + 1);
        dll += L"pdfium.dll";
        module = LoadLibraryExW(dll.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
#define PDF_LOAD(name) name##_fn = reinterpret_cast<decltype(name##_fn)>(GetProcAddress(module, #name)); if (!name##_fn) return
        PDF_LOAD(FPDF_InitLibraryWithConfig); PDF_LOAD(FPDF_LoadCustomDocument);
        PDF_LOAD(FPDF_CloseDocument); PDF_LOAD(FPDF_GetLastError);
        PDF_LOAD(FPDF_GetPageCount); PDF_LOAD(FPDF_LoadPage); PDF_LOAD(FPDF_ClosePage);
        PDF_LOAD(FPDF_GetPageWidthF); PDF_LOAD(FPDF_GetPageHeightF);
        PDF_LOAD(FPDF_RenderPageBitmap); PDF_LOAD(FPDFBitmap_CreateEx);
        PDF_LOAD(FPDF_RenderPageBitmap_Start); PDF_LOAD(FPDF_RenderPage_Continue); PDF_LOAD(FPDF_RenderPage_Close);
        PDF_LOAD(FPDFBitmap_FillRect); PDF_LOAD(FPDFBitmap_GetBuffer);
        PDF_LOAD(FPDFBitmap_GetStride); PDF_LOAD(FPDFBitmap_Destroy);
#undef PDF_LOAD
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        FPDF_InitLibraryWithConfig_fn(&config);
        ready = true;
    }
};

PdfApi& Api() {
    static PdfApi api;
    return api;
}

struct ThumbnailBudget {
    bool enabled = false, exceeded = false;
    ULONGLONG deadline = GetTickCount64() + 2000, next_memory_check = 0;
    bool Exceeded(bool sample_now = false) {
        if (!enabled) return false;
        if (exceeded) return true;
        const auto now = GetTickCount64();
        if (now >= deadline) return exceeded = true;
        if (sample_now || now >= next_memory_check) {
            next_memory_check = now + 16;
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) &&
                memory.PrivateUsage >= 128ull * 1024 * 1024) return exceeded = true;
        }
        return false;
    }
    static FPDF_BOOL Pause(IFSDK_PAUSE* pause) {
        return static_cast<ThumbnailBudget*>(pause->user)->Exceeded() ? 1 : 0;
    }
};

// Buffered random-access reader: PDFium issues many small reads while it parses
// the cross-reference table and the first page's objects.
struct Reader {
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned long size = 0;
    std::array<unsigned char, 64 * 1024> cache{};
    unsigned long offset = 0;
    DWORD available = 0;
    ThumbnailBudget* budget = nullptr;

    static int Read(void* context, unsigned long position, unsigned char* data,
                    unsigned long count) {
        auto& self = *static_cast<Reader*>(context);
        if (self.budget && self.budget->Exceeded()) return 0;
        if (position > self.size || count > self.size - position) return 0;
        while (count) {
            if (self.budget && self.budget->Exceeded()) return 0;
            if (position < self.offset || position >= self.offset + self.available) {
                self.offset = position;
                self.available = 0;
                LARGE_INTEGER at{};
                at.QuadPart = position;
                if (!SetFilePointerEx(self.file, at, nullptr, FILE_BEGIN)) return 0;
                const DWORD want = static_cast<DWORD>((std::min)(
                    static_cast<unsigned long>(self.cache.size()), self.size - position));
                if (!ReadFile(self.file, self.cache.data(), want, &self.available, nullptr) ||
                    !self.available)
                    return 0;
            }
            const DWORD from = position - self.offset;
            const DWORD n = (std::min)(static_cast<DWORD>(count), self.available - from);
            memcpy(data, self.cache.data() + from, n);
            count -= n;
            position += n;
            data += n;
        }
        return 1;
    }
};

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

// Preview budget: very large sets (plotted drawing sets, scanned books) still
// open quickly because PDFium reads lazily, but the 32-bit FPDF_FILEACCESS
// length caps what it can address.
constexpr ULONGLONG kMaxPdfBytes = 1024ull * 1024 * 1024;
constexpr UINT kMaxRenderEdge = 4096;

} // namespace

bool RasterizePdfFile(const std::wstring& path, UINT max_edge,
                      std::vector<unsigned char>& pixels,
                      UINT& width, UINT& height, UINT& stride,
                      UINT& source_width, UINT& source_height,
                      std::wstring* error, UINT page_index, UINT* page_count, bool thumbnail) {
    ThumbnailBudget budget; budget.enabled = thumbnail;
    PdfTrace trace;
    if (page_count) *page_count = 0;
    pixels.clear();
    width = height = stride = source_width = source_height = 0;
    const auto budget_failed = [&]() {
        if (!budget.Exceeded(true)) return false;
        trace.Log("budget_exceeded");
        pixels.clear(); width = height = stride = source_width = source_height = 0;
        if (page_count) *page_count = 0;
        SetError(error, L"pdf-thumbnail-budget");
        return true;
    };
    trace.Log("library_begin");
    PdfApi& api = Api();
    trace.Log("library_end");
    if (budget_failed()) return false;
    if (!api.ready) {
        SetError(error, L"pdfium-unavailable");
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    struct FileOwner { HANDLE h; ~FileOwner() { CloseHandle(h); } } file_owner{file};
    LARGE_INTEGER bytes{};
    if (!GetFileSizeEx(file, &bytes) || bytes.QuadPart < 8) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    if (static_cast<ULONGLONG>(bytes.QuadPart) > kMaxPdfBytes) {
        SetError(error, L"pdf-too-large");
        return false;
    }

    // Legacy Illustrator files (and EPS renamed to .ai) are PostScript; PDFium
    // would only report a format error after scanning the whole file.
    char head[1024]{};
    DWORD got = 0;
    if (!ReadFile(file, head, sizeof(head), &got, nullptr) || got < 5) {
        SetError(error, L"pdf-open-failed");
        return false;
    }
    bool has_marker = false;
    for (DWORD i = 0; i + 5 <= got; ++i) {
        if (memcmp(head + i, "%PDF-", 5) == 0) { has_marker = true; break; }
    }
    if (!has_marker) {
        SetError(error, L"pdf-not-pdf");
        return false;
    }

    auto reader = std::make_unique<Reader>();
    reader->file = file;
    reader->size = static_cast<unsigned long>(bytes.QuadPart);
    reader->budget = &budget;
    FPDF_FILEACCESS access{reader->size, Reader::Read, reader.get()};
    trace.Log("document_load_begin");
    FPDF_DOCUMENT doc = api.FPDF_LoadCustomDocument_fn(&access, nullptr);
    trace.Log("document_load_end");
    struct DocOwner {
        PdfApi& api; FPDF_DOCUMENT d; PdfTrace& trace;
        ~DocOwner() { if (d) api.FPDF_CloseDocument_fn(d); trace.Log("document_released"); }
    } doc_owner{api, doc, trace};
    if (budget_failed()) return false;
    if (!doc) {
        SetError(error, api.FPDF_GetLastError_fn() == FPDF_ERR_PASSWORD
            ? L"pdf-password" : L"pdf-load-failed");
        return false;
    }
    trace.Log("page_count_begin");
    const int pages = api.FPDF_GetPageCount_fn(doc);
    trace.Log("page_count_end");
    if (budget_failed()) return false;
    if (pages <= 0) {
        SetError(error, L"pdf-empty");
        return false;
    }
    const int index = static_cast<int>((std::min)(page_index, static_cast<UINT>(pages - 1)));
    trace.Log("page_load_begin");
    FPDF_PAGE page = api.FPDF_LoadPage_fn(doc, index);
    trace.Log("page_load_end");
    struct PageOwner {
        PdfApi& api; FPDF_PAGE p; PdfTrace& trace;
        ~PageOwner() { if (p) api.FPDF_ClosePage_fn(p); trace.Log("page_released"); }
    } page_owner{api, page, trace};
    if (budget_failed()) return false;
    if (!page) {
        SetError(error, L"pdf-page-failed");
        return false;
    }

    // Page size in points (1/72 in), already accounting for /Rotate.
    const float page_w = api.FPDF_GetPageWidthF_fn(page);
    const float page_h = api.FPDF_GetPageHeightF_fn(page);
    if (!(page_w > 0.5f) || !(page_h > 0.5f) || !std::isfinite(page_w) || !std::isfinite(page_h)) {
        SetError(error, L"pdf-page-failed");
        return false;
    }
    source_width = static_cast<UINT>(std::lround(page_w * 96.0f / 72.0f));
    source_height = static_cast<UINT>(std::lround(page_h * 96.0f / 72.0f));

    const UINT edge = (std::clamp)(max_edge ? max_edge : 256u, 16u, kMaxRenderEdge);
    const float scale = static_cast<float>(edge) / (std::max)(page_w, page_h);
    const int bw = (std::max)(1, static_cast<int>(std::lround(page_w * scale)));
    const int bh = (std::max)(1, static_cast<int>(std::lround(page_h * scale)));

    trace.Log("bitmap_create_begin");
    FPDF_BITMAP bitmap = api.FPDFBitmap_CreateEx_fn(bw, bh, FPDFBitmap_BGRA, nullptr, 0);
    trace.Log("bitmap_create_end");
    if (!bitmap) {
        SetError(error, L"pdf-render-failed");
        return false;
    }
    struct BitmapOwner {
        PdfApi& api; FPDF_BITMAP b; PdfTrace& trace;
        ~BitmapOwner() { api.FPDFBitmap_Destroy_fn(b); trace.Log("bitmap_released"); }
    } bitmap_owner{api, bitmap, trace};
    if (budget_failed()) return false;
    // Paper is white regardless of theme; PDF content assumes it. Every pixel
    // stays opaque, so straight BGRA equals the premultiplied form WIC returns.
    api.FPDFBitmap_FillRect_fn(bitmap, 0, 0, bw, bh, 0xFFFFFFFF);
    trace.Log("render_begin");
    if (thumbnail) {
        IFSDK_PAUSE pause{1, ThumbnailBudget::Pause, &budget};
        struct RenderOwner {
            PdfApi& api; FPDF_PAGE page; PdfTrace& trace;
            ~RenderOwner() { api.FPDF_RenderPage_Close_fn(page); trace.Log("render_released"); }
        } render_owner{api, page, trace};
        int status = api.FPDF_RenderPageBitmap_Start_fn(bitmap, page, 0, 0, bw, bh, 0,
            FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE, &pause);
        while (status == FPDF_RENDER_TOBECONTINUED) {
            if (budget_failed()) return false;
            status = api.FPDF_RenderPage_Continue_fn(page, &pause);
        }
        if (budget_failed()) return false;
        if (status != FPDF_RENDER_DONE) {
            SetError(error, L"pdf-render-failed"); return false;
        }
    } else {
        api.FPDF_RenderPageBitmap_fn(bitmap, page, 0, 0, bw, bh, 0, FPDF_ANNOT);
    }
    trace.Log("render_end");

    const auto* buffer = static_cast<const unsigned char*>(api.FPDFBitmap_GetBuffer_fn(bitmap));
    const int src_stride = api.FPDFBitmap_GetStride_fn(bitmap);
    if (!buffer || src_stride < bw * 4) {
        SetError(error, L"pdf-render-failed");
        return false;
    }
    width = static_cast<UINT>(bw);
    height = static_cast<UINT>(bh);
    stride = width * 4;
    pixels.resize(static_cast<size_t>(stride) * height);
    for (UINT y = 0; y < height; ++y) {
        const unsigned char* src = buffer + static_cast<size_t>(src_stride) * y;
        unsigned char* dst = pixels.data() + static_cast<size_t>(stride) * y;
        memcpy(dst, src, stride);
        for (UINT x = 0; x < width; ++x) dst[x * 4 + 3] = 0xFF;
    }
    if (budget_failed()) return false;
    if (page_count) *page_count = static_cast<UINT>(pages);
    return true;
}

} // namespace pulse::preview
