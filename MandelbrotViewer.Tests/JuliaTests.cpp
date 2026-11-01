#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"
#include "JuliaView.h"
#include "FramePresentation.h"
#include "ViewState.h"
#include "BackgroundRenderer.h"
#include <condition_variable>
#include <mutex>

using namespace Mandelbrot;

namespace
{
    MandelbrotConfig JuliaConfig(double cx, double cy)
    {
        MandelbrotConfig config;
        config.juliaSet = true;
        config.juliaX = BigFloat(cx); config.juliaY = BigFloat(cy);
        config.SetCenter(0.0, 0.0);
        config.width = 64; config.height = 48; config.zoom = 0.75; config.maxIterations = 200;
        return config;
    }

    float QuadraticOrbit(double zr, double zi, double cx, double cy, int maxIter)
    {
        const double bailout = std::max(16.0, 4.0 * std::hypot(cx, cy));
        for (int i = 0; i < maxIter; ++i)
        {
            if (zr * zr + zi * zi > bailout) return static_cast<float>(i);
            const double next = zr * zr - zi * zi + cx;
            zi = 2.0 * zr * zi + cy;
            zr = next;
        }
        return -1.0f;
    }
}

TEST_CASE(Julia, ZeroParameterProducesTheFilledUnitDisk)
{
    auto config = JuliaConfig(0.0, 0.0);
    MandelbrotEngine engine;
    std::vector<float> pixels;
    engine.Compute(config, pixels);
    for (int y = 0; y < config.height; ++y)
        for (int x = 0; x < config.width; ++x)
        {
            double zr, zi;
            MandelbrotEngine::ScreenToComplex(x, y, config.width, config.height, 0, 0, config.zoom, zr, zi);
            ASSERT_EQ(zr * zr + zi * zi < 1.0, pixels[static_cast<size_t>(y) * config.width + x] < 0);
        }
}

TEST_CASE(Julia, FixedParameterAndVaryingInitialPointMatchQuadraticOrbits)
{
    MandelbrotEngine engine;
    for (auto parameter : { std::pair{-1.0,0.0}, std::pair{-0.8,0.156}, std::pair{3.0,2.0} })
    {
        auto config = JuliaConfig(parameter.first, parameter.second);
        config.smooth = false;
        std::vector<float> pixels;
        engine.Compute(config, pixels);
        for (int y = 0; y < config.height; ++y)
            for (int x = 0; x < config.width; ++x)
            {
                double zr, zi;
                MandelbrotEngine::ScreenToComplex(x, y, config.width, config.height, 0, 0, config.zoom, zr, zi);
                ASSERT_EQ(QuadraticOrbit(zr, zi, parameter.first, parameter.second, config.maxIterations),
                    pixels[static_cast<size_t>(y) * config.width + x]);
            }
    }
}

TEST_CASE(Julia, RepeatingOrbitShortcutPreservesHighIterationResults)
{
    MandelbrotEngine engine;
    for (auto c : { std::pair{0.0, 0.0}, std::pair{-1.0, 0.0},
        std::pair{-0.12, 0.65}, std::pair{-0.75, 0.0} })
    {
        auto config = JuliaConfig(c.first, c.second);
        config.width = 24; config.height = 16;
        config.maxIterations = 10000;
        config.smooth = false;
        std::vector<float> pixels;
        engine.Compute(config, pixels);
        for (int y = 0; y < config.height; ++y)
            for (int x = 0; x < config.width; ++x)
            {
                double zr, zi;
                MandelbrotEngine::ScreenToComplex(x, y, config.width, config.height, 0, 0, config.zoom, zr, zi);
                ASSERT_EQ(QuadraticOrbit(zr, zi, c.first, c.second, config.maxIterations),
                    pixels[static_cast<size_t>(y) * config.width + x]);
            }
    }
}

TEST_CASE(Julia, DeepZoomResolvesPointsOnOppositeSidesOfTheUnitCircle)
{
    auto config = JuliaConfig(0.0, 0.0);
    config.SetCenter(1.0, 0.0);
    config.zoom = 1e25; config.width = 8; config.height = 4; config.maxIterations = 300;
    MandelbrotEngine engine;
    std::vector<float> pixels;
    std::vector<SubpixelSamples> samples;
    engine.Compute(config, pixels, nullptr, &samples);
    ASSERT_FALSE(samples.empty());
    for (int y = 0; y < config.height; ++y)
        for (int x = 0; x < config.width; ++x)
            ASSERT_EQ(x < config.width / 2, pixels[y * config.width + x] < 0);
    for (auto value : pixels) ASSERT_TRUE(std::isfinite(value));
    std::atomic<bool> cancel{true};
    engine.Compute(config, pixels, &cancel, &samples);
    ASSERT_TRUE(samples.empty());
}

TEST_CASE(Julia, DeepJuliaRetainsParameterChangesSmallerThanDoubleCoordinateSpacing)
{
    auto config = JuliaConfig(0.0, 0.0);
    config.juliaX = BigFloat(1e-25);
    config.SetCenter(1.0, 0.0);
    config.zoom = 1e25; config.width = 8; config.height = 4; config.maxIterations = 300;
    MandelbrotEngine engine;
    std::vector<float> pixels;
    engine.Compute(config, pixels);
    // The repelling fixed point moves from 1 to approximately 1-c. Only the leftmost
    // column lies inside this shifted Julia boundary; rounding c into z would lose it.
    for (int y = 0; y < config.height; ++y)
        for (int x = 0; x < config.width; ++x)
            ASSERT_EQ(x == 0, pixels[y * config.width + x] < 0);
}

