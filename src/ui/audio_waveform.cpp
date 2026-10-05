// Keep MF header feature gates consistent with the Windows 8.1 app target
// (same as video_preview.cpp).
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#include "audio_waveform.h"
#include "../common/path_utils.h"
#include "../common/ffmpeg_tool.h"
#include "../common/preview_packs.h"
#include "ffmpeg_playback.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

#pragma comment(lib, "mfuuid.lib")

namespace pulse::ui {
using Microsoft::WRL::ComPtr;

namespace {
// Longer recordings (podcasts, audiobooks) would keep a core busy for many
// seconds; they get the plain progress track instead.
constexpr int64_t kMaxDuration = 30ll * 60 * 10000000;  // 30 min in 100 ns

// Media Foundation's verdict: Unreadable lets the FFmpeg pack try instead.
enum class MfResult { Done, Unreadable, Failed };
}  // namespace

struct AudioWaveform::Shared {
    mutable std::mutex mutex;
    std::vector<float> peaks = std::vector<float>(kBuckets, 0.0f);
    float progress = 0.0f;
    bool failed = false;
    std::atomic<bool> stop{false};
};

AudioWaveform::~AudioWaveform() { Reset(); }

void AudioWaveform::Start(const std::wstring& path) {
    Reset();
    state_ = std::make_shared<Shared>();
    try {
        std::thread(Run, state_, path).detach();
    } catch (...) {
        state_->failed = true;
    }
}

void AudioWaveform::Reset() {
    if (state_) state_->stop.store(true);
    state_.reset();
}

bool AudioWaveform::Snapshot(std::vector<float>& peaks, float& progress, bool& failed) const {
    if (!state_) return false;
    std::lock_guard lock(state_->mutex);
    peaks = state_->peaks;
    progress = state_->progress;
    failed = state_->failed;
    return true;
}

std::vector<float> WaveformLevels::Levels() const {
    std::vector<float> levels(sum_.size(), 0.0f);
    double top = 0.0;
    for (size_t i = 0; i < sum_.size(); ++i)
        if (count_[i]) { levels[i] = static_cast<float>(std::sqrt(sum_[i] / count_[i])); top = (std::max)(top, static_cast<double>(levels[i])); }
    if (top < 1e-4) return std::vector<float>(sum_.size(), 0.0f);   // below -80 dBFS: silence
    for (float& level : levels) {
        if (level <= 0.0f) continue;
        const double db = 20.0 * std::log10(level / top);
        level = static_cast<float>((std::clamp)((db + kWindowDb) / kWindowDb, 0.0, 1.0));
    }
    return levels;
}

namespace {
MfResult MediaFoundationWaveform(const std::wstring& path, const std::atomic<bool>& stop,
                                 const std::function<void(const std::vector<float>&, float)>& publish_levels) {
    constexpr size_t kBuckets = AudioWaveform::kBuckets;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return MfResult::Failed;
    struct ComScope { ~ComScope() { CoUninitialize(); } } com_scope;

    HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    struct Libraries {
        HMODULE a, b;
        ~Libraries() { if (b) FreeLibrary(b); if (a) FreeLibrary(a); }
    } libraries{plat, readwrite};
    if (!plat || !readwrite) return MfResult::Unreadable;
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    using CreateType = HRESULT (WINAPI*)(IMFMediaType**);
    using CreateReader = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
    const auto startup = reinterpret_cast<Startup>(GetProcAddress(plat, "MFStartup"));
    const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(plat, "MFShutdown"));
    const auto create_type = reinterpret_cast<CreateType>(GetProcAddress(plat, "MFCreateMediaType"));
    const auto create_reader = reinterpret_cast<CreateReader>(
        GetProcAddress(readwrite, "MFCreateSourceReaderFromURL"));
    if (!startup || !shutdown || !create_type || !create_reader ||
        FAILED(startup(MF_VERSION, MFSTARTUP_LITE))) return MfResult::Unreadable;
    struct MfScope { Shutdown s; ~MfScope() { s(); } } mf_scope{shutdown};

