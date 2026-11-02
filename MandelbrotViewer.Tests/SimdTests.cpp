#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"
#include <bit>
#include <random>

using namespace Mandelbrot;

namespace
{
    void AssertSameFrame(const MandelbrotConfig& config, bool refine = false)
    {
        MandelbrotEngine scalar(MandelbrotEngine::ExecutionMode::Scalar), automatic;
        std::vector<float> expected, actual;
        std::vector<SubpixelSamples> expectedSamples, actualSamples;
        scalar.Compute(config, expected, nullptr, refine ? &expectedSamples : nullptr);
        automatic.Compute(config, actual, nullptr, refine ? &actualSamples : nullptr);
        ASSERT_EQ(expected.size(), actual.size());
        for (size_t i = 0; i < expected.size(); ++i)
            ASSERT_EQ(std::bit_cast<uint32_t>(expected[i]), std::bit_cast<uint32_t>(actual[i]));
        ASSERT_EQ(expectedSamples.size(), actualSamples.size());
        for (size_t i = 0; i < expectedSamples.size(); ++i)
        {
            ASSERT_EQ(expectedSamples[i].pixelIndex, actualSamples[i].pixelIndex);
            ASSERT_EQ(std::bit_cast<uint32_t>(expectedSamples[i].weight), std::bit_cast<uint32_t>(actualSamples[i].weight));
            for (int s = 0; s < 4; ++s)
                ASSERT_EQ(std::bit_cast<uint32_t>(expectedSamples[i].iterations[s]),
                    std::bit_cast<uint32_t>(actualSamples[i].iterations[s]));
        }
    }
}

TEST_CASE(Simd, AutomaticMatchesScalarAcrossBookmarksAndRandomViews)
{
    for (auto type : GetAllFractalTypes())
        for (const auto& bookmark : GetBookmarks(type))
            for (bool smooth : { false, true })
            {
                MandelbrotConfig config;
                config.fractalType = type;
                config.width = 257; config.height = 145;
                config.SetCenter(bookmark.centerX, bookmark.centerY);
                config.zoom = bookmark.zoom;
                config.maxIterations = bookmark.maxIterations;
                config.smooth = smooth;
                AssertSameFrame(config);
            }

    std::mt19937_64 rng(101);
    std::uniform_real_distribution<double> x(-2.1, 0.9), y(-1.3, 1.3), depth(0, 10);
    for (int i = 0; i < 32; ++i)
    {
        MandelbrotConfig config;
        config.width = 129; config.height = 97;
        config.SetCenter(x(rng), y(rng));
        config.zoom = std::pow(10.0, depth(rng));
        config.maxIterations = 100 + (i % 5) * 500;
        config.smooth = i % 2 == 0;
        AssertSameFrame(config);
    }
}

TEST_CASE(Simd, OddTailsTinyImagesAndIterationLimitsMatchScalar)
{
    for (auto size : { std::pair{1, 1}, std::pair{1, 9}, std::pair{2, 3},
        std::pair{3, 3}, std::pair{256, 144}, std::pair{257, 145} })
        for (int limit : { 0, 1, 2, 8, 9, 500 })
            for (bool smooth : { false, true })
            {
                MandelbrotConfig config;
                config.width = size.first; config.height = size.second;
                config.maxIterations = limit; config.smooth = smooth;
                AssertSameFrame(config);
            }
}

TEST_CASE(Simd, PeriodicInteriorsDetailedEdgesAndSubpixelsMatchScalar)
{
    for (auto center : { std::pair{-0.122561166876, 0.744861766619},
        std::pair{-1.754877666, 0.0}, std::pair{-0.7436438870371587, 0.1318259042053119} })
        for (int stride : { 1, 2, 4 })
            for (bool smooth : { false, true })
            {
                MandelbrotConfig config;
                config.width = 257; config.height = 145;
                config.SetCenter(center.first, center.second);
                config.zoom = 10000; config.maxIterations = 3000;
                config.renderStride = stride; config.smooth = smooth;
                AssertSameFrame(config, true);
            }
}

TEST_CASE(Simd, JuliaAndDeepZoomKeepTheirScalarResults)
{
    for (bool julia : { false, true })
        for (double zoom : { 85000.0, MandelbrotEngine::DeepZoomThreshold, 1e25 })
        {
            MandelbrotConfig config;
            config.width = 65; config.height = 49;
            config.SetCenter(0.0, 1.0);
            config.zoom = zoom; config.maxIterations = 1000;
            config.juliaSet = julia;
            config.juliaX = BigFloat(-0.7436438870371587);
            config.juliaY = BigFloat(0.1318259042053119);
            AssertSameFrame(config, true);
        }
}

TEST_CASE(Simd, CanceledFramesClearSubpixelSamples)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.width = 65; config.height = 49;
    config.SetCenter(-0.7436438870371587, 0.1318259042053119);
    config.zoom = 85000; config.maxIterations = 1500;
    std::vector<float> values;
    std::vector<SubpixelSamples> samples;
    engine.Compute(config, values, nullptr, &samples);
    ASSERT_FALSE(samples.empty());
    std::atomic<bool> cancel{ true };
    engine.Compute(config, values, &cancel, &samples);
    ASSERT_TRUE(samples.empty());
}
