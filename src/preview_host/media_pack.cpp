#include "media_pack.h"
#include "../common/ffmpeg_tool.h"
#include "../common/preview_packs.h"
#include "../common/path_utils.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <thread>

namespace pulse::preview {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// ---- Process runner ----------------------------------------------------------

struct ToolRun {
    bool started = false;
    bool timed_out = false;
    DWORD exit_code = STILL_ACTIVE;
    std::vector<uint8_t> out;
    std::string err;
};

// Launch failures (blocked by antivirus, damaged pack) back off instead of
// costing a CreateProcess per thumbnail.
std::atomic<uint32_t> g_launch_failures{0};
std::atomic<ULONGLONG> g_backoff_until{0};
constexpr uint32_t kLaunchFailureLimit = 3;
constexpr ULONGLONG kLaunchBackoffMs = 60000;

void DrainPipe(ffmpeg::Process& process, bool out, size_t limit, std::vector<uint8_t>* bytes,
               std::string* text) {
    uint8_t buffer[64 * 1024];
    size_t total = 0;
    for (;;) {
        const DWORD got = out ? process.ReadOut(buffer, sizeof(buffer)) : process.ReadErr(buffer, sizeof(buffer));
        if (got == 0) break;
        const size_t keep = total >= limit ? 0 : (std::min)(static_cast<size_t>(got), limit - total);
        total += got;
        if (keep == 0) continue;   // keep reading so the child never blocks on a full pipe
        if (bytes) bytes->insert(bytes->end(), buffer, buffer + keep);
        if (text) text->append(reinterpret_cast<const char*>(buffer), keep);
    }
}

ToolRun RunTool(const std::wstring& exe, const std::wstring& arguments, DWORD timeout_ms,
                size_t max_stdout) {
    ToolRun run;
    const ULONGLONG now = GetTickCount64();
    if (now < g_backoff_until.load()) return run;
    ffmpeg::Process process;
    run.started = process.Start(exe, arguments);
    if (!run.started) {
        if (g_launch_failures.fetch_add(1) + 1 >= kLaunchFailureLimit) {
            g_backoff_until = GetTickCount64() + kLaunchBackoffMs;
            g_launch_failures = 0;
            diagnostics::runtime::Event("media_pack_backoff", {{"error", GetLastError()}});
        }
        return run;
    }
    g_launch_failures = 0;
    std::thread err_reader([&] { DrainPipe(process, false, 256 * 1024, nullptr, &run.err); });
    std::thread out_reader([&] { DrainPipe(process, true, max_stdout, &run.out, nullptr); });
    if (!process.Wait(timeout_ms)) {
        run.timed_out = true;
        process.Terminate(ERROR_TIMEOUT);
    }
    run.exit_code = process.ExitCode();
    out_reader.join();
    err_reader.join();
    return run;
}

std::wstring FfmpegInput(const std::wstring& path) { return ffmpeg::InputArgument(path); }

std::wstring FormatDuration(uint32_t ms) {
    const uint32_t total = (ms + 500) / 1000;
    wchar_t text[32]{};
    if (total >= 3600) swprintf_s(text, L"%u:%02u:%02u", total / 3600, total / 60 % 60, total % 60);
    else swprintf_s(text, L"%u:%02u", total / 60, total % 60);
    return text;
}

bool HasRow(const std::vector<PreviewPropertyValue>& rows, const wchar_t* label) {
    return std::any_of(rows.begin(), rows.end(), [&](const PreviewPropertyValue& row) { return row.label == label; });
}

std::wstring CodecDisplay(const std::string& name, const std::string& profile) {
    static const struct { const char* id; const wchar_t* text; } kNames[] = {
        {"h264", L"H.264 (AVC)"}, {"hevc", L"H.265 (HEVC)"}, {"av1", L"AV1"}, {"vp9", L"VP9"},
        {"vp8", L"VP8"}, {"mpeg4", L"MPEG-4 Part 2"}, {"mpeg2video", L"MPEG-2"},
        {"mpeg1video", L"MPEG-1"}, {"prores", L"Apple ProRes"}, {"vc1", L"VC-1"},
        {"wmv3", L"Windows Media Video 9"}, {"rv40", L"RealVideo 4"}, {"rv30", L"RealVideo 3"},
        {"flv1", L"Sorenson Spark"}, {"theora", L"Theora"}, {"mjpeg", L"Motion JPEG"},
        {"dnxhd", L"Avid DNxHD"}, {"aac", L"AAC"}, {"mp3", L"MP3"}, {"ac3", L"Dolby Digital"},
        {"eac3", L"Dolby Digital Plus"}, {"truehd", L"Dolby TrueHD"}, {"dts", L"DTS"},
        {"flac", L"FLAC"}, {"opus", L"Opus"}, {"vorbis", L"Vorbis"}, {"alac", L"ALAC"},
        {"ape", L"Monkey's Audio"}, {"wavpack", L"WavPack"}, {"cook", L"RealAudio"},
        {"wmav2", L"Windows Media Audio"}, {"amr_nb", L"AMR"}, {"pcm_s16le", L"PCM"},
        {"pcm_s24le", L"PCM"}, {"dsd_lsbf", L"DSD"}, {"dsd_msbf", L"DSD"},
    };
    std::wstring text;
    for (const auto& entry : kNames)
        if (name == entry.id) { text = entry.text; break; }
    if (text.empty()) text = Utf8ToWide(name);
    if (!profile.empty() && profile != "unknown" && profile != "LC" && name != "aac")
        text += L" " + Utf8ToWide(profile);
    return text;
}

} // namespace

