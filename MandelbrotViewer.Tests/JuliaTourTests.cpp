#include "pch.h"
#include "TestHarness.h"
#include "JuliaTour.h"
#include "JuliaView.h"
#include <limits>

using Mandelbrot::JuliaTour;

namespace
{
    struct Location { double radius; bool bulb; bool primary; };

    Location Locate(JuliaTour::Point c)
    {
        const double q = std::norm(c - JuliaTour::Point(0.25));
        const double cardioidRadius = std::abs(JuliaTour::Point(1.0) -
            std::sqrt(JuliaTour::Point(1.0) - 4.0 * c));
        const double bulbRadius = 4.0 * std::abs(c + JuliaTour::Point(1.0));
        if (q * (q + c.real() - 0.25) <= 0.25 * c.imag() * c.imag() + 1e-12)
            return { cardioidRadius, false, true };
        if (bulbRadius <= 1.0 + 1e-12) return { bulbRadius, true, true };
        return { std::min(cardioidRadius, bulbRadius), bulbRadius < cardioidRadius, false };
    }

    bool CriticalOrbitEscapes(JuliaTour::Point c)
    {
        JuliaTour::Point z{};
        for (int iter = 0; iter < 2048; ++iter)
        {
            z = z * z + c;
            if (std::norm(z) > 4.0) return true;
        }
        return false;
    }
}

TEST_CASE(JuliaTour, StartsAtTheSelectionWithinEitherConnectedRegion)
{
    JuliaTour tour;
    for (auto c : { JuliaTour::Point(0.0), JuliaTour::Point(-0.1, 0.2),
        JuliaTour::Point(-1.0, 0.1), JuliaTour::Point(-0.75), JuliaTour::Point(0.25) })
        ASSERT_NEAR(0.0, std::abs(tour.Start(c.real(), c.imag(), 42) - c), 1e-14);
}

TEST_CASE(JuliaTour, UnsupportedSelectionsStartAtTheSharedPoint)
{
    JuliaTour tour;
    for (auto c : { JuliaTour::Point(-2.0), JuliaTour::Point(1.0, 1.0),
        JuliaTour::Point(1e300, 1e300), JuliaTour::Point(std::numeric_limits<double>::infinity()),
        JuliaTour::Point(std::numeric_limits<double>::quiet_NaN()) })
        ASSERT_NEAR(0.0, std::abs(tour.Start(c.real(), c.imag(), 42) - JuliaTour::Point(-0.75)), 0.0);
}

TEST_CASE(JuliaTour, RestartsAtNearbyExteriorSelectionsAndReturnsInward)
{
    for (auto c : { JuliaTour::Point(0.3, 0.0), JuliaTour::Point(-1.28, 0.05) })
    {
        JuliaTour tour;
        ASSERT_NEAR(0.0, std::abs(tour.Start(c.real(), c.imag(), 42) - c), 1e-14);
        for (int tick = 0; tick < 600; ++tick) tour.Advance(1.0 / 60.0);
        ASSERT_TRUE(Locate(tour.Current()).primary);
    }
}

TEST_CASE(JuliaTour, LongRoutesStayFiniteBoundedAndContinuous)
{
    for (uint64_t seed : { 1, 42, 9127 })
    {
        JuliaTour tour;
        auto previous = tour.Start(0.0, 0.0, seed);
        bool visitedCardioid = false, visitedBulb = false;
        double distance = 0.0;
        for (int tick = 0; tick < 36000; ++tick)
        {
            const auto c = tour.Advance(1.0 / 60.0);
            ASSERT_TRUE(std::isfinite(c.real()) && std::isfinite(c.imag()));
            const double step = std::abs(c - previous);
            ASSERT_LE(step, JuliaTour::MaxParameterStep + 1e-14);
            distance += step;
            const auto location = Locate(c);
            ASSERT_LE(location.radius, JuliaTour::MaxMultiplierRadius + 1e-12);
            visitedCardioid |= location.primary && !location.bulb;
            visitedBulb |= location.primary && location.bulb;
            if (tick % 97 == 0 && location.primary) ASSERT_FALSE(CriticalOrbitEscapes(c));
            previous = c;
        }
        ASSERT_TRUE(visitedCardioid && visitedBulb);
        ASSERT_GT(distance, 10.0);
    }
}

TEST_CASE(JuliaTour, SeedsReproduceRoutesAndGenerateDifferentTours)
{
    JuliaTour first, second, other;
    first.Start(-0.75, 0.0, 42);
    second.Start(-0.75, 0.0, 42);
    other.Start(-0.75, 0.0, 43);
    for (int tick = 0; tick < 1000; ++tick)
    {
        ASSERT_TRUE(first.Advance(1.0 / 60.0) == second.Advance(1.0 / 60.0));
        other.Advance(1.0 / 60.0);
    }
    ASSERT_GT(std::abs(first.Current() - other.Current()), 0.01);
}

