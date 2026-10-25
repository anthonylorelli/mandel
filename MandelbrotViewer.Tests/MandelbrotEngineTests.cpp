#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"

using namespace Mandelbrot;

TEST_CASE(MandelbrotEngine, ScreenToComplex_Center)
{
    const int width = 800;
    const int height = 600;
    const double centerX = -0.75;
    const double centerY = 0.25;
    const double zoom = 2.0;

    double outCx = 0.0, outCy = 0.0;
    // Pixel (width/2 - 0.5, height/2 - 0.5) corresponds to exact center
    MandelbrotEngine::ScreenToComplex(399.5, 299.5, width, height, centerX, centerY, zoom, outCx, outCy);

    ASSERT_NEAR(centerX, outCx, 1e-6);
    ASSERT_NEAR(centerY, outCy, 1e-6);
}

TEST_CASE(MandelbrotEngine, ScreenToComplex_ZoomScaling)
{
    const int width = 1000;
    const int height = 1000;
    double cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;

    // Zoom 1.0 spanX = 3.0 / 1.0 = 3.0
    MandelbrotEngine::ScreenToComplex(0.0, 0.0, width, height, 0.0, 0.0, 1.0, cx1, cy1);
    MandelbrotEngine::ScreenToComplex(999.0, 0.0, width, height, 0.0, 0.0, 1.0, cx2, cy2);
    double span1 = cx2 - cx1;

    // Zoom 2.0 spanX = 3.0 / 2.0 = 1.5
    MandelbrotEngine::ScreenToComplex(0.0, 0.0, width, height, 0.0, 0.0, 2.0, cx1, cy1);
    MandelbrotEngine::ScreenToComplex(999.0, 0.0, width, height, 0.0, 0.0, 2.0, cx2, cy2);
    double span2 = cx2 - cx1;

    ASSERT_NEAR(span1 * 0.5, span2, 1e-4);
}

TEST_CASE(MandelbrotEngine, ScreenToComplex_AspectRatio)
{
    const int width = 800;
    const int height = 400; // 2:1 aspect ratio
    double cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;

    MandelbrotEngine::ScreenToComplex(0.0, 0.0, width, height, 0.0, 0.0, 1.0, cx0, cy0);
    MandelbrotEngine::ScreenToComplex(width - 1, height - 1, width, height, 0.0, 0.0, 1.0, cx1, cy1);

    double spanX = cx1 - cx0;
    double spanY = cy0 - cy1; // cy decreases downward
    ASSERT_NEAR(spanX * 0.5, spanY, 1e-2);
}

TEST_CASE(MandelbrotEngine, ComplexToScreen_RoundTrip)
{
    const int width = 1920;
    const int height = 1080;
    const double centerX = -0.743643887;
    const double centerY = 0.131825904;
    const double zoom = 50000.0;

    // Test a grid of pixels
    for (int px = 50; px < width; px += 250)
    {
        for (int py = 50; py < height; py += 150)
        {
            double cx = 0.0, cy = 0.0;
            MandelbrotEngine::ScreenToComplex(px, py, width, height, centerX, centerY, zoom, cx, cy);

            double outPx = 0.0, outPy = 0.0;
            MandelbrotEngine::ComplexToScreen(cx, cy, width, height, centerX, centerY, zoom, outPx, outPy);

            ASSERT_NEAR(static_cast<double>(px), outPx, 0.5);
            ASSERT_NEAR(static_cast<double>(py), outPy, 0.5);
        }
    }
}

