#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"
#include "ColorTheme.h"
#include "FramePresentation.h"

using namespace Mandelbrot;

TEST_CASE(SubpixelSampling, CenterValuesArePreservedAndQuarterPixelsMatchAFullGrid)
{
    for (auto type : GetAllFractalTypes())
        for (bool deep : { false, true })
        for (bool smooth : { false, true })
        {
            MandelbrotConfig config;
            config.fractalType = type;
            config.smooth = smooth;
            config.width = 32; config.height = 24; config.maxIterations = 400;
            if (deep)
            {
                config.zoom = 1e25;
                config.SetCenter(type == FractalType::Newton ? BigFloat() : BigFloat(-2.0) + BigFloat(1e-25), BigFloat(1e-25));
            }
            MandelbrotEngine engine;
            std::vector<float> centers, baseline, high;
            std::vector<SubpixelSamples> samples;
            engine.Compute(config, centers, nullptr, &samples);
            engine.Compute(config, baseline);
            ASSERT_TRUE(centers == baseline);
            auto doubled = config;
            doubled.width *= 2; doubled.height *= 2;
            engine.Compute(doubled, high);
            size_t previous = 0;
            for (size_t i = 0; i < samples.size(); ++i)
            {
                const auto& sample = samples[i];
                ASSERT_LT(sample.pixelIndex, centers.size());
                if (i > 0) ASSERT_GT(sample.pixelIndex, previous);
                previous = sample.pixelIndex;
                ASSERT_GT(sample.weight, 0.0f);
                ASSERT_LE(sample.weight, 1.0f);
                const int x = sample.pixelIndex % config.width, y = sample.pixelIndex / config.width;
                for (int s = 0; s < 4; ++s)
                {
                    const auto expected = high[static_cast<size_t>(y * 2 + s / 2) * doubled.width + x * 2 + s % 2];
                    ASSERT_NEAR(expected, sample.iterations[s], 0.001f);
                }
            }
            if (!deep) ASSERT_FALSE(samples.empty());
        }
}

TEST_CASE(SubpixelSampling, FlatRegionsSkipExtraSamplesAndCancellationClearsOldSamples)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.width = 32; config.height = 24; config.zoom = 100000;
    std::vector<float> centers;
    std::vector<SubpixelSamples> samples(3);
    config.SetCenter(0.0, 0.0);
    engine.Compute(config, centers, nullptr, &samples);
    ASSERT_TRUE(samples.empty());
    config.SetCenter(20.0, 20.0);
    engine.Compute(config, centers, nullptr, &samples);
    ASSERT_TRUE(samples.empty());
    samples.resize(3);
    std::atomic<bool> cancel{ true };
    engine.Compute(config, centers, &cancel, &samples);
    ASSERT_TRUE(samples.empty());
    samples.resize(3);
    config.width = 0;
    engine.Compute(config, centers, nullptr, &samples);
    ASSERT_TRUE(samples.empty());
}

TEST_CASE(SubpixelSampling, RepeatedFramesUseIdenticalMasksAndExactSamples)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.width = 65; config.height = 49; config.renderStride = 2;
    config.SetCenter(-0.7436438870371587, 0.1318259042053119);
    config.zoom = 85000; config.maxIterations = 1500;
    std::vector<float> a, b;
    std::vector<SubpixelSamples> first, second;
    engine.Compute(config, a, nullptr, &first);
    engine.Compute(config, b, nullptr, &second);
    ASSERT_TRUE(a == b);
    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    for (size_t i = 0; i < first.size(); ++i)
    {
        ASSERT_EQ(first[i].pixelIndex, second[i].pixelIndex);
        ASSERT_EQ(first[i].weight, second[i].weight);
        ASSERT_TRUE(first[i].iterations == second[i].iterations);
    }
}

