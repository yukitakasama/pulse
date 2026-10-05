#pragma once
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

// Loudness envelope of an audio file for the Quick Look audio preview.
// Decoding runs on a detached worker (Media Foundation source reader, loaded on
// demand). What Media Foundation cannot read (no decoder, Windows N) is decoded
// by the FFmpeg preview pack when present; otherwise the track stays plain. Each
// Start has its own mailbox; a retired worker can never publish into the next
// file's waveform, and Reset/Start only raise its stop flag, never block.
class AudioWaveform {
public:
    static constexpr size_t kBuckets = 600;
    AudioWaveform() = default;
    ~AudioWaveform();
    AudioWaveform(const AudioWaveform&) = delete;
    AudioWaveform& operator=(const AudioWaveform&) = delete;

    void Start(const std::wstring& path);
    void Reset();
    // Loudness levels (0..1, kBuckets entries; see WaveformLevels) decoded so
    // far; buckets not yet reached are zero. progress is 0..1; failed means "draw a plain track".
    bool Snapshot(std::vector<float>& peaks, float& progress, bool& failed) const;
    bool active() const noexcept { return state_ != nullptr; }

private:
    struct Shared;
    static void Run(std::shared_ptr<Shared> state, std::wstring path);
    std::shared_ptr<Shared> state_;
};

// Per-bucket loudness as the ear hears it: the RMS of each bucket in dB,
// mapped over a 30 dB window below the loudest bucket so far to 0..1. Peaks
// made every bar of mastered music touch the top; RMS in dB shows the song's
// shape (quiet verse, loud chorus). Silent buckets are 0. Exposed for tests.
class WaveformLevels {
public:
    static constexpr float kWindowDb = 30.0f;
    explicit WaveformLevels(size_t buckets = AudioWaveform::kBuckets)
        : sum_(buckets, 0.0), count_(buckets, 0) {}
    void Add(size_t bucket, float sample) {
        if (bucket >= sum_.size() || !std::isfinite(sample)) return;
        sum_[bucket] += static_cast<double>(sample) * sample;
        ++count_[bucket];
    }
    std::vector<float> Levels() const;
private:
    std::vector<double> sum_;
    std::vector<uint32_t> count_;
};

// Levels of `path` decoded by `ffmpeg` into AudioWaveform::kBuckets buckets.
// `publish(levels, progress)` is called every ~200 ms and at the end; false
// when ffmpeg cannot read the file or it is too long. Exposed for tests.
bool FfmpegWaveform(const std::wstring& ffmpeg, const std::wstring& path, const std::atomic<bool>& stop,
                    const std::function<void(const std::vector<float>&, float)>& publish);

} // namespace pulse::ui