TEST_CASE(MandelbrotEngine, Compute_MandelbrotCardioidAndBulb)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Mandelbrot;
    config.width = 64;
    config.height = 64;
    config.centerX = 0.0;
    config.centerY = 0.0;
    config.zoom = 1.0;
    config.maxIterations = 100;
    config.smooth = true;

    std::vector<float> iterations;
    engine.Compute(config, iterations);

    ASSERT_EQ(static_cast<size_t>(64 * 64), iterations.size());

    // Center pixel is (0, 0), inside main cardioid -> should be -1.0f
    int centerIdx = 32 * 64 + 32;
    ASSERT_EQ(-1.0f, iterations[centerIdx]);

    // Test period-2 bulb point (-1.0, 0.0)
    MandelbrotConfig bulbConfig = config;
    bulbConfig.centerX = -1.0;
    bulbConfig.centerY = 0.0;
    bulbConfig.zoom = 10.0;
    engine.Compute(bulbConfig, iterations);
    ASSERT_EQ(-1.0f, iterations[centerIdx]);

    // Test exterior point (2.0, 2.0)
    MandelbrotConfig extConfig = config;
    extConfig.centerX = 2.0;
    extConfig.centerY = 2.0;
    extConfig.zoom = 10.0;
    engine.Compute(extConfig, iterations);
    ASSERT_GT(iterations[centerIdx], 0.0f);
    ASSERT_LT(iterations[centerIdx], 5.0f); // Escapes in 1-2 iterations
}

TEST_CASE(MandelbrotEngine, Compute_SmoothVsDiscrete)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Mandelbrot;
    config.width = 50;
    config.height = 50;
    config.centerX = -0.75;
    config.centerY = 0.1;
    config.zoom = 10.0;
    config.maxIterations = 200;

    std::vector<float> smoothIters;
    config.smooth = true;
    engine.Compute(config, smoothIters);

    std::vector<float> discreteIters;
    config.smooth = false;
    engine.Compute(config, discreteIters);

    // Discrete iterations must all be exact integers (or -1.0f for interior)
    for (float val : discreteIters)
    {
        if (val >= 0.0f)
        {
            ASSERT_NEAR(val, std::floor(val), 1e-5);
        }
    }

    // Smooth iterations should have non-zero fractional parts on escaped pixels
    bool hasFractional = false;
    for (float val : smoothIters)
    {
        if (val > 0.0f && std::abs(val - std::floor(val)) > 0.01f)
        {
            hasFractional = true;
            break;
        }
    }
    ASSERT_TRUE(hasFractional);
}

TEST_CASE(MandelbrotEngine, Compute_BurningShip_Formula)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::BurningShip;
    config.width = 40;
    config.height = 40;
    config.maxIterations = 300;
    config.smooth = true;

    // Origin (0, 0) is inside the Burning Ship
    config.centerX = 0.0;
    config.centerY = 0.0;
    config.zoom = 5.0;
    std::vector<float> iters;
    engine.Compute(config, iters);
    ASSERT_EQ(-1.0f, iters[20 * 40 + 20]);

    // Hull interior point (-0.45, 0.5) is inside the ship
    config.centerX = -0.45;
    config.centerY = 0.5;
    config.zoom = 5.0;
    engine.Compute(config, iters);
    ASSERT_EQ(-1.0f, iters[20 * 40 + 20]);

    // Far exterior (3.0, 3.0) escapes quickly
    config.centerX = 3.0;
    config.centerY = 3.0;
    config.zoom = 5.0;
    engine.Compute(config, iters);
    ASSERT_GT(iters[20 * 40 + 20], 0.0f);
    ASSERT_LT(iters[20 * 40 + 20], 5.0f);
}

TEST_CASE(MandelbrotEngine, Compute_Tricorn_Formula)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Tricorn;
    config.width = 40;
    config.height = 40;
    config.maxIterations = 300;
    config.smooth = true;

    // Origin (0, 0) is inside the Tricorn
    config.centerX = 0.0;
    config.centerY = 0.0;
    config.zoom = 5.0;
    std::vector<float> iters;
    engine.Compute(config, iters);
    ASSERT_EQ(-1.0f, iters[20 * 40 + 20]);

    // Needle satellite (-1.40, 0.0) contains points inside set
    config.centerX = -1.40;
    config.centerY = 0.0;
    config.zoom = 100.0;
    engine.Compute(config, iters);
    bool foundInside = false;
    for (float v : iters)
    {
        if (v < 0.0f) { foundInside = true; break; }
    }
    ASSERT_TRUE(foundInside);
}

