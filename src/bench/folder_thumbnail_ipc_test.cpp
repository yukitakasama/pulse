#include "preview_host_client.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace ipc = pulse::ipc;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!ok) ++failures;
}
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Cancel only this test's blocking pipe call. Host owns and tears down only the
// child it created; no process-name termination can touch a user's host.
class Watchdog {
public:
    Watchdog() {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &main_thread_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        wchar_t value[32]{};
        DWORD seconds = 120;
        if (GetEnvironmentVariableW(L"PULSE_FOLDER_TEST_TIMEOUT_SECONDS", value, ARRAYSIZE(value))) {
            const auto requested = wcstoul(value, nullptr, 10);
            if (requested >= 5 && requested <= 3600) seconds = requested;
        }
        worker_ = std::thread([this, seconds] {
            std::unique_lock lock(mutex_);
            if (condition_.wait_for(lock, std::chrono::seconds(seconds), [this] { return done_; })) return;
            std::fprintf(stderr, "[FAIL] folder thumbnail whole-test watchdog expired\n");
            CancelSynchronousIo(main_thread_);
            if (!condition_.wait_for(lock, std::chrono::seconds(2), [this] { return done_; }))
                TerminateProcess(GetCurrentProcess(), 124);
        });
    }
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        condition_.notify_one();
        worker_.join();
        CloseHandle(main_thread_);
    }
private:
    HANDLE main_thread_ = nullptr;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    std::thread worker_;
};

