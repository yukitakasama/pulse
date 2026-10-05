// pixel_ops.h - pixel arithmetic of pulse-imgpack: fitting, area downscaling
// fed one row at a time (a 16k EXR never has to sit in memory), and the HDR
// path (PQ / HLG to linear, BT.2020 to BT.709, auto exposure, filmic curve,
// sRGB). Pure functions, covered by imgpack_test.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace imgpack {

struct Size {
    uint32_t w = 0, h = 0;
    bool operator==(const Size&) const = default;
};

// Fits w x h into cap x cap keeping the aspect ratio; never enlarges.
inline Size FitSize(uint32_t w, uint32_t h, uint32_t cap) {
    if (!w || !h || !cap) return {};
    if (w <= cap && h <= cap) return {w, h};
    const double scale = static_cast<double>(cap) / static_cast<double>((std::max)(w, h));
    return {(std::max)(1u, static_cast<uint32_t>(std::lround(w * scale))),
            (std::max)(1u, static_cast<uint32_t>(std::lround(h * scale)))};
}

// Box filter: every source pixel lands in exactly one output pixel, which
// averages what it received. Rows may arrive in any order or be missing.
template <typename Accum>
class BoxGrid {
public:
    BoxGrid(uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh)
        : sw_(sw), sh_(sh), ow_(ow), oh_(oh), column_(sw), sum_(static_cast<size_t>(ow) * oh * 4, 0),
          count_(static_cast<size_t>(ow) * oh, 0) {
        for (uint32_t x = 0; x < sw; ++x)
            column_[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * ow / sw);
    }
    uint32_t OutputRow(uint32_t y) const { return static_cast<uint32_t>(static_cast<uint64_t>(y) * oh_ / sh_); }
    uint32_t width() const { return ow_; }
    uint32_t height() const { return oh_; }
protected:
    uint32_t sw_, sh_, ow_, oh_;
    std::vector<uint32_t> column_;
    std::vector<Accum> sum_;
    std::vector<uint32_t> count_;
};

// 8-bit RGBA (straight alpha) in, 8-bit premultiplied BGRA out.
class ByteDownscaler : public BoxGrid<uint64_t> {
public:
    using BoxGrid::BoxGrid;
    // channels: 3 (RGB) or 4 (RGBA); `row` holds sw pixels.
    void AddRow(uint32_t y, const uint8_t* row, int channels) {
        if (y >= sh_) return;
        const size_t base = static_cast<size_t>(OutputRow(y)) * ow_;
        for (uint32_t x = 0; x < sw_; ++x, row += channels) {
            const uint32_t a = channels == 4 ? row[3] : 255;
            const size_t cell = base + column_[x];
            uint64_t* s = &sum_[cell * 4];
            s[0] += static_cast<uint64_t>(row[2]) * a;   // B
            s[1] += static_cast<uint64_t>(row[1]) * a;   // G
            s[2] += static_cast<uint64_t>(row[0]) * a;   // R
            s[3] += a;
            ++count_[cell];
        }
    }
    std::vector<uint8_t> Finish() const {
        std::vector<uint8_t> out(static_cast<size_t>(ow_) * oh_ * 4, 0);
        for (size_t cell = 0; cell < count_.size(); ++cell) {
            const uint64_t n = count_[cell];
            if (!n) continue;
            const uint64_t* s = &sum_[cell * 4];
            for (int c = 0; c < 3; ++c) out[cell * 4 + c] = static_cast<uint8_t>((s[c] + n * 255 / 2) / (n * 255));
            out[cell * 4 + 3] = static_cast<uint8_t>((s[3] + n / 2) / n);
        }
        return out;
    }
};