TEST_CASE(MandelbrotEngine, Compute_Newton_Roots)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Newton;
    config.width = 30;
    config.height = 30;
    config.maxIterations = 100;
    config.smooth = true;

    std::vector<float> iters;

    // Test Root 0: (1.0, 0.0) -> Point (1.1, 0.0) converges to Root 0
    config.centerX = 1.1;
    config.centerY = 0.0;
    config.zoom = 20.0;
    engine.Compute(config, iters);
    float val0 = iters[15 * 30 + 15];
    ASSERT_GE(val0, 0.0f);
    int root0 = static_cast<int>(val0 / 1000000.0f);
    ASSERT_EQ(0, root0);

    // Test Root 1: (-0.5, +0.866) -> Point (-0.5, 0.9) converges to Root 1
    config.centerX = -0.5;
    config.centerY = 0.9;
    config.zoom = 20.0;
    engine.Compute(config, iters);
    float val1 = iters[15 * 30 + 15];
    ASSERT_GE(val1, 0.0f);
    int root1 = static_cast<int>(val1 / 1000000.0f);
    ASSERT_EQ(1, root1);

    // Test Root 2: (-0.5, -0.866) -> Point (-0.5, -0.9) converges to Root 2
    config.centerX = -0.5;
    config.centerY = -0.9;
    config.zoom = 20.0;
    engine.Compute(config, iters);
    float val2 = iters[15 * 30 + 15];
    ASSERT_GE(val2, 0.0f);
    int root2 = static_cast<int>(val2 / 1000000.0f);
    ASSERT_EQ(2, root2);
}

TEST_CASE(MandelbrotEngine, Compute_Newton_SingularityOrigin)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Newton;
    config.width = 40;
    config.height = 40;
    config.centerX = 0.0;
    config.centerY = 0.0;
    config.zoom = 100.0;
    config.maxIterations = 50;

    std::vector<float> iters;
    // Should complete cleanly without crashing or NaN
    engine.Compute(config, iters);
    ASSERT_EQ(static_cast<size_t>(1600), iters.size());
    for (float v : iters)
    {
        ASSERT_FALSE(std::isnan(v));
    }
}

TEST_CASE(MandelbrotEngine, Compute_CancellationToken)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Mandelbrot;
    config.width = 200;
    config.height = 200;
    config.maxIterations = 2000;

    std::atomic<bool> cancelToken{ true };
    std::vector<float> iters;
    double elapsed = engine.Compute(config, iters, &cancelToken);

    // With immediate cancel, computation finishes almost instantly
    ASSERT_LT(elapsed, 20.0);
}

TEST_CASE(MandelbrotEngine, Bookmarks_Integrity)
{
    for (auto type : GetAllFractalTypes())
    {
        const auto& bookmarks = GetBookmarks(type);
        ASSERT_GE(static_cast<int>(bookmarks.size()), 6);

        for (const auto& bm : bookmarks)
        {
            ASSERT_FALSE(bm.name.empty());
            ASSERT_FALSE(bm.description.empty());
            ASSERT_GT(bm.zoom, 0.0);
            ASSERT_GE(bm.maxIterations, 50);
            ASSERT_LE(std::abs(bm.centerX), 3.0);
            ASSERT_LE(std::abs(bm.centerY), 3.0);
        }
    }
}

TEST_CASE(MandelbrotEngine, Bookmarks_NoBlankScreens)
{
    MandelbrotEngine engine;
    const int w = 40;
    const int h = 30;

    for (auto type : GetAllFractalTypes())
    {
        const auto& bookmarks = GetBookmarks(type);
        for (const auto& bm : bookmarks)
        {
            MandelbrotConfig config;
            config.fractalType = type;
            config.width = w;
            config.height = h;
            config.centerX = bm.centerX;
            config.centerY = bm.centerY;
            config.zoom = bm.zoom;
            config.maxIterations = bm.maxIterations;
            config.smooth = true;

            std::vector<float> iters;
            engine.Compute(config, iters);

            int insideCount = 0;
            std::vector<float> escaped;
            for (float v : iters)
            {
                if (v < 0.0f) insideCount++;
                else escaped.push_back(v);
            }

            double insidePct = static_cast<double>(insideCount) / (w * h) * 100.0;

            // NONE should be 100% black (the regression test for the user's issue!)
            if (insidePct >= 99.0)
            {
                std::ostringstream ss;
                ss << "Bookmark renders as solid black: " << std::string(bm.name.length(), ' ');
                for (size_t i = 0; i < bm.name.length(); ++i) ss.str().back() = static_cast<char>(bm.name[i]);
                throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__);
            }

            // NONE should be 100% flat unescaped background
            if (insideCount == 0 && !escaped.empty())
            {
                float minV = *std::min_element(escaped.begin(), escaped.end());
                float maxV = *std::max_element(escaped.begin(), escaped.end());
                if (maxV - minV < 0.5f)
                {
                    std::ostringstream ss;
                    ss << "Bookmark renders as flat background with no fractal features: "
                       << std::string(bm.name.length(), ' ');
                    for (size_t i = 0; i < bm.name.length(); ++i) ss.str().back() = static_cast<char>(bm.name[i]);
                    throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__);
                }
            }
        }
    }
}

