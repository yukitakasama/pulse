#include "preview_host_client.h"
#include "../preview_host/image_frames.h"
#include "../preview_host/svg_raster.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<unsigned char>;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
void U32(Bytes& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
void Chunk(Bytes& file, const char* type, const Bytes& data) {
    U32(file, static_cast<uint32_t>(data.size()));
    const size_t start = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), data.begin(), data.end());
    uint32_t crc = 0xffffffffu;
    for (size_t i = start; i < file.size(); ++i) {
        crc ^= file[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    U32(file, ~crc);
}
Bytes Animation(uint32_t actual, uint32_t declared) {
    Bytes file{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr;
    U32(ihdr, 1); U32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    Chunk(file, "IHDR", ihdr);
    Bytes control; U32(control, declared); U32(control, 0);
    Chunk(file, "acTL", control);
    // A zlib stream with one uncompressed DEFLATE block: filter byte + red RGB pixel.
    const Bytes raw{0, 255, 0, 0};
    Bytes zlib{0x78, 0x01, 0x01, 4, 0, 0xfb, 0xff};
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    uint32_t a = 1, b = 0;
    for (const auto value : raw) { a = (a + value) % 65521; b = (b + a) % 65521; }
    U32(zlib, (b << 16) | a);
    uint32_t sequence = 0;
    for (uint32_t i = 0; i < actual; ++i) {
        Bytes frame; U32(frame, sequence++); U32(frame, 1); U32(frame, 1);
        U32(frame, 0); U32(frame, 0);
        frame.insert(frame.end(), {0, 1, 0, 10, 0, 0});
        Chunk(file, "fcTL", frame);
        if (i == 0) Chunk(file, "IDAT", zlib);
        else {
            Bytes data; U32(data, sequence++); data.insert(data.end(), zlib.begin(), zlib.end());
            Chunk(file, "fdAT", data);
        }
    }
    Chunk(file, "IEND", {});
    return file;
}
bool Write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}
bool Tiff(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatTiff, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    for (unsigned page = 0; page < 2; ++page) {
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        Bytes pixel = page == 0 ? Bytes{0, 0, 255} : Bytes{255, 0, 0};
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(1, 1)) || FAILED(frame->SetPixelFormat(&format)) ||
            !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) ||
            FAILED(frame->WritePixels(1, 3, 3, pixel.data())) || FAILED(frame->Commit())) return false;
    }
    return SUCCEEDED(encoder->Commit());
}
struct SvgImage {
    Bytes pixels;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
    std::wstring error;
    unsigned Channel(UINT x, UINT y, UINT channel) const {
        return x < width && y < height ? pixels[static_cast<size_t>(y) * stride + x * 4 + channel] : 0;
    }
};
bool Svg(const std::filesystem::path& directory, std::string_view source, SvgImage& image) {
    const auto path = directory / "case.svg";
    if (!Write(path, Bytes(source.begin(), source.end()))) return false;
    image = {};
    return pulse::preview::RasterizeSvgFile(path.wstring(), 512, image.pixels, image.width, image.height,
        image.stride, image.source_width, image.source_height, &image.error);
}
void SvgCases(const std::filesystem::path& directory) {
    SvgImage image;
    const std::string simple = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='80'><rect x='20' y='20' width='40' height='20' fill='red'/><!-- <text> --><title>中文标题</title></svg>";
    Check(Svg(directory, simple, image) && image.Channel(25, 25, 2) > 240 &&
        image.Channel(25, 25, 0) < 10 && image.Channel(5, 5, 3) == 0,
        "system SVG paints actual red shape and ignores only harmless title/comment");
    Check(Write(directory / "simple.svg", Bytes(simple.begin(), simple.end())), "write system SVG fixture");
    Check(Svg(directory, "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='80'><defs><clipPath id='c'><rect x='20' y='20' width='20' height='20'/></clipPath></defs><rect x='20' y='20' width='60' height='20' fill='red' clip-path='url(#c)'/></svg>", image) &&
        image.Channel(25, 25, 3) > 240 && image.Channel(45, 25, 3) == 0, "system SVG clipPath removes outside pixels");
    Check(Svg(directory, "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='30'><defs><linearGradient id='g'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect width='100' height='30' fill='url(#g)'/></svg>", image) &&
        image.Channel(10, 15, 2) > image.Channel(10, 15, 0) && image.Channel(90, 15, 0) > image.Channel(90, 15, 2), "system SVG gradient preserves colors");
    const std::string chinese = "<svg xmlns='http://www.w3.org/2000/svg'><text>中文预览 VISIBLE_LABEL</text></svg>";
    Check(!Svg(directory, chinese, image) && image.error == L"svg-unsupported:element:text" && image.pixels.empty(),
        "SVG text is rejected before Direct2D can omit it");
    Check(Write(directory / "text.svg", Bytes(chinese.begin(), chinese.end())), "write multilingual source fallback fixture");
    const std::wstring utf16 = L"<svg xmlns='http://www.w3.org/2000/svg'><text>中文预览</text></svg>";
    Bytes utf16_bytes{0xff, 0xfe};
    for (wchar_t value : utf16) { utf16_bytes.push_back(static_cast<unsigned char>(value)); utf16_bytes.push_back(static_cast<unsigned char>(value >> 8)); }
    const auto utf16_path = directory / "utf16.svg";
    Check(Write(utf16_path, utf16_bytes) && !pulse::preview::RasterizeSvgFile(utf16_path.wstring(), 512,
        image.pixels, image.width, image.height, image.stride, image.source_width, image.source_height, &image.error) &&
        image.error == L"svg-unsupported:element:text" && image.pixels.empty(), "UTF-16 SVG cannot bypass unsupported-text detection");
    for (const auto& item : std::vector<std::pair<std::string, std::wstring>>{
        {"<svg><style>rect{fill:red}</style><rect/></svg>", L"svg-unsupported:element:style"},
        {"<svg><rect style='filter:blur(3px)'/></svg>", L"svg-unsupported:attribute:style"},
        {"<svg><filter id='f'/></svg>", L"svg-unsupported:element:filter"},
        {"<svg><script>VISIBLE_LABEL</script></svg>", L"svg-unsupported:element:script"},
        {"<svg><animate attributeName='x'/></svg>", L"svg-unsupported:element:animate"},
        {"<svg><foreignObject/></svg>", L"svg-unsupported:element:foreignObject"},
        {"<svg xmlns:xlink='http://www.w3.org/1999/xlink'><image xlink:href='file:///C:/private.png'/></svg>", L"svg-unsupported:attribute:href"},
        {"<svg><rect width='2em'/></svg>", L"svg-unsupported:attribute:width"},
        {"<svg xmlns:x='urn:test'><rect x:fill='red'/></svg>", L"svg-unsupported:attribute-namespace"},
        {"<svg><rect></svg>", L"svg-parse-failed"}})
        Check(!Svg(directory, item.first, image) && image.pixels.empty() && image.error == item.second,
            "system SVG rejects unsupported content with explicit reason and no partial pixels");
    std::string nested = "<svg>";
    for (int i = 0; i < 129; ++i) nested += "<g>";
    for (int i = 0; i < 129; ++i) nested += "</g>";
    nested += "</svg>";
    Check(!Svg(directory, nested, image) && image.error == L"svg-budget-exceeded", "SVG nesting budget reports explicit rejection");
    Check(!Svg(directory, std::string(8u * 1024 * 1024 + 1, ' '), image) && image.error == L"svg-budget-exceeded",
        "SVG source byte budget rejects before renderer allocation");
}
void SvgIpcCases(pulse_test::Host& host, const std::filesystem::path& directory) {
    pulse_test::Result result;
    Check(host.Request((directory / "simple.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Bitmap && !result.pixels.empty() && result.error.empty(),
        "system SVG shapes render through host IPC");
    Check(host.Request((directory / "text.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Text && result.text.find(L"中文预览 VISIBLE_LABEL") != std::wstring::npos &&
        result.error == L"svg-unsupported:element:text", "SVG source fallback preserves text and warning through IPC");
    Check(host.Request((directory / "utf16.svg").wstring(), result) &&
        result.response.kind == pulse::ipc::PreviewContentKind::Text && result.text.find(L"中文预览") != std::wstring::npos &&
        result.error == L"svg-unsupported:element:text", "UTF-16 SVG source and warning survive host IPC");
}
}

int main(int argc, char** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { Check(false, "COM initialization"); return 1; }
    const auto directory = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("image-integrity-" + std::to_string(GetCurrentProcessId())));
    std::filesystem::create_directories(directory);
    SvgCases(directory);
    if (argc > 1 && std::string_view(argv[1]) == "--svg-only") {
        pulse_test::Host svg_host;
        const bool svg_started = svg_host.Start();
        Check(svg_started, "start SVG preview host");
        if (svg_started) SvgIpcCases(svg_host, directory);
        svg_host.Stop();
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        Check(!cleanup_error, "clean SVG fixtures");
        CoUninitialize();
        return failures ? 1 : 0;
    }
    Check(Tiff(directory / "pages.tiff"), "encode two distinct TIFF pages");
    const auto capped = Animation(4097, 4097);
    pulse::preview::ApngInfo info;
    Check(pulse::preview::ParseApng(capped, info) && info.declared_frames == 4097 && info.observed_frames == 4097 &&
        info.frames.size() == 4096 && info.incomplete_reason == L"frame-limit", "APNG retains declared and bounded playable frame counts");
    Check(Write(directory / "capped.png", capped), "write bounded APNG fixture");
    auto truncated = Animation(2, 3);
    Check(pulse::preview::ParseApng(truncated, info) && info.frames.size() == 2 &&
        info.incomplete_reason == L"frame-count-mismatch", "APNG declared count mismatch is explicit");
    Check(Write(directory / "mismatch.png", truncated), "write mismatched APNG fixture");
    truncated.resize(truncated.size() - 12);
    Check(pulse::preview::ParseApng(truncated, info) && info.incomplete_reason == L"truncated-data", "APNG missing IEND reports truncation");
    Check(Write(directory / "limited.png", Animation(2, 2)), "write APNG limit fallback fixture");
    std::filesystem::resize_file(directory / "limited.png", 97ull * 1024 * 1024);
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "start isolated preview host");
    if (started) {
        pulse_test::Result result;
        SvgIpcCases(host, directory);
        for (uint32_t page : {0u, 1u, 99u}) {
            result = {};
            const bool received = host.Request((directory / "pages.tiff").wstring(), result, MAXDWORD, 128,
                pulse::ipc::PreviewRequestKind::Content, 0, page);
            const bool blue = page != 0;
            Check(received && result.response.kind == pulse::ipc::PreviewContentKind::Bitmap &&
                result.response.frame_count == 2 && result.response.frame_delay_ms == 0 && result.pixels.size() >= 4 &&
                result.pixels[blue ? 0 : 2] > 240 && result.pixels[blue ? 2 : 0] < 10 &&
                result.text.find(blue ? L"F\t2\t2\t1" : L"F\t2\t2\t0") != std::wstring::npos,
                "TIFF frame request selects actual page, clamps and never animates");
        }
        for (const auto& item : std::vector<std::pair<std::wstring, std::wstring>>{
            {L"capped.png", L"frame-limit"}, {L"mismatch.png", L"frame-count-mismatch"}, {L"limited.png", L"file-limit"}}) {
            result = {};
            const bool received = host.Request((directory / item.first).wstring(), result);
            const uint32_t count = item.first == L"capped.png" ? 4096u : item.first == L"limited.png" ? 1u : 2u;
            Check(received && result.response.kind == pulse::ipc::PreviewContentKind::Bitmap &&
                result.response.frame_count == count && (result.response.flags & pulse::ipc::kPreviewFlagTruncated) &&
                result.error == L"image-preview-incomplete:" + item.second &&
                result.text.find(L"W\t" + item.second) != std::wstring::npos,
                "APNG IPC preserves warning and honest playable count");
            if (!result.error.empty()) std::wcout << L"  reason: " << result.error << L'\n';
            if (count == 1) Check(result.response.frame_delay_ms == 0 &&
                result.text.find(L"T\tstatic-fallback") != std::wstring::npos &&
                result.text.find(L"F\t1\t2\t0") != std::wstring::npos, "static APNG fallback preserves declared count as metadata only");
        }
        host.Stop();
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    Check(!cleanup_error, "clean isolated fixtures");
    CoUninitialize();
    return failures ? 1 : 0;
}