    const std::wstring url = pulse::path::StripExtendedPathPrefix(path);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(create_reader(url.c_str(), nullptr, &reader))) return MfResult::Unreadable;
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(audio, TRUE))) return MfResult::Unreadable;
    ComPtr<IMFMediaType> wanted;
    if (FAILED(create_type(&wanted)) ||
        FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
        FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
        FAILED(reader->SetCurrentMediaType(audio, nullptr, wanted.Get()))) return MfResult::Unreadable;
    ComPtr<IMFMediaType> actual;
    if (FAILED(reader->GetCurrentMediaType(audio, &actual))) return MfResult::Unreadable;
    const UINT32 channels = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    const UINT32 rate = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    if (!channels || !rate) return MfResult::Unreadable;

    PROPVARIANT value{};
    PropVariantInit(&value);
    int64_t duration = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) &&
        value.vt == VT_UI8)
        duration = static_cast<int64_t>(value.uhVal.QuadPart);
    PropVariantClear(&value);
    if (duration > kMaxDuration) return MfResult::Failed;
    if (duration <= 0) return MfResult::Unreadable;

    WaveformLevels levels(kBuckets);
    auto publish = [&](float progress) { publish_levels(levels.Levels(), progress); };
    auto last_publish = std::chrono::steady_clock::now();
    const double buckets_per_tick = static_cast<double>(kBuckets) / static_cast<double>(duration);
    const double ticks_per_frame = 1e7 / static_cast<double>(rate);
    while (!stop.load()) {
        DWORD index = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(audio, 0, &index, &flags, &timestamp, &sample))) break;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        const auto* samples = reinterpret_cast<const float*>(data);
        const size_t frames = length / sizeof(float) / channels;
        for (size_t f = 0; f < frames; ++f) {
            const double at = static_cast<double>(timestamp) + static_cast<double>(f) * ticks_per_frame;
            const size_t bucket = static_cast<size_t>((std::clamp)(at * buckets_per_tick, 0.0,
                static_cast<double>(kBuckets - 1)));
            for (UINT32 c = 0; c < channels; ++c) levels.Add(bucket, samples[f * channels + c]);
        }
        buffer->Unlock();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_publish > std::chrono::milliseconds(200)) {
            last_publish = now;
            publish(static_cast<float>((std::clamp)(static_cast<double>(timestamp) /
                                                    static_cast<double>(duration), 0.0, 1.0)));
        }
    }
    if (!stop.load()) publish(1.0f);
    return MfResult::Done;
}
}  // namespace

bool FfmpegWaveform(const std::wstring& ffmpeg_exe, const std::wstring& path, const std::atomic<bool>& stop,
                    const std::function<void(const std::vector<float>&, float)>& publish) {
    constexpr size_t kBuckets = AudioWaveform::kBuckets;
    constexpr uint32_t kRate = 8000;   // loudness only: mono 8 kHz is plenty for 600 buckets
    FfmpegMediaInfo info;
    if (!ProbeFfmpegMedia(ffmpeg_exe, path, info) || !info.audio) return false;
    if (info.duration <= 0 || info.duration > kMaxDuration) return false;
    const double total = static_cast<double>(info.duration) / 1e7 * kRate;
    const std::wstring args = L"-hide_banner -nostdin -v error -i " + ffmpeg::QuoteArgument(ffmpeg::InputArgument(path)) +
        L" -map 0:a:0 -vn -sn -dn -ac 1 -ar " + std::to_wstring(kRate) + L" -f s16le pipe:1";
    ffmpeg::LaunchOptions options;
    options.priority = IDLE_PRIORITY_CLASS;
    options.memory_limit = static_cast<SIZE_T>(512) * 1024 * 1024;
    options.stdout_buffer = 64 * 1024;
    options.discard_stderr = true;
    ffmpeg::Process process;
    if (!process.Start(ffmpeg_exe, args, options)) return false;
    WaveformLevels levels(kBuckets);
    uint64_t done = 0;
    int16_t samples[8192];
    uint8_t carry[1];
    bool carried = false;
    auto last_publish = std::chrono::steady_clock::now();
    const double buckets_per_sample = static_cast<double>(kBuckets) / (std::max)(total, 1.0);
    while (!stop.load()) {
        auto* bytes = reinterpret_cast<uint8_t*>(samples);
        DWORD got = 0;
        if (carried) { bytes[0] = carry[0]; got = 1; carried = false; }
        const DWORD read = process.ReadOut(bytes + got, sizeof(samples) - got);
        if (!read) break;
        got += read;
        if (got & 1) { carry[0] = bytes[got - 1]; carried = true; --got; }
        const size_t count = got / 2;
        for (size_t k = 0; k < count; ++k, ++done) {
            const size_t bucket = static_cast<size_t>((std::min)(static_cast<double>(done) * buckets_per_sample,
                                                                 static_cast<double>(kBuckets - 1)));
            levels.Add(bucket, static_cast<float>(samples[k]) / 32768.0f);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_publish > std::chrono::milliseconds(200)) {
            last_publish = now;
            publish(levels.Levels(), static_cast<float>((std::min)(static_cast<double>(done) / total, 1.0)));
        }
    }
    if (stop.load()) return true;   // abandoned: nothing to show, nothing failed
    process.Wait(2000);
    if (!done) return false;
    publish(levels.Levels(), 1.0f);
    return true;
}

void AudioWaveform::Run(std::shared_ptr<Shared> state, std::wstring path) {
    const auto publish = [&](const std::vector<float>& levels, float progress) {
        std::lock_guard lock(state->mutex);
        for (size_t i = 0; i < kBuckets && i < levels.size(); ++i) state->peaks[i] = levels[i];
        state->progress = progress;
    };
    MfResult result = MediaFoundationWaveform(path, state->stop, publish);
    if (result == MfResult::Unreadable && !state->stop.load()) {
        const std::wstring ffmpeg_exe = packs::PackToolPath(packs::PackId::Media, L"ffmpeg.exe");
        if (!ffmpeg_exe.empty() && FfmpegWaveform(ffmpeg_exe, path, state->stop, publish)) result = MfResult::Done;
    }
    if (result != MfResult::Done) {
        std::lock_guard lock(state->mutex);
        state->failed = true;
    }
}

} // namespace pulse::ui