TEST_CASE(JuliaTour, BalancesBoundaryDetailWithInteriorExcursionsInBothRegions)
{
    for (uint64_t seed : { 1, 42, 9127 })
    {
        JuliaTour tour;
        tour.Start(0.0, 0.0, seed);
        std::array<int, 2> nearBoundary{}, interior{}, samples{};
        int deepInterior = 0;
        for (int tick = 0; tick < 36000; ++tick)
        {
            const auto c = tour.Advance(1.0 / 60.0);
            if (tick < 900) continue; // Allow the tour to leave the initial selection.
            const auto location = Locate(c);
            const int region = location.bulb ? 1 : 0;
            nearBoundary[region] += location.radius > 0.85 && location.radius < 1.05;
            interior[region] += location.radius < 0.80;
            deepInterior += location.radius < 0.60;
            ++samples[region];
        }
        for (int region = 0; region < 2; ++region)
        {
            ASSERT_GT(samples[region], 0);
            const double boundaryFraction = static_cast<double>(nearBoundary[region]) / samples[region];
            ASSERT_GT(boundaryFraction, 0.40);
            ASSERT_LT(boundaryFraction, 0.85);
            ASSERT_GT(static_cast<double>(interior[region]) / samples[region], 0.15);
        }
        ASSERT_GT(static_cast<double>(deepInterior) / (samples[0] + samples[1]), 0.04);
    }
}

TEST_CASE(JuliaTour, ExteriorExcursionsAreOccasionalAndReturnInside)
{
    for (uint64_t seed : { 1, 42, 9127 })
    {
        JuliaTour tour;
        tour.Start(-0.75, 0.0, seed);
        int samples = 0, exterior = 0, returned = 0, leftAt = -1;
        for (int tick = 0; tick < 36000; ++tick)
        {
            const auto c = tour.Advance(1.0 / 60.0);
            if (tick % 15 != 0) continue;
            const auto location = Locate(c);
            if (!location.primary && CriticalOrbitEscapes(c))
            {
                ++exterior;
                if (leftAt < 0) leftAt = tick;
            }
            if (location.primary && leftAt >= 0) { ++returned; leftAt = -1; }
            if (leftAt >= 0) ASSERT_LT(tick - leftAt, 1200);
            ++samples;
        }
        ASSERT_GT(static_cast<double>(exterior) / samples, 0.01);
        ASSERT_LT(static_cast<double>(exterior) / samples, 0.25);
        ASSERT_GT(returned, 1);
    }
}

TEST_CASE(JuliaTour, RenderedRoutesMixDetailedAndBroaderJuliaShapes)
{
    Mandelbrot::MandelbrotEngine engine;
    auto boundaryLength = [&](JuliaTour::Point c)
    {
        Mandelbrot::MandelbrotConfig config;
        config.width = config.height = 96;
        config.maxIterations = 500;
        config.smooth = false;
        config.juliaX = Mandelbrot::BigFloat(c.real());
        config.juliaY = Mandelbrot::BigFloat(c.imag());
        Mandelbrot::ResetJuliaView(config);
        std::vector<float> pixels;
        engine.Compute(config, pixels);
        int transitions = 0;
        for (int y = 0; y < config.height - 1; ++y)
            for (int x = 0; x < config.width - 1; ++x)
            {
                const auto i = static_cast<size_t>(y) * config.width + x;
                transitions += (pixels[i] < 0) != (pixels[i + 1] < 0);
                transitions += (pixels[i] < 0) != (pixels[i + config.width] < 0);
            }
        return transitions;
    };
    const double centers = (boundaryLength(JuliaTour::Point(0.0)) + boundaryLength(JuliaTour::Point(-1.0))) * 0.5;
    double detail = 0.0;
    int rendered = 0, detailedShapes = 0, broaderShapes = 0;
    for (uint64_t seed : { 1, 42, 9127 })
    {
        JuliaTour tour;
        tour.Start(-0.75, 0.0, seed);
        for (int frame = 1; frame <= 7200; ++frame)
        {
            const auto c = tour.Advance(1.0 / 60.0);
            if (frame % 300 != 0) continue;
            const int length = boundaryLength(c);
            detail += length;
            detailedShapes += length > centers * 1.5;
            broaderShapes += length < centers * 1.2;
            ++rendered;
        }
    }
    // Both detailed and broad forms should appear regularly; an average alone could
    // pass for a route that stays near the edge throughout.
    ASSERT_GT(static_cast<double>(detailedShapes) / rendered, 0.20);
    ASSERT_GT(static_cast<double>(broaderShapes) / rendered, 0.20);
    ASSERT_GT(detail / rendered, centers * 1.1);
}

TEST_CASE(JuliaTour, InvalidTimesDoNotMoveAndLongDelaysCannotJump)
{
    JuliaTour tour;
    const auto start = tour.Start(-0.75, 0.0, 42);
    for (double seconds : { 0.0, -1.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN() })
        ASSERT_TRUE(tour.Advance(seconds) == start);
    for (int tick = 0; tick < 2000; ++tick)
    {
        const auto previous = tour.Current();
        ASSERT_LE(std::abs(tour.Advance(1000.0) - previous), JuliaTour::MaxParameterStep + 1e-14);
    }
}