// ---- Public API ------------------------------------------------------------

bool IsMediaPackVideoExtension(std::wstring_view e) { return ffmpeg::IsVideoExtension(e); }

bool IsMediaPackAudioExtension(std::wstring_view e) { return ffmpeg::IsAudioExtension(e); }

bool MediaPackAvailable() {
    return packs::ResolvePack(packs::PackId::Media).source != packs::ToolSource::None;
}

uint32_t ParseFfmpegDurationMs(std::string_view log) { return ffmpeg::ParseDurationMs(log); }

bool ParseFfmpegVideoSize(std::string_view log, UINT& width, UINT& height) {
    return ffmpeg::ParseVideoSize(log, width, height);
}

bool DecodeBmpToBgra(const std::vector<uint8_t>& bmp, std::vector<uint8_t>& out,
                     UINT& width, UINT& height, UINT& stride) {
    if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, bmp.data() + at, 4); return v; };
    auto u16 = [&](size_t at) { uint16_t v; memcpy(&v, bmp.data() + at, 2); return v; };
    const uint32_t offset = u32(10);
    const int32_t w = static_cast<int32_t>(u32(18));
    const int32_t h_signed = static_cast<int32_t>(u32(22));
    const uint16_t bpp = u16(28);
    const uint32_t compression = u32(30);
    if (w <= 0 || h_signed == 0 || w > 16384 || std::abs(h_signed) > 16384) return false;
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) return false;
    const bool bottom_up = h_signed > 0;
    const UINT h = static_cast<UINT>(std::abs(h_signed));
    const size_t src_stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    if (offset > bmp.size() || bmp.size() - offset < src_stride * h) return false;
    width = static_cast<UINT>(w);
    height = h;
    stride = width * 4;
    out.assign(static_cast<size_t>(stride) * height, 0);
    const size_t step = bpp / 8;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* src = bmp.data() + offset + src_stride * (bottom_up ? h - 1 - y : y);
        uint8_t* dst = out.data() + static_cast<size_t>(stride) * y;
        for (UINT x = 0; x < width; ++x, src += step, dst += 4) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        }
    }
    return true;
}

uint32_t ThumbnailSeekMs(uint32_t duration_ms) {
    // Skip intros and fade-ins like Explorer, without walking far into long
    // files (seeking a remote or spinning disk costs time).
    if (duration_ms == 0) return 3000;
    if (duration_ms < 2000) return 0;
    return (std::min)(duration_ms / 10, 30000u);
}

std::wstring QuoteArgument(const std::wstring& argument) { return ffmpeg::QuoteArgument(argument); }

