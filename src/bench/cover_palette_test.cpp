// cover_palette_test - colours of a cover for the Quick Look audio waveform.
#include "../ui/cover_palette.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace pulse::ui;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}
struct Image {
    uint32_t w, h;
    std::vector<uint8_t> px;
    Image(uint32_t w_, uint32_t h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 0) {}
    void Fill(uint32_t x0, uint32_t x1, uint32_t rgb, uint8_t a = 255) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = static_cast<uint8_t>((rgb & 255) * a / 255); p[1] = static_cast<uint8_t>(((rgb >> 8) & 255) * a / 255);
                p[2] = static_cast<uint8_t>(((rgb >> 16) & 255) * a / 255); p[3] = a;
            }
    }
    CoverPalette Palette() const { return ComputeCoverPalette(px.data(), w, h, w * 4); }
};
int Hue(uint32_t rgb) {   // coarse: 0 red, 1 yellow/orange, 2 green, 3 cyan, 4 blue, 5 magenta
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (r >= g && r >= b) return g > b + 40 ? 1 : (b > g + 40 ? 5 : 0);
    if (g >= r && g >= b) return b > r + 40 ? 3 : (r > b + 40 ? 1 : 2);
    return r > g + 40 ? 5 : (g > r + 40 ? 3 : 4);
}
int Light(uint32_t rgb) { return (((rgb >> 16) & 255) * 3 + ((rgb >> 8) & 255) * 6 + (rgb & 255)) / 10; }
} // namespace

int main() {
    Image grey(96, 96);
    grey.Fill(0, 48, 0x202020); grey.Fill(48, 96, 0xD0D0D0);
    Check(!grey.Palette().valid, "black-and-white cover has no palette");

    Image red_blue(120, 120);   // red left, blue right
    red_blue.Fill(0, 60, 0xD02020); red_blue.Fill(60, 120, 0x2040D0);
    const CoverPalette rb = red_blue.Palette();
    Check(rb.valid && Hue(rb.colors[0]) == 0 && Hue(rb.colors[2]) == 4, "two hues ordered left to right");
    Check(rb.valid && Hue(rb.colors[1]) == 5, "two hues blend through the hue between, not grey");

    Image sunset(150, 90);      // orange, pink-red, purple
    sunset.Fill(0, 50, 0xF0A030); sunset.Fill(50, 100, 0xE04060); sunset.Fill(100, 150, 0x7040B0);
    const CoverPalette sp = sunset.Palette();
    Check(sp.valid && Hue(sp.colors[0]) == 1 && Hue(sp.colors[1]) == 0 && (Hue(sp.colors[2]) == 4 || Hue(sp.colors[2]) == 5), "three hues keep the cover's order");

    Image mostly_grey(100, 100);   // a small red badge on grey
    mostly_grey.Fill(0, 100, 0x808080); mostly_grey.Fill(0, 6, 0xE02020);
    Check(!mostly_grey.Palette().valid, "a few coloured pixels are not the cover's colour");

    Image teal(64, 64);
    teal.Fill(0, 64, 0x1A9A8A);
    const CoverPalette tp = teal.Palette();
    Check(tp.valid && Hue(tp.colors[1]) == 3 && tp.colors[0] != tp.colors[2], "one hue becomes a gentle gradient");

    Image clear(64, 64);
    clear.Fill(0, 64, 0xE02020, 40);
    Check(!clear.Palette().valid, "transparent pixels are ignored");
    Check(!ComputeCoverPalette(nullptr, 10, 10, 40).valid, "no pixels, no palette");

    const uint32_t navy = 0x101850;
    Check(Light(CoverPaletteThemed(navy, true)) > 110, "dark theme lifts deep colours");
    Check(Light(CoverPaletteThemed(0xFFE8A0, false)) < 200, "light theme deepens pale colours");
    std::printf("cover_palette_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
