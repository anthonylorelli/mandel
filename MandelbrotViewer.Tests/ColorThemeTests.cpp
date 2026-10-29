#include "pch.h"
#include "TestHarness.h"
#include "ColorTheme.h"
#include "MandelbrotEngine.h"

using namespace Mandelbrot;

TEST_CASE(ColorTheme, Presets_CountAndNames)
{
    const auto& presets = GetAllPresets();
    ASSERT_GE(static_cast<int>(presets.size()), 8);

    for (auto preset : presets)
    {
        std::wstring name = GetPresetName(preset);
        ASSERT_FALSE(name.empty());
    }
}

TEST_CASE(ColorTheme, Presets_GradientStopsIntegrity)
{
    for (auto preset : GetAllPresets())
    {
        const auto stops = GetPresetStops(preset);
        ASSERT_GE(static_cast<int>(stops.size()), 2);

        float prevPos = -0.01f;
        for (const auto& stop : stops)
        {
            ASSERT_GE(stop.position, 0.0f);
            ASSERT_LE(stop.position, 1.0f);
            ASSERT_GE(stop.position, prevPos);
            prevPos = stop.position;
        }

        // First stop should be at or near 0.0; last stop at or near 1.0
        ASSERT_NEAR(0.0f, stops.front().position, 0.05f);
        ASSERT_NEAR(1.0f, stops.back().position, 0.05f);
    }
}

TEST_CASE(ColorTheme, Lut_Generation)
{
    ColorTheme theme(PresetTheme::ElectricBlue);
    const auto& lut = theme.Lut();

    ASSERT_EQ(ColorTheme::LUT_SIZE, lut.size());

    // All colors must have fully opaque alpha (0xFF in the upper 8 bits)
    for (uint32_t color : lut)
    {
        uint32_t alpha = (color >> 24) & 0xFF;
        ASSERT_EQ(0xFFu, alpha);
    }
}

TEST_CASE(ColorTheme, CustomStops_AddRemoveUpdate)
{
    ColorTheme theme(PresetTheme::Monochrome);
    size_t initialStops = theme.Stops().size();

    // Add stop
    theme.AddStop(0.5f, 255, 0, 128);
    ASSERT_EQ(initialStops + 1, theme.Stops().size());
    ASSERT_EQ(static_cast<int>(PresetTheme::Custom), static_cast<int>(theme.CurrentPreset()));

    // Update stop
    size_t lastIdx = theme.Stops().size() - 1;
    theme.UpdateStop(lastIdx, 0.5f, 100, 200, 50);
    ASSERT_EQ(100, theme.Stops()[lastIdx].r);
    ASSERT_EQ(200, theme.Stops()[lastIdx].g);
    ASSERT_EQ(50, theme.Stops()[lastIdx].b);

    // Remove stop
    theme.RemoveStop(lastIdx);
    ASSERT_EQ(initialStops, theme.Stops().size());

    // Should not allow removing below 2 stops
    while (theme.Stops().size() > 2)
    {
        theme.RemoveStop(theme.Stops().size() - 1);
    }
    ASSERT_EQ(static_cast<size_t>(2), theme.Stops().size());
    theme.RemoveStop(0); // Attempt to remove below 2
    ASSERT_EQ(static_cast<size_t>(2), theme.Stops().size());
}

TEST_CASE(ColorTheme, Recolor_InteriorPoints)
{
    ColorTheme theme(PresetTheme::ElectricBlue);
    std::vector<float> iterBuffer = { -1.0f, -1.0f, -1.0f };
    std::vector<uint32_t> pixels(3, 0);

    const uint32_t interiorColor = 0xFF123456;
    theme.Recolor(iterBuffer, pixels.data(), pixels.size(), 1.0f, 0.0f, false, interiorColor);

    for (uint32_t p : pixels)
    {
        ASSERT_EQ(interiorColor, p);
    }
}

TEST_CASE(ColorTheme, Recolor_CyclicLUT)
{
    ColorTheme theme(PresetTheme::ElectricBlue);
    std::vector<float> iterBuffer = { 0.25f, 5.33f, 12.7f };
    std::vector<uint32_t> pixelsA(3, 0);
    std::vector<uint32_t> pixelsB(3, 0);

    // Baseline coloring
    theme.Recolor(iterBuffer, pixelsA.data(), pixelsA.size(), 1.0f, 0.0f, false);

    // Phase shifted coloring should produce different colors
    theme.Recolor(iterBuffer, pixelsB.data(), pixelsB.size(), 1.0f, 0.5f, false);

    ASSERT_NE(pixelsA[0], pixelsB[0]);

    // Inverted coloring should also differ
    std::vector<uint32_t> pixelsInv(3, 0);
    theme.Recolor(iterBuffer, pixelsInv.data(), pixelsInv.size(), 1.0f, 0.0f, true);
    ASSERT_NE(pixelsA[0], pixelsInv[0]);
}

TEST_CASE(ColorTheme, Recolor_NewtonMultiBasin)
{
    ColorTheme theme(PresetTheme::Inferno);

    // 3 points corresponding to Root 0, Root 1, and Root 2 with iteration 10
    float root0Point = 0 * 1000000.0f + 10.0f;
    float root1Point = 1 * 1000000.0f + 10.0f;
    float root2Point = 2 * 1000000.0f + 10.0f;

    std::vector<float> iterBuffer = { root0Point, root1Point, root2Point };
    std::vector<uint32_t> pixels(3, 0);

    theme.Recolor(iterBuffer, pixels.data(), pixels.size(), 1.0f, 0.0f, false, 0xFF000000, FractalType::Newton);

    // All 3 root points must receive different colors because they map to different 1/3 sectors
    ASSERT_NE(pixels[0], pixels[1]);
    ASSERT_NE(pixels[1], pixels[2]);
    ASSERT_NE(pixels[0], pixels[2]);
}