TEST_CASE(Julia, CriticalPointDisplacementsSurviveDoubleExponentUnderflow)
{
    auto config = JuliaConfig(-2.0, 0.0);
    config.zoom = 1e200; config.width = 2; config.height = 2; config.maxIterations = 1000;
    MandelbrotEngine engine;
    std::vector<float> pixels;
    engine.Compute(config, pixels);
    // K(-2) is the real interval [-2,2]. Every sample here has a nonzero imaginary
    // component, so it must escape even though its first squared displacement is ~1e-400.
    for (auto value : pixels)
    {
        ASSERT_GE(value, 0.0f);
        ASSERT_TRUE(std::isfinite(value));
    }
}

TEST_CASE(Julia, SupersamplesMatchAFullDoubleResolutionJuliaGrid)
{
    auto config = JuliaConfig(-1.0, 0.0);
    MandelbrotEngine engine;
    std::vector<float> pixels, full;
    std::vector<SubpixelSamples> samples;
    engine.Compute(config, pixels, nullptr, &samples);
    auto doubled = config;
    doubled.width *= 2; doubled.height *= 2;
    engine.Compute(doubled, full);
    ASSERT_FALSE(samples.empty());
    for (const auto& sample : samples)
    {
        const int x = sample.pixelIndex % config.width, y = sample.pixelIndex / config.width;
        for (int s = 0; s < 4; ++s)
            ASSERT_NEAR(full[(y * 2 + s / 2) * doubled.width + x * 2 + s % 2], sample.iterations[s], 1e-4);
    }
}

TEST_CASE(Julia, SelectionAndMarkerRetainDeepMandelbrotCoordinates)
{
    MandelbrotConfig view;
    view.width = 600; view.height = 400; view.zoom = 1e100;
    view.SetCenter(-0.75, 0.125);
    const auto [x, y] = ViewportPoint(view, 312.25, 187.75);
    ASSERT_FALSE(x == view.hpCenterX);
    ASSERT_EQ(x.ToDouble(), view.centerX);
    const auto point = ProjectPoint(view, x, y);
    ASSERT_NEAR(312.25, point.first, 1e-9);
    ASSERT_NEAR(187.75, point.second, 1e-9);
}

TEST_CASE(Julia, OverviewFitsBothAspectRatiosAndRetainsTheParameter)
{
    for (auto dimensions : { std::pair{800,300}, std::pair{300,800} })
    {
        auto view = JuliaConfig(-0.8, 0.156);
        view.width = dimensions.first; view.height = dimensions.second;
        ResetJuliaView(view);
        ASSERT_TRUE(view.juliaSet);
        ASSERT_TRUE(view.juliaX == BigFloat(-0.8));
        ASSERT_TRUE(view.hpCenterX.IsZero());
        const double span = 3.0 / view.zoom;
        ASSERT_GE(span, 4.0);
        ASSERT_GE(span * view.height / view.width, 4.0);
    }
}

TEST_CASE(Julia, FrameCoverageRejectsAnotherParameterOrTheMandelbrotPlane)
{
    auto view = JuliaConfig(0.0, 0.0);
    auto frame = AnimationRenderConfig(view);
    ASSERT_TRUE(FrameCoversViewport(frame, view));
    view.juliaX = BigFloat(-1.0);
    ASSERT_FALSE(FrameCoversViewport(frame, view));
    view = FrameViewport(frame, view.width, view.height);
    view.juliaSet = false;
    ASSERT_FALSE(FrameCoversViewport(frame, view));
}

TEST_CASE(Julia, SharingIncludesTheFixedParameterAtFullPrecision)
{
    auto view = JuliaConfig(0.0, 0.0);
    view.juliaX = BigFloat(-0.75) + BigFloat(1e-100);
    view.juliaY = BigFloat(0.125);
    ColorTheme theme;
    const auto json = SerializeViewState(view, theme, 1, 0, false, 0xFF000000);
    ASSERT_TRUE(json.find(L"\"fractalType\": \"Julia Set\"") != std::wstring::npos);
    ASSERT_TRUE(json.find(view.juliaX.ToWString(308)) != std::wstring::npos);
    ASSERT_TRUE(json.find(L"juliaParameterY") != std::wstring::npos);
    view.juliaSet = false;
    ASSERT_TRUE(SerializeViewState(view, theme, 1, 0, false, 0xFF000000).find(L"juliaParameter") == std::wstring::npos);
}

TEST_CASE(Julia, BackgroundRendererPublishesOnlyTheLatestSelectedParameter)
{
    std::mutex mutex;
    std::condition_variable ready;
    BackgroundRenderer renderer([&] { std::lock_guard lock(mutex); ready.notify_one(); });
    auto first = JuliaConfig(0.0, 0.0), next = JuliaConfig(-1.0, 0.0);
    renderer.Submit(first);
    renderer.Submit(next);
    std::vector<float> pixels, expected;
    std::optional<RenderResult> frame;
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&]
        {
            frame = renderer.TakeCompleted(pixels);
            return frame.has_value();
        }));
    }
    ASSERT_TRUE(frame.has_value());
    ASSERT_FALSE(frame->error != nullptr);
    ASSERT_TRUE(frame->config.juliaX == next.juliaX);
    MandelbrotEngine{}.Compute(next, expected);
    ASSERT_TRUE(pixels == expected);
}
