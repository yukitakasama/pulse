#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <libraw/libraw.h>
#include "image_pack_protocol.h"
#include "../../images/src/pixel_ops.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wire = pulse::imgpack;
namespace {
struct Frame {
    wire::FrameHeader header;
    std::vector<uint8_t> pixels;
};
constexpr uint64_t kFullPixels = 128ull * 1024 * 1024;
constexpr unsigned kThumbnailBytes = 64 * 1024 * 1024;

bool Convert(IWICImagingFactory* factory, IWICBitmapSource* source, unsigned cap, int flip, Frame& frame) {
    ComPtr<IWICBitmapSource> oriented = source;
    ComPtr<IWICBitmapFlipRotator> rotate;
    WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
    if (flip == 1) transform = WICBitmapTransformFlipHorizontal;
    else if (flip == 2) transform = WICBitmapTransformFlipVertical;
    else if (flip == 3) transform = WICBitmapTransformRotate180;
    else if (flip == 4) transform = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    else if (flip == 5) transform = WICBitmapTransformRotate270;
    else if (flip == 6) transform = WICBitmapTransformRotate90;
    else if (flip == 7) transform = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    if (transform != WICBitmapTransformRotate0) {
        if (FAILED(factory->CreateBitmapFlipRotator(&rotate)) || FAILED(rotate->Initialize(source, transform))) return false;
        oriented = rotate;
    }
    UINT w = 0, h = 0;
    if (FAILED(oriented->GetSize(&w, &h)) || !w || !h || uint64_t(w) * h > kFullPixels) return false;
    const auto size = imgpack::FitSize(w, h, cap);
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(oriented.Get(), size.w, size.h, WICBitmapInterpolationModeFant))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                    nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    frame.header.width = size.w;
    frame.header.height = size.h;
    frame.header.stride = size.w * 4;
    frame.pixels.resize(size_t(size.w) * size.h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, frame.header.stride, static_cast<UINT>(frame.pixels.size()), frame.pixels.data()));
}

bool ConvertMemory(IWICImagingFactory* factory, libraw_processed_image_t* image, unsigned cap, int flip, Frame& frame) {
    if (!image || !image->data_size) return false;
    if (image->type == LIBRAW_IMAGE_JPEG) {
        if (image->data_size > kThumbnailBytes) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> source;
        return SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(image->data, image->data_size)) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &source)) && Convert(factory, source.Get(), cap, flip, frame);
    }
    if (image->type != LIBRAW_IMAGE_BITMAP || image->bits != 8 ||
        (image->colors != 3 && image->colors != 1) || !image->width || !image->height ||
        uint64_t(image->width) * image->height > kFullPixels ||
        uint64_t(image->width) * image->height * image->colors != image->data_size) return false;
    ComPtr<IWICBitmap> bitmap;
    const GUID& format = image->colors == 3 ? GUID_WICPixelFormat24bppRGB : GUID_WICPixelFormat8bppGray;
    return SUCCEEDED(factory->CreateBitmapFromMemory(image->width, image->height, format,
        unsigned(image->width) * image->colors, image->data_size, image->data, &bitmap)) &&
        Convert(factory, bitmap.Get(), cap, flip, frame);
}
std::string Clean(const char* text, size_t limit) {
    std::string out;
    for (size_t i = 0; i < limit && text[i]; ++i) out += (static_cast<unsigned char>(text[i]) < ' ' ? ' ' : text[i]);
    return out;
}
int Run(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"formats") {
        puts(".cr2 .cr3 .crw .nef .nrw .arw .srf .sr2 .dng .raf .orf .rw2 .pef .srw .rwl .3fr .fff .iiq .kdc .dcr .mos .mrw .x3f .raw");
        return wire::kExitOk;
    }
    if (argc < 3) return wire::kExitUsage;
    const std::wstring_view mode(argv[1]);
    const bool full = mode == L"decode-full";
    const bool probe = mode == L"probe";
    if ((!probe && mode != L"decode" && !full) || argc != (probe ? 3 : 4)) return wire::kExitUsage;
    unsigned cap = 0;
    if (!probe) {
        wchar_t* end = nullptr;
        const auto value = wcstoul(argv[3], &end, 10);
        if (!end || *end || value < 16 || value > wire::kMaxCap) return wire::kExitUsage;
        cap = static_cast<unsigned>(value);
    }
    LibRaw raw;
    raw.imgdata.rawparams.max_raw_memory_mb = full ? 1024 : 256;
    if (raw.open_file(argv[2]) != LIBRAW_SUCCESS) return wire::kExitUnsupported;
    const auto& size = raw.imgdata.sizes;
    if (!size.width || !size.height || uint64_t(size.raw_width) * size.raw_height > wire::kMaxSourcePixels)
        return wire::kExitTooLarge;
    if (probe) {
        printf("format=Camera RAW\nwidth=%u\nheight=%u\ncamera=%s %s\n", size.width, size.height,
            Clean(raw.imgdata.idata.make, sizeof(raw.imgdata.idata.make)).c_str(),
            Clean(raw.imgdata.idata.model, sizeof(raw.imgdata.idata.model)).c_str());
        return wire::kExitOk;
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return wire::kExitDecodeFailed;
    Frame frame;
    frame.header.source_width = size.width;
    frame.header.source_height = size.height;
    bool decoded = false;
    if (raw.imgdata.thumbnail.tlength <= kThumbnailBytes && raw.unpack_thumb() == LIBRAW_SUCCESS) {
        std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)> thumb(raw.dcraw_make_mem_thumb(), &LibRaw::dcraw_clear_mem);
        decoded = ConvertMemory(factory.Get(), thumb.get(), cap, size.flip, frame);
        if (decoded) frame.header.flags = wire::kFlagEmbeddedThumbnail;
    }
    if (!decoded && full) {
        if (uint64_t(size.raw_width) * size.raw_height > kFullPixels) return wire::kExitTooLarge;
        raw.imgdata.params.output_bps = 8;
        raw.imgdata.params.output_color = 1;
        raw.imgdata.params.use_camera_wb = 1;
        raw.imgdata.params.user_qual = 0;
        raw.imgdata.params.half_size = std::max(size.width, size.height) > cap * 2;
        if (raw.unpack() != LIBRAW_SUCCESS || raw.dcraw_process() != LIBRAW_SUCCESS) return wire::kExitDecodeFailed;
        std::unique_ptr<libraw_processed_image_t, decltype(&LibRaw::dcraw_clear_mem)> image(raw.dcraw_make_mem_image(), &LibRaw::dcraw_clear_mem);
        decoded = ConvertMemory(factory.Get(), image.get(), cap, 0, frame);
    }
    if (!decoded) return wire::kExitDecodeFailed;
    _setmode(_fileno(stdout), _O_BINARY);
    if (fwrite(&frame.header, sizeof(frame.header), 1, stdout) != 1 ||
        fwrite(frame.pixels.data(), 1, frame.pixels.size(), stdout) != frame.pixels.size() || fflush(stdout)) return wire::kExitIoError;
    return wire::kExitOk;
}
}
int wmain(int argc, wchar_t** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return wire::kExitDecodeFailed;
    int code = wire::kExitDecodeFailed;
    try { code = Run(argc, argv); } catch (...) { code = wire::kExitDecodeFailed; }
    CoUninitialize();
    return code;
}