TEST_CASE(SubpixelSampling, InteriorCoverageIsAveragedAfterColoringInLinearLight)
{
    ColorTheme theme;
    theme.SetStops({ {0.0f,255,255,255}, {1.0f,255,255,255} });
    std::vector<float> centers{ -1.0f };
    std::vector<SubpixelSamples> samples{ {0, 1.0f, {-1.0f,-1.0f,-1.0f,1000.0f}} };
    uint32_t color;
    theme.Recolor(centers, &color, 1, 1.0f, 0.0f, false, 0xFF000000, FractalType::Mandelbrot, 1, &samples);
    // One quarter white coverage encodes to sRGB 137, rather than a dark gamma-space average.
    ASSERT_EQ(0xFF898989u, color);
    samples[0].weight = 0.5f;
    theme.Recolor(centers, &color, 1, 1.0f, 0.0f, false, 0xFF000000, FractalType::Mandelbrot, 1, &samples);
    ASSERT_EQ(0xFF636363u, color);
    samples[0].pixelIndex = 1; // A stale/out-of-range sparse entry cannot overwrite another pixel.
    theme.Recolor(centers, &color, 1, 1.0f, 0.0f, false, 0xFF000000, FractalType::Mandelbrot, 1, &samples);
    ASSERT_EQ(0xFF000000u, color);
}

TEST_CASE(SubpixelSampling, NewtonBasinsAndLivePaletteSettingsAreColoredIndependently)
{
    ColorTheme theme(PresetTheme::Inferno);
    std::vector<float> centers{ -1.0f };
    std::vector<SubpixelSamples> samples{ {0, 1.0f, {-1.0f,0.0f,1000000.0f,2000000.0f}} };
    auto decode = [](double s) { s /= 255.0; return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4); };
    for (float phase : { 0.0f, 0.25f })
        for (bool invert : { false, true })
        {
            uint32_t actual;
            theme.Recolor(centers, &actual, 1, 2.0f, phase, invert, 0xFF102030, FractalType::Newton, 1, &samples);
            uint32_t individual[4];
            for (int s = 0; s < 4; ++s)
                theme.Recolor(std::vector<float>{samples[0].iterations[s]}, &individual[s], 1,
                    2.0f, phase, invert, 0xFF102030, FractalType::Newton, 1);
            for (int c = 0; c < 3; ++c)
            {
                double linear = 0.0;
                for (auto color : individual) linear += decode((color >> (8 * c)) & 255) * 0.25;
                double encoded = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
                ASSERT_NEAR(encoded * 255.0, (actual >> (8 * c)) & 255, 1.0);
            }
        }
}

TEST_CASE(SubpixelSampling, FourSamplesReduceCameraAlignedSubpixelColorVariation)
{
    double baselineError = 0.0, refinedError = 0.0;
    for (double zoom : { 85000.0, 500000.0 })
    {
        MandelbrotConfig view;
        view.width = 256; view.height = 192; view.zoom = zoom; view.maxIterations = 2000; view.renderStride = 2;
        view.SetCenter(-0.7436438870371587, 0.1318259042053119);
        auto next = view;
        next.SetCenter(view.centerX + 0.25 * 3.0 / zoom / view.width, view.centerY);
        MandelbrotEngine engine;
        ColorTheme theme;
        std::vector<uint32_t> oldBase, oldRefined;
        for (int f = 0; f < 2; ++f)
        {
            const auto& config = f == 0 ? view : next;
            std::vector<float> centers;
            std::vector<SubpixelSamples> samples;
            engine.Compute(config, centers, nullptr, &samples);
            std::vector<uint32_t> base(centers.size()), refined(centers.size());
            theme.Recolor(centers, base.data(), base.size(), 1.0f, 0.0f, false, 0xFF000000, config.fractalType, config.width);
            theme.Recolor(centers, refined.data(), refined.size(), 1.0f, 0.0f, false, 0xFF000000, config.fractalType, config.width, &samples);
            if (f == 0) { oldBase = std::move(base); oldRefined = std::move(refined); continue; }
            const auto alignedBase = SnapshotViewport(reinterpret_cast<const uint8_t*>(oldBase.data()), view, next);
            const auto alignedRefined = SnapshotViewport(reinterpret_cast<const uint8_t*>(oldRefined.data()), view, next);
            const auto* newBase = reinterpret_cast<const uint8_t*>(base.data());
            const auto* newRefined = reinterpret_cast<const uint8_t*>(refined.data());
            for (int y = 3; y < view.height - 3; ++y)
                for (int x = 3; x < view.width - 3; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        const size_t i = (static_cast<size_t>(y) * view.width + x) * 4 + c;
                        baselineError += std::abs(static_cast<int>(alignedBase[i]) - newBase[i]);
                        refinedError += std::abs(static_cast<int>(alignedRefined[i]) - newRefined[i]);
                    }
        }
    }
    ASSERT_LT(refinedError, baselineError * 0.90);
}