bool WritePng(const fs::path& path, UINT width, UINT height, UINT stride, const Bytes& pixels,
              bool premultiplied = false) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) return false;
    Bytes straight = pixels;
    if (premultiplied) {
        for (size_t i = 0; i + 3 < straight.size(); i += 4) {
            const unsigned alpha = straight[i + 3];
            if (alpha > 0 && alpha < 255) {
                for (size_t channel = 0; channel < 3; ++channel)
                    straight[i + channel] = static_cast<unsigned char>(
                        (std::min)(255u, (static_cast<unsigned>(straight[i + channel]) * 255u + alpha / 2u) / alpha));
            }
        }
    }
    return SUCCEEDED(frame->WritePixels(height, stride, static_cast<UINT>(straight.size()), straight.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool Image(const fs::path& path, unsigned seed) {
    constexpr UINT width = 120, height = 80;
    Bytes pixels(width * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            pixels[at] = static_cast<unsigned char>((x * 2 + seed * 71) % 256);
            pixels[at + 1] = static_cast<unsigned char>((y * 3 + seed * 43) % 256);
            pixels[at + 2] = static_cast<unsigned char>((x + y + seed * 97) % 256);
            pixels[at + 3] = 255;
        }
    }
    return WritePng(path, width, height, width * 4, pixels);
}
bool Bitmap(const pulse_test::Result& result) {
    return result.response.kind == ipc::PreviewContentKind::Bitmap &&
           result.response.width == 256 && result.response.height == 256 &&
           result.response.stride == 256 * 4 && result.pixels.size() == 256 * 256 * 4;
}
bool TransparentArtwork(const pulse_test::Result& result) {
    if (!Bitmap(result)) return false;
    size_t transparent = 0, visible = 0;
    for (size_t i = 3; i < result.pixels.size(); i += 4) {
        if (result.pixels[i] == 0) ++transparent;
        if (result.pixels[i] != 0) ++visible;
    }
    return transparent > 100 && visible > 100;
}
bool Request(pulse_test::Host& host, const fs::path& path, pulse_test::Result& result,
             uint32_t extra_flags = 0, DWORD attrs = MAXDWORD) {
    return host.Request(path.wstring(), result, attrs, 256, ipc::PreviewRequestKind::Content,
                        ipc::kPreviewRequestFlagFolderThumbnail | ipc::kPreviewRequestFlagGrid | extra_flags);
}
int PathEquivalence(const fs::path& requested) {
    fs::path normal = requested;
    if (normal.empty()) {
        normal = fs::current_path() / L"bench_data" /
            (L"folder_thumbnail_paths_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
             std::to_wstring(GetTickCount64()));
        fs::create_directories(normal);
        Check(Image(normal / L"0.png", 1) && Image(normal / L"1.png", 2) &&
              Image(normal / L"2.png", 3), "generate extended-path equivalence fixture");
    }
    normal = fs::absolute(normal);
    std::wstring normal_text = normal.wstring();
    if (normal_text.starts_with(L"\\\\?\\")) normal_text.erase(0, 4);
    const fs::path extended = std::wstring(L"\\\\?\\") + normal_text;
    normal = normal_text;
    std::wprintf(L"[INFO] normal: %ls\n[INFO] extended: %ls\n", normal.c_str(), extended.c_str());
    pulse_test::Host host;
    Check(host.Start(), "start dedicated host for path equivalence");
    pulse_test::Result plain, prefixed, forced;
    Check(Request(host, normal, plain) && TransparentArtwork(plain), "ordinary path returns folder artwork");
    Check(Request(host, extended, prefixed) && TransparentArtwork(prefixed), "extended path returns folder artwork");
    Check(Bitmap(plain) && Bitmap(prefixed) && plain.pixels == prefixed.pixels,
          "normal and NormalizePath-style extended paths produce equal pixels");
    Check(Request(host, extended, forced, ipc::kPreviewRequestFlagFolderRefresh) && Bitmap(forced) &&
          forced.pixels == plain.pixels, "extended path forced refresh preserves equivalent artwork");
    std::printf("[SUMMARY] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
}

int wmain(int argc, wchar_t** argv) {
    Watchdog watchdog;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    if (argc >= 2 && std::wstring(argv[1]) == L"--path-equivalence-only") {
        int code = 1;
        try { code = PathEquivalence(argc >= 3 ? fs::path(argv[2]) : fs::path()); }
        catch (const std::exception& error) { std::fprintf(stderr, "[FAIL] %s\n", error.what()); }
        CoUninitialize();
        return code;
    }
    const fs::path root = fs::current_path() / L"bench_data" /
        (L"folder_thumbnail_ipc_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    try {
        for (const auto* name : {L"empty", L"one", L"two", L"three", L"documents", L"corrupt",
                                 L"many", L"nested", L"excluded", L"mutation", L"renders"})
            fs::create_directories(root / name);
        fs::create_directories(root / L"nested" / L"child");
        bool fixtures = true;
        for (unsigned count = 1; count <= 3; ++count) {
            const wchar_t* name = count == 1 ? L"one" : count == 2 ? L"two" : L"three";
            for (unsigned i = 0; i < count; ++i)
                fixtures = Image(root / name / (std::to_wstring(i) + L".png"), i + 1) && fixtures;
        }
        fixtures = Image(root / L"nested" / L"child" / L"image.png", 1) && fixtures;
        fixtures = Image(root / L"corrupt" / L"1.png", 1) && fixtures;
        std::ofstream(root / L"corrupt" / L"0.png", std::ios::binary) << "not an image";
        for (const auto* name : {L"report.pdf", L"slides.pptx", L"notes.docx"})
            std::ofstream(root / L"documents" / name) << "type-card fixture; intentionally not a valid document";
        for (unsigned i = 0; i < 240; ++i)
            fs::copy_file(root / L"one" / L"0.png", root / L"many" / (std::to_wstring(i) + L".png"));
        fixtures = Image(root / L"excluded" / L"hidden.png", 2) && fixtures;
        fixtures = Image(root / L"excluded" / L"offline.png", 3) && fixtures;
        Check(SetFileAttributesW((root / L"excluded" / L"hidden.png").c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE,
              "hidden candidate fixture");
        Check(SetFileAttributesW((root / L"excluded" / L"offline.png").c_str(), FILE_ATTRIBUTE_OFFLINE) != FALSE,
              "offline candidate fixture");
        fixtures = Image(root / L"mutation" / L"image.png", 1) && fixtures;
        Check(fixtures, "generated isolated image fixtures");
        pulse_test::Host host;
        auto start = Clock::now();
        const bool started = host.Start();
        std::printf("[BENCH] host startup %.3f ms\n", Milliseconds(start));
        Check(started, "start dedicated preview host");
        if (!started || !fixtures) { CoUninitialize(); return 2; }
        for (const auto* name : {L"empty", L"nested", L"excluded"}) {
            pulse_test::Result result;
            const bool ok = Request(host, root / name, result);
            Check(ok && result.response.kind == ipc::PreviewContentKind::Unsupported,
                  name == std::wstring(L"empty") ? "empty folder keeps ordinary folder icon" :
                  name == std::wstring(L"nested") ? "folder artwork does not recurse" : "hidden and offline candidates skipped");
        }
        Bytes previous;
        for (const auto* name : {L"one", L"two", L"three", L"documents", L"corrupt", L"many"}) {
            pulse_test::Result cold;
            start = Clock::now();
            const bool ok = Request(host, root / name, cold);
            std::wprintf(L"[BENCH] %ls first request 256px %.3f ms\n", name, Milliseconds(start));
            Check(ok && TransparentArtwork(cold), "populated folder returns transparent 256px artwork");
            if (Bitmap(cold)) {
                Check(WritePng(root / L"renders" / (std::wstring(name) + L".png"), 256, 256, cold.response.stride,
                               cold.pixels, true), "save actual host artwork PNG");
                if (name == std::wstring(L"two") || name == std::wstring(L"three"))
                    Check(previous != cold.pixels, "adding a card changes composed artwork");
                previous = cold.pixels;
            }
            std::vector<double> timings;
            bool stable = true;
            for (unsigned repeat = 0; repeat < 15; ++repeat) {
                pulse_test::Result warm;
                start = Clock::now();
                stable = Request(host, root / name, warm) && warm.response.kind == cold.response.kind &&
                         warm.pixels == cold.pixels && stable;
                timings.push_back(Milliseconds(start));
            }
            std::sort(timings.begin(), timings.end());
            std::wprintf(L"[BENCH] %ls warm n=15 median %.3f ms p95 %.3f ms\n", name, timings[7], timings[14]);
            Check(stable, "repeated requests preserve pixels and IPC framing");
        }
        pulse_test::Result fan, single;
        Check(Request(host, root / L"three", fan) &&
              Request(host, root / L"three", single, ipc::kPreviewRequestFlagFolderSingle) &&
              Bitmap(single) && single.pixels != fan.pixels, "single-card mode has distinct cache identity and artwork");
        if (Bitmap(single)) Check(WritePng(root / L"renders" / L"single.png", 256, 256, single.response.stride,
                                          single.pixels, true), "save single-card host artwork");
        pulse_test::Result before, after;
        Check(Request(host, root / L"mutation", before) && Bitmap(before), "initial mutable folder artwork");
        Check(Image(root / L"mutation" / L"image.png", 8), "replace child image content");
        auto modified = fs::last_write_time(root / L"mutation" / L"image.png");
        fs::last_write_time(root / L"mutation" / L"image.png", modified + std::chrono::seconds(3));
        Check(Request(host, root / L"mutation", after) && Bitmap(after) && after.pixels != before.pixels,
              "changed child content and mtime automatically invalidate cached artwork");
        const auto unchanged_mtime = fs::last_write_time(root / L"mutation" / L"image.png");
        Check(Image(root / L"mutation" / L"image.png", 11), "replace image for forced refresh");
        fs::last_write_time(root / L"mutation" / L"image.png", unchanged_mtime);
        pulse_test::Result refreshed;
        Check(Request(host, root / L"mutation", refreshed, ipc::kPreviewRequestFlagFolderRefresh) &&
              Bitmap(refreshed) && refreshed.pixels != after.pixels,
              "F5 refresh observes changed child content even with preserved mtime");
        pulse_test::Result missing, recovered;
        Check(Request(host, root / L"missing", missing, 0, FILE_ATTRIBUTE_DIRECTORY) &&
              missing.response.kind != ipc::PreviewContentKind::Bitmap, "missing folder fails without bitmap");
        Check(Request(host, root / L"three", recovered) && Bitmap(recovered), "host recovers after missing folder");
        host.Stop();
        Check(host.Start() && Request(host, root / L"one", recovered) && Bitmap(recovered),
              "dedicated host can stop and restart cleanly");
        std::wprintf(L"[INFO] retained fixtures and actual PNG artwork: %ls\n", root.c_str());
        std::printf("[INFO] IPC timings include transfer; they do not measure UI frames or UI cancellation.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] fixture/test exception: %s\n", error.what());
        ++failures;
    }
    CoUninitialize();
    std::printf("[SUMMARY] %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