TEST_CASE(MandelbrotEngine, FractalTypeHelpers)
{
    const auto& types = GetAllFractalTypes();
    ASSERT_EQ(static_cast<size_t>(4), types.size());

    for (auto type : types)
    {
        std::wstring name = GetFractalTypeName(type);
        ASSERT_FALSE(name.empty());
    }
}

TEST_CASE(HighPrecision, BigFloat_ArithmeticAndNoCancellation)
{
    // Verify basic arithmetic
    BigFloat a(1.5);
    BigFloat b(-2.25);
    ASSERT_NEAR(-0.75, (a + b).ToDouble(), 1e-12);
    ASSERT_NEAR(3.75, (a - b).ToDouble(), 1e-12);
    ASSERT_NEAR(-3.375, (a * b).ToDouble(), 1e-12);
    ASSERT_NEAR(2.25, a.Sqr().ToDouble(), 1e-12);
    ASSERT_NEAR(1.0 / 3.0, (BigFloat(1.0) / BigFloat(3.0)).ToDouble(), 1e-12);

    // Crucial test: adding a 1e-40 offset to -0.75 and subtracting -0.75 must preserve 1e-40 without cancellation!
    BigFloat base(-0.75);
    BigFloat tiny(1e-40);
    BigFloat sum = base + tiny;
    BigFloat deltaResult = sum - base;
    ASSERT_NEAR(1e-40, deltaResult.ToDouble(), 1e-52);

    // String round-trip
    BigFloat parsed("-0.743643887037158704752191506114774");
    ASSERT_NEAR(-0.7436438870371587, parsed.ToDouble(), 1e-15);
    std::string str = parsed.ToString(20);
    ASSERT_TRUE(str.find("-0.743643887037158") == 0);
}

TEST_CASE(MandelbrotEngine, ZoomAndPanViewport_DeepPrecision)
{
    MandelbrotConfig config;
    config.width = 800;
    config.height = 600;
    config.SetCenter(-0.7436438870371587, 0.1318259042053119);
    config.zoom = 1e40;

    const BigFloat initialX = config.hpCenterX;
    const BigFloat initialY = config.hpCenterY;

    // Zoom in off-center at (600, 150) at 1e40 magnification
    MandelbrotEngine::ZoomViewport(config, 2.0, 600.0, 150.0);
    ASSERT_NEAR(2e40, config.zoom, 1e26);

    // In standard double, initialX and config.hpCenterX have the exact same double representation,
    // but in 1024-bit BigFloat they differ by ~3.75e-41!
    const double shiftX = (config.hpCenterX - initialX).ToDouble();
    const double shiftY = (config.hpCenterY - initialY).ToDouble();
    ASSERT_GT(std::abs(shiftX), 1e-42);
    ASSERT_GT(std::abs(shiftY), 1e-42);

    // Pan viewport by 100 pixels at 2e40 zoom
    const BigFloat beforePanX = config.hpCenterX;
    MandelbrotEngine::PanViewport(config, beforePanX, config.hpCenterY, 100.0, -50.0);
    const double panDeltaX = (config.hpCenterX - beforePanX).ToDouble();
    ASSERT_GT(std::abs(panDeltaX), 1e-42);
}

TEST_CASE(MandelbrotEngine, RecommendMaxIterations_ScalesWithZoom)
{
    const int iter1 = MandelbrotEngine::RecommendMaxIterations(1.0, FractalType::Mandelbrot, 300);
    const int iter1e6 = MandelbrotEngine::RecommendMaxIterations(1e6, FractalType::Mandelbrot, 300);
    const int iter1e20 = MandelbrotEngine::RecommendMaxIterations(1e20, FractalType::Mandelbrot, 300);

    ASSERT_GE(iter1, 300);
    ASSERT_GT(iter1e6, iter1);
    ASSERT_GT(iter1e20, iter1e6);
}

