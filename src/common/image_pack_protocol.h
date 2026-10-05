// image_pack_protocol.h - what the preview host and pulse-imgpack.exe (the
// image preview pack, packs/images) agree on. Header-only, no dependencies:
// the pack is built from this repository by its own CI job.
//
//   pulse-imgpack.exe decode <file> <cap>   stdout: FrameHeader + pixels
//   pulse-imgpack.exe probe <file>          stdout: UTF-8 "key=value" lines
//   pulse-imgpack.exe formats               stdout: supported extensions
//
// Pixels are 32bpp premultiplied BGRA, top-down, scaled to fit cap x cap
// (never enlarged). HDR pictures are tone-mapped to sRGB by the pack.
#pragma once
#include <cstdint>
#include <cwchar>
#include <string_view>

namespace pulse::imgpack {

inline constexpr uint32_t kMagic = 0x474D4950;   // "PIMG", little endian
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxCap = 8192;
// Larger sources are refused before decoding (pixels, not bytes).
inline constexpr uint64_t kMaxSourcePixels = 1ull << 28;   // 268 MP

enum FrameFlags : uint32_t {
    kFlagAlpha = 1,              // has transparency
    kFlagToneMapped = 2,         // HDR source mapped to SDR
    kFlagEmbeddedThumbnail = 4,  // the file's own preview image was used
};

struct FrameHeader {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    uint32_t width = 0, height = 0, stride = 0;
    uint32_t source_width = 0, source_height = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(FrameHeader) == 32, "wire format");

enum ExitCode : int {
    kExitOk = 0,
    kExitUsage = 1,
    kExitUnsupported = 2,   // not a format the pack reads
    kExitDecodeFailed = 3,  // damaged or an unsupported variant
    kExitTooLarge = 4,
    kExitIoError = 5,
};

// File extensions (lower case, with the dot) the pack is offered for.
inline constexpr std::wstring_view kExtensions[] = {
    L".heic", L".heif", L".hif", L".avif", L".jxl", L".exr", L".hdr", L".qoi"};
inline bool IsImagePackExtension(std::wstring_view ext) {
    for (const auto candidate : kExtensions) {
        const std::wstring_view c(candidate);
        if (ext.size() != c.size()) continue;
        bool same = true;
        for (size_t i = 0; i < c.size() && same; ++i) {
            const wchar_t ch = ext[i] >= L'A' && ext[i] <= L'Z' ? static_cast<wchar_t>(ext[i] + 32) : ext[i];
            same = ch == c[i];
        }
        if (same) return true;
    }
    return false;
}

// Formats Windows may read itself through Store extensions (lower-case
// extension): the pack is the fallback for these and the first choice for the
// rest (EXR, Radiance HDR, QOI), which WIC never reads.
inline bool WindowsMayDecode(std::wstring_view ext) {
    return ext == L".heic" || ext == L".heif" || ext == L".hif" || ext == L".avif" || ext == L".jxl";
}

} // namespace pulse::imgpack