bool MediaPackFrame(const std::wstring& path, UINT cap, bool grid, uint32_t known_duration_ms,
                    MediaFrame& frame) {
    const std::wstring ffmpeg = packs::PackToolPath(packs::PackId::Media, L"ffmpeg.exe");
    if (ffmpeg.empty()) return false;
    cap = std::clamp(cap, 16u, 4096u);
    const ULONGLONG started = GetTickCount64();
    // Square-pixel display size, then fit inside cap x cap without enlarging.
    wchar_t filter[256];
    swprintf_s(filter, L"scale=iw*sar:ih,setsar=1,scale=w='min(%u,iw)':h='min(%u,ih)':"
        L"force_original_aspect_ratio=decrease:flags=%s,format=bgr24", cap, cap,
        grid ? L"fast_bilinear" : L"bicubic");
    const DWORD timeout = grid ? 6000 : 12000;
    const uint32_t seeks[2] = {ThumbnailSeekMs(known_duration_ms), 0};
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1 && seeks[0] == 0) break;
        std::wstring args = L"-hide_banner -nostdin -loglevel info -threads 2 ";
        if (seeks[attempt] > 0) {
            wchar_t ss[48];
            swprintf_s(ss, L"-ss %u.%03u ", seeks[attempt] / 1000, seeks[attempt] % 1000);
            args += ss;
        }
        args += L"-i " + QuoteArgument(FfmpegInput(path));
        args += L" -map 0:v:0 -an -sn -dn -frames:v 1 -vf ";
        args += QuoteArgument(filter);
        args += L" -f image2pipe -c:v bmp pipe:1";
        ToolRun run = RunTool(ffmpeg, args, timeout, 64u * 1024 * 1024);
        if (!run.started) return false;
        if (!frame.duration_ms) frame.duration_ms = ParseFfmpegDurationMs(run.err);
        if (!frame.source_width) ParseFfmpegVideoSize(run.err, frame.source_width, frame.source_height);
        if (!run.timed_out && DecodeBmpToBgra(run.out, frame.pixels, frame.width, frame.height, frame.stride)) {
            diagnostics::runtime::Event("media_pack_frame", {{"ms", GetTickCount64() - started},
                {"grid", grid ? 1u : 0u}, {"attempt", static_cast<uint64_t>(attempt)}});
            return true;
        }
        if (run.timed_out) break;   // a second try would only double the wait
        // Otherwise the seek may have passed the end of a short clip: the
        // second attempt starts from the first frame.
    }
    diagnostics::runtime::Event("media_pack_frame_failed", {{"ms", GetTickCount64() - started}});
    frame = MediaFrame{};
    return false;
}