TEST_CASE(ColorTheme, GeneratePreview_SizeAndContent)
{
    ColorTheme theme(PresetTheme::SunsetGlow);
    const size_t width = 256;
    std::vector<uint32_t> preview(width, 0);

    theme.GeneratePreview(preview.data(), width);

    // Preview should be fully populated with non-zero opaque pixels
    for (uint32_t p : preview)
    {
        uint32_t alpha = (p >> 24) & 0xFF;
        ASSERT_EQ(0xFFu, alpha);
    }

    // Start of preview should differ from end of preview
    ASSERT_NE(preview.front(), preview.back());
}

TEST_CASE(ColorTheme, GeneratePreview_SinglePixelAndEmpty)
{
    ColorTheme theme(PresetTheme::SunsetGlow);
    uint32_t pixel = 0;
    theme.GeneratePreview(&pixel, 1);
    ASSERT_EQ(theme.Lut().front(), pixel);
    theme.GeneratePreview(&pixel, 0);
    theme.GeneratePreview(nullptr, 1);
    ASSERT_EQ(theme.Lut().front(), pixel);
}

TEST_CASE(ColorTheme, Recolor_HorizontalNeighborsStayWithinRows)
{
    ColorTheme theme(PresetTheme::Monochrome);
    // Each row has a flat plateau at both ends. Only the unrelated next row's first
    // pixel differs at the scanline seam, so it must not blur the preceding pixel.
    for (int width : { 1, 8, 11 })
    {
        std::vector<float> iterations(64 * width, 10.0f);
        for (size_t i = 0; i < iterations.size(); ++i)
        {
            if (i % width < width / 2) iterations[i] = 10000.0f;
        }
        std::vector<uint32_t> pixels(iterations.size());
        theme.Recolor(iterations, pixels.data(), pixels.size(), 1.0f, 0.0f, false,
            0xFF000000, FractalType::Mandelbrot, width);
        std::vector<float> flat{ 10.0f };
        uint32_t expected = 0;
        theme.Recolor(flat, &expected, 1, 1.0f, 0.0f, false);
        for (size_t i = width - 1; i < pixels.size(); i += width)
        {
            ASSERT_EQ(expected, pixels[i]);
        }
    }
}

TEST_CASE(ColorTheme, Recolor_PartialFinalRowDoesNotWrapLeft)
{
    ColorTheme theme(PresetTheme::Inferno);
    std::vector<float> iterations(65, 12.0f);
    iterations[63] = 10000.0f;
    std::vector<uint32_t> pixels(iterations.size());
    theme.Recolor(iterations, pixels.data(), pixels.size(), 1.0f, 0.0f, false,
        0xFF000000, FractalType::Mandelbrot, 8);
    uint32_t expected = 0;
    theme.Recolor(std::vector<float>{ 12.0f }, &expected, 1, 1.0f, 0.0f, false);
    ASSERT_EQ(expected, pixels.back());
}

TEST_CASE(ColorTheme, HighIteration_LogCompressionAndAntiAliasing)
{
    // 1. Logarithmic compression: +50 iterations at iter=2000 must advance palette phase by a small fraction
    // of a cycle (< 0.05) instead of 50 full cycles, and doubling at high iterations must be scale-invariant.
    const float phase2000 = ColorTheme::MapIterationToPhase(2000.0f, 1.0f);
    const float phase2050 = ColorTheme::MapIterationToPhase(2050.0f, 1.0f);
    ASSERT_LT(phase2050 - phase2000, 0.05f);

    const float span1000To2000 = ColorTheme::MapIterationToPhase(2000.0f, 1.0f) - ColorTheme::MapIterationToPhase(1000.0f, 1.0f);
    const float span5000To10000 = ColorTheme::MapIterationToPhase(10000.0f, 1.0f) - ColorTheme::MapIterationToPhase(5000.0f, 1.0f);
    ASSERT_NEAR(span1000To2000, span5000To10000, 0.015f);

    // 2. Band-limited anti-aliasing: when adjacent pixels have extreme iteration jumps (above Nyquist),
    // Recolor damps the high-frequency oscillation toward the palette mean rather than producing raw static.
    ColorTheme theme(PresetTheme::Monochrome); // Black (0) at 0.0/1.0, White (240) at 0.5, mean ~ 120
    std::vector<float> noisyIters(64, 100.0f);
    for (size_t i = 0; i < noisyIters.size(); ++i)
    {
        noisyIters[i] = (i % 2 == 0) ? 100.0f : 5000.0f;
    }

    std::vector<uint32_t> pixels(64, 0);
    theme.Recolor(noisyIters, pixels.data(), pixels.size(), 1.0f, 0.0f, false, 0xFF000000, FractalType::Mandelbrot, 8);

    const int r0 = static_cast<int>((pixels[0] >> 16) & 0xFF);
    const int r1 = static_cast<int>((pixels[1] >> 16) & 0xFF);
    // Both high-gradient pixels should be damped into the mid-tone range (neither pure 0 nor pure 240)
    ASSERT_GT(r0, 40);
    ASSERT_LT(r0, 200);
    ASSERT_GT(r1, 40);
    ASSERT_LT(r1, 200);
}