TEST_CASE(MandelbrotEngine, Compute_DeepZoom_AllFractals_NoPixelation)
{
    MandelbrotEngine engine;
    const int w = 32;
    const int h = 24;

    // 1. Mandelbrot at zoom = 1e25 at the Misiurewicz antenna tip (-2.0, 0.0)
    // (In 64-bit double, zoom = 1e25 collapses the entire viewport to a single constant pixel value,
    // whereas with 1024-bit Perturbation Theory, every pixel resolves the exact 4^n escape gradient!)
    {
        MandelbrotConfig config;
        config.fractalType = FractalType::Mandelbrot;
        config.width = w;
        config.height = h;
        config.SetCenter(BigFloat(-2.0) + BigFloat(1e-25), BigFloat(1e-25));
        config.zoom = 1e25;
        config.maxIterations = 1000;
        config.smooth = true;

        std::vector<float> iters;
        engine.Compute(config, iters);

        int distinctNeighbors = 0;
        const int midRow = (h / 2) * w;
        for (int px = 1; px < w; ++px)
        {
            if (iters[midRow + px] != iters[midRow + px - 1])
            {
                distinctNeighbors++;
            }
        }
        ASSERT_GT(distinctNeighbors, w / 2);
    }

    // 2. Burning Ship at zoom = 1e25 at the Misiurewicz antenna tip (-2.0, 0.0)
    {
        MandelbrotConfig config;
        config.fractalType = FractalType::BurningShip;
        config.width = w;
        config.height = h;
        config.SetCenter(BigFloat(-2.0) + BigFloat(1e-25), BigFloat(1e-25));
        config.zoom = 1e25;
        config.maxIterations = 1000;
        config.smooth = true;

        std::vector<float> iters;
        engine.Compute(config, iters);

        int distinctNeighbors = 0;
        const int midRow = (h / 2) * w;
        for (int px = 1; px < w; ++px)
        {
            if (iters[midRow + px] != iters[midRow + px - 1])
            {
                distinctNeighbors++;
            }
        }
        ASSERT_GT(distinctNeighbors, w / 2);
    }

    // 3. Tricorn at zoom = 1e25 at the Misiurewicz antenna tip (-2.0, 0.0)
    {
        MandelbrotConfig config;
        config.fractalType = FractalType::Tricorn;
        config.width = w;
        config.height = h;
        config.SetCenter(BigFloat(-2.0) + BigFloat(1e-25), BigFloat(1e-25));
        config.zoom = 1e25;
        config.maxIterations = 1000;
        config.smooth = true;

        std::vector<float> iters;
        engine.Compute(config, iters);

        int distinctNeighbors = 0;
        const int midRow = (h / 2) * w;
        for (int px = 1; px < w; ++px)
        {
            if (iters[midRow + px] != iters[midRow + px - 1])
            {
                distinctNeighbors++;
            }
        }
        ASSERT_GT(distinctNeighbors, w / 2);
    }

    // 4. Newton's Fractal at zoom = 1e25 right at the Origin Singularity (0, 0) and at (-0.75, 0.0)
    {
        MandelbrotConfig config;
        config.fractalType = FractalType::Newton;
        config.width = w;
        config.height = h;
        config.SetCenter(0.0, 0.0);
        config.zoom = 1e25;
        config.maxIterations = 500;
        config.smooth = true;

        std::vector<float> iters;
        engine.Compute(config, iters);

        // At (0, 0) all 3 roots meet in a rosette at EVERY scale down to 1e-300!
        bool seenRoot[3] = { false, false, false };
        for (float v : iters)
        {
            if (v >= 0.0f)
            {
                int r = static_cast<int>(v / 1000000.0f);
                if (r >= 0 && r < 3) seenRoot[r] = true;
            }
        }
        ASSERT_TRUE(seenRoot[0]);
        ASSERT_TRUE(seenRoot[1]);
        ASSERT_TRUE(seenRoot[2]);
    }
}