// Linear float RGBA (straight alpha) in, premultiplied linear float out.
class FloatDownscaler : public BoxGrid<double> {
public:
    using BoxGrid::BoxGrid;
    void AddRow(uint32_t y, const float* rgba) {
        if (y >= sh_) return;
        const size_t base = static_cast<size_t>(OutputRow(y)) * ow_;
        for (uint32_t x = 0; x < sw_; ++x, rgba += 4) {
            float a = rgba[3];
            if (!(a >= 0.0f)) a = 0.0f;   // NaN -> transparent
            a = (std::min)(a, 1.0f);
            const size_t cell = base + column_[x];
            double* s = &sum_[cell * 4];
            for (int c = 0; c < 3; ++c) {
                const float v = rgba[c];
                // Negative and non-finite values (out-of-gamut EXR) carry no light.
                s[c] += (v > 0.0f && v < 65504.0f * 4) ? static_cast<double>(v) * a : 0.0;
            }
            s[3] += a;
            ++count_[cell];
        }
    }
    std::vector<float> Finish() const {
        std::vector<float> out(static_cast<size_t>(ow_) * oh_ * 4, 0.0f);
        for (size_t cell = 0; cell < count_.size(); ++cell) {
            const uint32_t n = count_[cell];
            if (!n) continue;
            for (int c = 0; c < 4; ++c) out[cell * 4 + c] = static_cast<float>(sum_[cell * 4 + c] / n);
        }
        return out;
    }
};

// ---- HDR ---------------------------------------------------------------------

// SMPTE ST 2084 (PQ) signal 0..1 -> absolute luminance in cd/m2.
inline float PqToNits(float e) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    e = std::clamp(e, 0.0f, 1.0f);
    const float p = std::pow(e, 1.0f / m2);
    return 10000.0f * std::pow((std::max)(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// ARIB STD-B67 (HLG) signal 0..1 -> scene-linear 0..1.
inline float HlgToLinear(float e) {
    constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::clamp(e, 0.0f, 1.0f);
    return e <= 0.5f ? e * e / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
}

// HDR reference white (ITU-R BT.2408): 203 cd/m2 becomes 1.0.
inline constexpr float kReferenceWhiteNits = 203.0f;

// Linear BT.2020 -> linear BT.709 primaries.
inline void Bt2020To709(float& r, float& g, float& b) {
    const float r2 = 1.6605f * r - 0.5876f * g - 0.0728f * b;
    const float g2 = -0.1246f * r + 1.1329f * g - 0.0083f * b;
    const float b2 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
    r = r2; g = g2; b = b2;
}

// Exposure that puts the picture's log-average luminance at middle grey
// (premultiplied linear input). Clamped to +-6 stops.
inline float AutoExposure(const std::vector<float>& premultiplied) {
    double log_sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i + 3 < premultiplied.size(); i += 4) {
        const float a = premultiplied[i + 3];
        if (a < 0.01f) continue;
        const float y = (0.2126f * premultiplied[i] + 0.7152f * premultiplied[i + 1] +
                         0.0722f * premultiplied[i + 2]) / a;
        log_sum += std::log(1e-4 + (std::max)(0.0f, y));
        ++n;
    }
    if (!n) return 1.0f;
    const double average = std::exp(log_sum / static_cast<double>(n));
    return std::clamp(static_cast<float>(0.18 / average), 1.0f / 64.0f, 64.0f);
}

// Filmic shoulder (ACES fit by K. Narkowicz): soft highlights, 0..1 out.
inline float Filmic(float x) {
    x = (std::max)(x, 0.0f);
    return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

inline float SrgbEncode(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Premultiplied linear float RGBA -> premultiplied sRGB BGRA8 through the
// filmic curve at `exposure`. Display values come from a 4096-entry table.
inline std::vector<uint8_t> ToneMapToBgra(const std::vector<float>& premultiplied, float exposure) {
    static const std::array<uint8_t, 4097> table = [] {
        std::array<uint8_t, 4097> t{};
        for (size_t i = 0; i < t.size(); ++i)
            t[i] = static_cast<uint8_t>(std::lround(SrgbEncode(static_cast<float>(i) / 4096.0f) * 255.0f));
        return t;
    }();
    std::vector<uint8_t> out(premultiplied.size(), 0);
    for (size_t i = 0; i + 3 < premultiplied.size(); i += 4) {
        const float a = std::clamp(premultiplied[i + 3], 0.0f, 1.0f);
        if (a <= 0.0f) continue;
        uint8_t display[3];
        for (int c = 0; c < 3; ++c) {
            const float mapped = Filmic(premultiplied[i + c] / a * exposure);
            display[c] = table[static_cast<size_t>(mapped * 4096.0f + 0.5f)];
        }
        out[i + 0] = static_cast<uint8_t>(std::lround(display[2] * a));
        out[i + 1] = static_cast<uint8_t>(std::lround(display[1] * a));
        out[i + 2] = static_cast<uint8_t>(std::lround(display[0] * a));
        out[i + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
    return out;
}

} // namespace imgpack