std::vector<PreviewPropertyValue> ParseProbeRows(std::string_view output) {
    // Sections arrive in order: [STREAM]...[/STREAM] blocks then [FORMAT].
    struct Stream { std::string type, codec, profile, width, height, rate, sample_rate, channels; };
    std::vector<Stream> streams;
    std::string duration, bit_rate;
    Stream* current = nullptr;
    bool in_format = false;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) end = output.size();
        std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "[STREAM]") { streams.emplace_back(); current = &streams.back(); in_format = false; continue; }
        if (line == "[/STREAM]") { current = nullptr; continue; }
        if (line == "[FORMAT]") { in_format = true; current = nullptr; continue; }
        if (line == "[/FORMAT]") { in_format = false; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key(line.substr(0, eq)), value(line.substr(eq + 1));
        if (current) {
            if (key == "codec_type") current->type = value;
            else if (key == "codec_name") current->codec = value;
            else if (key == "profile") current->profile = value;
            else if (key == "width") current->width = value;
            else if (key == "height") current->height = value;
            else if (key == "avg_frame_rate") current->rate = value;
            else if (key == "sample_rate") current->sample_rate = value;
            else if (key == "channels") current->channels = value;
        } else if (in_format) {
            if (key == "duration") duration = value;
            else if (key == "bit_rate") bit_rate = value;
        }
    }
    std::vector<PreviewPropertyValue> rows;
    const double seconds = duration.empty() || duration == "N/A" ? 0.0 : atof(duration.c_str());
    if (seconds > 0) rows.push_back({L"时长", FormatDuration(static_cast<uint32_t>(seconds * 1000.0))});
    const Stream* video = nullptr;
    const Stream* audio = nullptr;
    for (const Stream& s : streams) {
        if (s.type == "video" && !video && s.codec != "mjpeg" && s.codec != "png") video = &s;
        if (s.type == "audio" && !audio) audio = &s;
    }
    if (!video) for (const Stream& s : streams) if (s.type == "video") { video = &s; break; }
    if (video) {
        if (!video->width.empty() && !video->height.empty() && video->width != "0")
            rows.push_back({L"分辨率", Utf8ToWide(video->width) + L" x " + Utf8ToWide(video->height)});
        const size_t slash = video->rate.find('/');
        if (slash != std::string::npos) {
            const double num = atof(video->rate.substr(0, slash).c_str());
            const double den = atof(video->rate.substr(slash + 1).c_str());
            if (num > 0 && den > 0) {
                // 25 fps, 29.97 fps, 23.976 fps - as players show them.
                const double value = num / den;
                wchar_t fps[32];
                if (std::abs(value - std::round(value)) < 0.005) swprintf_s(fps, L"%.0f fps", value);
                else {
                    swprintf_s(fps, L"%.3f", value);
                    std::wstring trimmed = fps;
                    while (trimmed.back() == L'0') trimmed.pop_back();
                    swprintf_s(fps, L"%s fps", trimmed.c_str());
                }
                rows.push_back({L"帧率", fps});
            }
        }
        rows.push_back({L"编码格式", CodecDisplay(video->codec, video->profile)});
    }
    if (audio) {
        std::wstring text = CodecDisplay(audio->codec, audio->profile);
        const unsigned rate = static_cast<unsigned>(strtoul(audio->sample_rate.c_str(), nullptr, 10));
        if (rate) {
            wchar_t hz[32];
            if (rate % 1000 == 0) swprintf_s(hz, L" · %u kHz", rate / 1000);
            else swprintf_s(hz, L" · %.1f kHz", rate / 1000.0);
            text += hz;
        }
        const unsigned channels = static_cast<unsigned>(strtoul(audio->channels.c_str(), nullptr, 10));
        if (channels == 1) text += L" · 单声道";
        else if (channels == 2) text += L" · 立体声";
        else if (channels > 2) text += L" · " + std::to_wstring(channels) + L" 声道";
        rows.push_back({L"音频", text});
    }
    const double bps = bit_rate.empty() || bit_rate == "N/A" ? 0.0 : atof(bit_rate.c_str());
    if (bps > 0) {
        wchar_t rate[32];
        if (bps >= 1e6) swprintf_s(rate, L"%.1f Mb/s", bps / 1e6);
        else swprintf_s(rate, L"%.0f kb/s", bps / 1e3);
        rows.push_back({L"比特率", rate});
    }
    return rows;
}

bool MediaPackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows,
                         size_t max_rows) {
    const std::wstring ffprobe = packs::PackToolPath(packs::PackId::Media, L"ffprobe.exe");
    if (ffprobe.empty() || rows.size() >= max_rows) return false;
    const std::wstring args = L"-hide_banner -v error -show_entries "
        L"format=duration,bit_rate:stream=codec_type,codec_name,profile,width,height,avg_frame_rate,"
        L"sample_rate,channels -of default=noprint_wrappers=0 " + QuoteArgument(FfmpegInput(path));
    ToolRun run = RunTool(ffprobe, args, 5000, 256 * 1024);
    if (!run.started || run.timed_out || run.out.empty()) return false;
    const std::string_view text(reinterpret_cast<const char*>(run.out.data()), run.out.size());
    bool added = false;
    for (PreviewPropertyValue& row : ParseProbeRows(text)) {
        if (rows.size() >= max_rows) break;
        if (HasRow(rows, row.label.c_str())) continue;
        rows.push_back(std::move(row));
        added = true;
    }
    return added;
}

} // namespace pulse::preview
