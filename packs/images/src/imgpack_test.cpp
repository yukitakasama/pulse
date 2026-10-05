// imgpack_test - pixel arithmetic of the image preview pack (pixel_ops.h).
#include "pixel_ops.h"
#include <cstdio>

using namespace imgpack;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}
bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
} // namespace

int main() {
    Check(FitSize(4000, 3000, 256) == Size{256, 192}, "landscape fits the cap");
    Check(FitSize(1000, 4000, 512) == Size{128, 512}, "portrait fits the cap");
    Check(FitSize(100, 80, 512) == Size{100, 80}, "small pictures are never enlarged");
    Check(FitSize(100000, 1, 256) == Size{256, 1}, "extreme aspect keeps one pixel");
    Check(FitSize(0, 10, 256) == Size{}, "empty source");

    {   // 4x2 -> 2x1: each output pixel averages a 2x2 block.
        ByteDownscaler d(4, 2, 2, 1);
        const uint8_t r0[] = {255,0,0,255, 255,0,0,255, 0,0,255,255, 0,0,255,0};
        const uint8_t r1[] = {255,0,0,255, 255,0,0,255, 0,0,255,255, 0,0,255,0};
        d.AddRow(0, r0, 4); d.AddRow(1, r1, 4);
        const auto out = d.Finish();
        Check(out.size() == 8 && out[2] == 255 && out[0] == 0 && out[3] == 255, "opaque red block stays red (BGRA)");
        Check(out[4] == 128 && out[7] == 128 && out[6] == 0, "half transparent blue block is premultiplied");
    }
    {   // RGB input is opaque.
        ByteDownscaler d(2, 1, 1, 1);
        const uint8_t row[] = {10,20,30, 30,40,50};
        d.AddRow(0, row, 3);
        const auto out = d.Finish();
        Check(out[0] == 40 && out[1] == 30 && out[2] == 20 && out[3] == 255, "RGB rows average opaque");
    }
    {   // Float: a NaN alpha counts as transparent.
        FloatDownscaler d(2, 1, 1, 1);
        const float row[] = {1.0f, 1.0f, 1.0f, 1.0f,   9.0f, 9.0f, 9.0f, NAN};
        d.AddRow(0, row);
        const auto out = d.Finish();
        Check(Near(out[0], 0.5f, 1e-6f) && Near(out[3], 0.5f, 1e-6f), "NaN alpha is transparent");
    }
    {
        FloatDownscaler d(2, 1, 1, 1);
        const float row[] = {2.0f, 1.0f, -5.0f, 1.0f,   4.0f, 0.0f, 0.0f, 0.5f};
        d.AddRow(0, row);
        const auto out = d.Finish();
        Check(Near(out[0], (2.0f + 2.0f) / 2, 1e-5f) && Near(out[2], 0.0f, 1e-6f) && Near(out[3], 0.75f, 1e-6f),
              "float rows premultiply and drop negative light");
    }
    Check(Near(PqToNits(0.0f), 0.0f, 1e-3f) && Near(PqToNits(1.0f), 10000.0f, 1.0f), "PQ endpoints");
    Check(Near(PqToNits(0.58f), 203.0f, 8.0f), "PQ 0.58 is about reference white");
    Check(Near(HlgToLinear(0.5f), 1.0f / 12.0f, 1e-5f) && Near(HlgToLinear(1.0f), 1.0f, 1e-3f), "HLG knee and peak");
    {
        float r = 1, g = 1, b = 1;
        Bt2020To709(r, g, b);
        Check(Near(r, 1, 0.002f) && Near(g, 1, 0.002f) && Near(b, 1, 0.002f), "white stays white across primaries");
    }
    {
        std::vector<float> grey = {0.02f, 0.02f, 0.02f, 1.0f, 0.02f, 0.02f, 0.02f, 1.0f};
        Check(Near(AutoExposure(grey), 9.0f, 0.1f), "dark picture is brightened to middle grey");
        std::vector<float> clear = {5, 5, 5, 0};
        Check(AutoExposure(clear) == 1.0f, "fully transparent picture keeps exposure 1");
        std::vector<float> sun = {1e6f, 1e6f, 1e6f, 1.0f};
        Check(Near(AutoExposure(sun), 1.0f / 64.0f, 1e-6f), "exposure is clamped to -6 stops");
    }
    Check(Filmic(0.0f) == 0.0f && Filmic(1e6f) == 1.0f && Filmic(0.5f) > Filmic(0.18f), "filmic curve is monotonic 0..1");
    {
        std::vector<float> px = {0.18f, 0.18f, 0.18f, 1.0f,   0.0f, 0.0f, 0.0f, 0.0f,   100.0f, 0.0f, 0.0f, 1.0f};
        const auto out = ToneMapToBgra(px, 1.0f);
        Check(out[3] == 255 && out[0] > 110 && out[0] < 160, "middle grey maps near the middle");
        Check(out[4] == 0 && out[7] == 0, "transparent stays transparent");
        Check(out[10] == 255 && out[8] == 0, "a very bright red clips softly to red (BGRA)");
    }
    std::printf("imgpack_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
