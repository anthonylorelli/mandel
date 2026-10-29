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

    // Regression test for 64-bit limb product carry overflow when squaring values with large 32-bit limbs (> 2^31)
    const double testVal = -0.7436438870371587;
    const BigFloat bfVal(testVal);
    ASSERT_NEAR(testVal * testVal, bfVal.Sqr().ToDouble(), 1e-15);
    ASSERT_NEAR(testVal * testVal, (bfVal * bfVal).ToDouble(), 1e-15);
    ASSERT_NEAR(testVal * testVal, bfVal.Sqr(6).ToDouble(), 1e-15);
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

TEST_CASE(MandelbrotEngine, PanViewport_PixelShiftConsistency_AllZoomLevels)
{
    MandelbrotEngine engine;
    const int w = 32;
    const int h = 24;
    const int deltaPx = 4;
    const int deltaPy = 3;

    // Test panning consistency across standard double zoom levels and 1024-bit perturbation zoom levels
    const double testZooms[] = { 100.0, 1e8, 1e12, 1e25 };

    for (double zoom : testZooms)
    {
        MandelbrotConfig beforeCfg;
        beforeCfg.fractalType = FractalType::Mandelbrot;
        beforeCfg.width = w;
        beforeCfg.height = h;
        beforeCfg.SetCenter(
            BigFloat("-0.743643887037158704752191506114774"),
            BigFloat("0.131825904205311970493132056385139"));
        beforeCfg.zoom = zoom;
        beforeCfg.maxIterations = 600;
        beforeCfg.smooth = true;

        std::vector<float> itersBefore;
        engine.Compute(beforeCfg, itersBefore);

        MandelbrotConfig afterCfg = beforeCfg;
        MandelbrotEngine::PanViewport(
            afterCfg,
            beforeCfg.hpCenterX,
            beforeCfg.hpCenterY,
            static_cast<double>(deltaPx),
            static_cast<double>(deltaPy));

        std::vector<float> itersAfter;
        engine.Compute(afterCfg, itersAfter);

        // Dragging right by +deltaPx and down by +deltaPy shifts the rendered image by (+deltaPx, +deltaPy)
        for (int py = 0; py < h - deltaPy; ++py)
        {
            for (int px = 0; px < w - deltaPx; ++px)
            {
                const float vBefore = itersBefore[static_cast<size_t>(py) * w + px];
                const float vAfter = itersAfter[static_cast<size_t>(py + deltaPy) * w + (px + deltaPx)];
                ASSERT_NEAR(vBefore, vAfter, 1e-2f);
            }
        }
    }
}

TEST_CASE(MandelbrotEngine, DeepZoomThreshold_Continuity)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.fractalType = FractalType::Mandelbrot;
    config.width = 24;
    config.height = 18;
    config.SetCenter(-0.7436438870371587, 0.1318259042053119);
    config.zoom = 1e4;
    config.maxIterations = 250;
    config.smooth = true;

    std::vector<float> stdIters;
    engine.Compute(config, stdIters);

    std::vector<float> deepIters;
    engine.ComputeDeepZoom(config, config.hpCenterX, config.hpCenterY, deepIters);

    for (size_t i = 0; i < stdIters.size(); ++i)
    {
        ASSERT_NEAR(stdIters[i], deepIters[i], 1e-2f);
    }
}

TEST_CASE(HighPrecision, BigFloat_TryParse_StringsAndScientific)
{
    BigFloat out;
    ASSERT_TRUE(BigFloat::TryParse(L" -0.743643887037158704752191506114774 ", out));
    ASSERT_NEAR(-0.7436438870371587, out.ToDouble(), 1e-15);

    ASSERT_TRUE(BigFloat::TryParse(L"+1.25e-4", out));
    ASSERT_NEAR(1.25e-4, out.ToDouble(), 1e-18);

    ASSERT_TRUE(BigFloat::TryParse("-2.5E+2", out));
    ASSERT_NEAR(-250.0, out.ToDouble(), 1e-12);

    ASSERT_FALSE(BigFloat::TryParse(L"", out));
    ASSERT_FALSE(BigFloat::TryParse(L"   ", out));
    ASSERT_FALSE(BigFloat::TryParse(L"not_a_number", out));
    ASSERT_FALSE(BigFloat::TryParse(L"1.23e", out));
}

TEST_CASE(MandelbrotEngine, StepZoomAnimation_ZoomInAndOut_TargetConvergence)
{
    MandelbrotConfig config;
    config.width = 800;
    config.height = 600;
    config.SetCenter(-0.75, 0.0);
    config.zoom = 1.0;

    const BigFloat targetX("-0.743643887037158704752191506114774");
    const BigFloat targetY("0.131825904205311970493132056385139");

    double prevDist = std::hypot((config.hpCenterX - targetX).ToDouble(), (config.hpCenterY - targetY).ToDouble());
    ASSERT_GT(prevDist, 0.1);

    // Step zoom-in animation smoothly toward (targetX, targetY)
    for (int step = 0; step < 30; ++step)
    {
        const bool cont = MandelbrotEngine::StepZoomAnimation(config, targetX, targetY, 1.15);
        ASSERT_TRUE(cont);
        const double dist = std::hypot((config.hpCenterX - targetX).ToDouble(), (config.hpCenterY - targetY).ToDouble());
        ASSERT_LE(dist, prevDist);
        prevDist = dist;
    }

    // Center should now be locked onto the exact 1024-bit target coordinate
    ASSERT_EQ(0, BigFloat::CompareMagnitude(config.hpCenterX - targetX, BigFloat(0.0)));
    ASSERT_EQ(0, BigFloat::CompareMagnitude(config.hpCenterY - targetY, BigFloat(0.0)));
    ASSERT_GT(config.zoom, 50.0);

    // Now animate zooming out (factor < 1.0) and verify zoom decreases while staying on target
    const double peakZoom = config.zoom;
    for (int step = 0; step < 10; ++step)
    {
        const bool cont = MandelbrotEngine::StepZoomAnimation(config, targetX, targetY, 0.85);
        ASSERT_TRUE(cont);
    }
    ASSERT_LT(config.zoom, peakZoom);
    ASSERT_EQ(0, BigFloat::CompareMagnitude(config.hpCenterX - targetX, BigFloat(0.0)));
    ASSERT_EQ(0, BigFloat::CompareMagnitude(config.hpCenterY - targetY, BigFloat(0.0)));

    // Verify zoom limits return false when reached
    config.zoom = MandelbrotEngine::MaxZoom;
    ASSERT_FALSE(MandelbrotEngine::StepZoomAnimation(config, targetX, targetY, 1.1));

    config.zoom = MandelbrotEngine::MinZoom;
    ASSERT_FALSE(MandelbrotEngine::StepZoomAnimation(config, targetX, targetY, 0.9));
}

TEST_CASE(HighPrecision, BigFloat_EqualityOperator)
{
    BigFloat a("-0.743643887037158704752191506114774");
    BigFloat b("-0.743643887037158704752191506114774");
    BigFloat c("-0.743643887037158704752191506114775");
    BigFloat z1(0.0);
    BigFloat z2(-0.0);

    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a != b);
    ASSERT_TRUE(a != c);
    ASSERT_TRUE(z1 == z2);
}

TEST_CASE(MandelbrotEngine, AdaptiveSubdivision_StrideAndEdgeRefinement)
{
    MandelbrotEngine engine;
    MandelbrotConfig baseCfg;
    baseCfg.fractalType = FractalType::Mandelbrot;
    baseCfg.width = 48;
    baseCfg.height = 36;
    baseCfg.SetCenter(-0.5, 0.0);
    baseCfg.zoom = 1.5;
    baseCfg.maxIterations = 200;
    baseCfg.smooth = true;
    baseCfg.renderStride = 1;
    baseCfg.edgeRefine = true;

    std::vector<float> fullIters;
    engine.Compute(baseCfg, fullIters);

    // 1. Stride = 2 with edgeRefine = true should match full 1x1 resolution at all interior/boundary blocks
    MandelbrotConfig refineCfg = baseCfg;
    refineCfg.renderStride = 2;
    refineCfg.edgeRefine = true;

    std::vector<float> refineIters;
    engine.Compute(refineCfg, refineIters);
    ASSERT_EQ(fullIters.size(), refineIters.size());

    int matchingSignCount = 0;
    for (size_t i = 0; i < fullIters.size(); ++i)
    {
        ASSERT_FALSE(std::isnan(refineIters[i]));
        if ((fullIters[i] < 0.0f) == (refineIters[i] < 0.0f))
        {
            matchingSignCount++;
        }
    }
    // Nearly 100% of interior/exterior classifications match
    ASSERT_GT(matchingSignCount, static_cast<int>(fullIters.size() * 95 / 100));

    // 2. Stride = 2 and Stride = 3 with edgeRefine = false (fast animation mode) must never blend -1.0f with exterior values
    for (int stride : { 2, 3 })
    {
        MandelbrotConfig fastCfg = baseCfg;
        fastCfg.renderStride = stride;
        fastCfg.edgeRefine = false;

        std::vector<float> fastIters;
        engine.Compute(fastCfg, fastIters);
        ASSERT_EQ(fullIters.size(), fastIters.size());

        for (float v : fastIters)
        {
            ASSERT_FALSE(std::isnan(v));
            // Must be either exact -1.0f (interior) or >= 0.0f (exterior) — never an invalid negative fraction like -0.5f
            ASSERT_TRUE(v == -1.0f || v >= 0.0f);
        }
    }
}

TEST_CASE(MandelbrotEngine, AdaptiveSubdivision_HighIterationFilamentsRefinePerPixel)
{
    MandelbrotEngine engine;
    MandelbrotConfig config;
    config.width = 96;
    config.height = 72;
    config.maxIterations = 2000;
    config.SetCenter(-0.7436438870371587, 0.1318259042053119);
    config.edgeRefine = true;

    for (int stride : { 2, 3, 4 })
    {
        int steepCellsChecked = 0;
        int hiddenDetailCellsChecked = 0;
        for (double zoom : { 85000.0, 500000.0 })
        {
            config.zoom = zoom;
            config.renderStride = 1;
            std::vector<float> full;
            engine.Compute(config, full);
            config.renderStride = stride;
            std::vector<float> adaptive;
            engine.Compute(config, adaptive);

            auto sample = [&](int x, int y) { return full[static_cast<size_t>(y) * config.width + x]; };
            for (int y0 = 0; y0 < config.height - 1; y0 += stride)
            {
                const int y1 = std::min(y0 + stride, config.height - 1);
                for (int x0 = 0; x0 < config.width - 1; x0 += stride)
                {
                    const int x1 = std::min(x0 + stride, config.width - 1);
                    const float v00 = sample(x0, y0), v10 = sample(x1, y0);
                    const float v01 = sample(x0, y1), v11 = sample(x1, y1);
                    const float minV = std::min({ v00, v10, v01, v11 });
                    const float maxV = std::max({ v00, v10, v01, v11 });
                    if (minV < 100.0f) continue; // Focus on detailed high-iteration exterior cells.

                    const int xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
                    const float tx = static_cast<float>(xm - x0) / (x1 - x0);
                    const float ty = static_cast<float>(ym - y0) / (y1 - y0);
                    const float top = v00 + tx * (v10 - v00);
                    const float bot = v01 + tx * (v11 - v01);
                    const float midpointError = std::abs(sample(xm, ym) - (top + ty * (bot - top)));
                    const bool steep = maxV - minV > 1.25f * stride;
                    const bool hiddenDetail = !steep && midpointError > 0.25f;
                    if (!steep && !hiddenDetail) continue;
                    if (steep) ++steepCellsChecked;
                    else ++hiddenDetailCellsChecked;

                    // Animation must resolve these filaments as precisely as the stopped frame,
                    // including cells whose similar corner values conceal a ridge at the midpoint.
                    const int endY = (y1 == config.height - 1) ? config.height : y1;
                    const int endX = (x1 == config.width - 1) ? config.width : x1;
                    for (int y = y0; y < endY; ++y)
                        for (int x = x0; x < endX; ++x)
                            ASSERT_EQ(sample(x, y), adaptive[static_cast<size_t>(y) * config.width + x]);
                }
            }
        }
        ASSERT_GT(steepCellsChecked, 0);
        ASSERT_GT(hiddenDetailCellsChecked, 0);
    }
}

TEST_CASE(MandelbrotEngine, DeepZoom_ReferenceOrbitCachingAndExtension)
{
    MandelbrotEngine engine;
    MandelbrotConfig cfg;
    cfg.fractalType = FractalType::Mandelbrot;
    cfg.width = 24;
    cfg.height = 18;
    cfg.SetCenter(
        BigFloat("-0.743643887037158704752191506114774"),
        BigFloat("0.131825904205311970493132056385139"));
    cfg.zoom = 1e16;
    cfg.maxIterations = 400;
    cfg.smooth = true;

    // First render populates the reference orbit cache
    std::vector<float> frame1;
    engine.Compute(cfg, frame1);

    // Second render at a deeper zoom on the SAME center coordinate reuses the cached 1024-bit reference orbit
    cfg.zoom = 1.25e16;
    std::vector<float> frame2Cached;
    engine.Compute(cfg, frame2Cached);

    // Compare against a fresh engine with no prior cache
    MandelbrotEngine freshEngine;
    std::vector<float> frame2Fresh;
    freshEngine.Compute(cfg, frame2Fresh);

    ASSERT_EQ(frame2Cached.size(), frame2Fresh.size());
    for (size_t i = 0; i < frame2Cached.size(); ++i)
    {
        ASSERT_NEAR(frame2Cached[i], frame2Fresh[i], 1e-4f);
    }

    // Third render on the same center with HIGHER maxIterations incrementally extends the cached reference orbit
    cfg.zoom = 1.5e16;
    cfg.maxIterations = 600;
    std::vector<float> frame3Extended;
    engine.Compute(cfg, frame3Extended);

    MandelbrotEngine freshEngine2;
    std::vector<float> frame3Fresh;
    freshEngine2.Compute(cfg, frame3Fresh);

    ASSERT_EQ(frame3Extended.size(), frame3Fresh.size());
    for (size_t i = 0; i < frame3Extended.size(); ++i)
    {
        ASSERT_NEAR(frame3Extended[i], frame3Fresh[i], 1e-4f);
    }
}

TEST_CASE(MandelbrotEngine, IterationEdge_SmoothContinuityAndNoFlicker)
{
    MandelbrotEngine engine;

    // 1. Verify that discrete iteration edges at stride = 2 (edgeRefine = true) are 100% bit-for-bit identical to stride = 1
    {
        MandelbrotConfig cfg1;
        cfg1.fractalType = FractalType::Mandelbrot;
        cfg1.width = 48;
        cfg1.height = 36;
        cfg1.SetCenter(-0.75, 0.12);
        cfg1.zoom = 4.0;
        cfg1.maxIterations = 150;
        cfg1.smooth = false;
        cfg1.renderStride = 1;
        cfg1.edgeRefine = true;

        MandelbrotConfig cfg2 = cfg1;
        cfg2.renderStride = 2;
        cfg2.edgeRefine = true;

        std::vector<float> iters1;
        std::vector<float> iters2;
        engine.Compute(cfg1, iters1);
        engine.Compute(cfg2, iters2);

        ASSERT_EQ(iters1.size(), iters2.size());
        for (size_t i = 0; i < iters1.size(); ++i)
        {
            ASSERT_EQ(iters1[i], iters2[i]);
        }
    }

    // 2. Verify that smooth coloring across an integer iteration boundary has no step discontinuity
    // by checking second-difference continuity (no sudden jump compared to adjacent pixel slope)
    {
        MandelbrotConfig scanCfg;
        scanCfg.fractalType = FractalType::Mandelbrot;
        scanCfg.width = 800;
        scanCfg.height = 1;
        scanCfg.SetCenter(-1.75, 0.15);
        scanCfg.zoom = 5.0; // Crosses iterations 3..10 smoothly from x = -2.05 to -1.45
        scanCfg.maxIterations = 200;
        scanCfg.smooth = false;

        std::vector<float> discScan;
        engine.Compute(scanCfg, discScan);

        scanCfg.smooth = true;
        std::vector<float> smoothScan;
        engine.Compute(scanCfg, smoothScan);

        int bandTransitionsChecked = 0;
        for (size_t px = 2; px + 1 < discScan.size(); ++px)
        {
            // When two adjacent pixels cross an integer iteration boundary (e.g. iter n -> iter n+1)
            if (discScan[px - 1] > 0.0f && discScan[px] > 0.0f &&
                std::abs(discScan[px] - discScan[px - 1]) == 1.0f)
            {
                const float stepAtBoundary = smoothScan[px] - smoothScan[px - 1];
                const float stepBefore = smoothScan[px - 1] - smoothScan[px - 2];
                const float jumpDiscontinuity = std::abs(stepAtBoundary - stepBefore);
                // Without post-bailout polish, jumpDiscontinuity is ~0.05-0.13 at integer iteration boundaries.
                // With post-bailout polish, jumpDiscontinuity is < 0.002.
                ASSERT_LT(jumpDiscontinuity, 0.005f);
                bandTransitionsChecked++;
            }
        }
        ASSERT_GT(bandTransitionsChecked, 2);
    }
}

TEST_CASE(MandelbrotEngine, AutoExplore_RandomInterestingTargetAndRetargetOnSolid)
{
    MandelbrotEngine engine;

    // 1. ShouldRetargetZoom detects solid interior (-1.0f) and flat exterior regions, while keeping rich boundaries
    {
        MandelbrotConfig cfg;
        cfg.fractalType = FractalType::Mandelbrot;
        cfg.width = 64;
        cfg.height = 48;
        cfg.SetCenter(-0.75, 0.0);
        cfg.zoom = 1.0;
        cfg.maxIterations = 300;
        cfg.smooth = true;

        std::vector<float> iters;
        engine.Compute(cfg, iters);

        // Deep inside main cardioid (-0.25, 0.0) -> solid interior -> should retarget
        ASSERT_TRUE(MandelbrotEngine::ShouldRetargetZoom(cfg, iters, BigFloat(-0.25), BigFloat(0.0)));

        // Far exterior corner (-2.1, 1.0) -> flat low-iteration exterior -> should retarget
        ASSERT_TRUE(MandelbrotEngine::ShouldRetargetZoom(cfg, iters, BigFloat(-2.1), BigFloat(1.0)));

        // Seahorse Valley boundary (-0.7436438870371587, 0.1318259042053119) -> rich boundary -> should stay on target
        ASSERT_FALSE(MandelbrotEngine::ShouldRetargetZoom(
            cfg, iters, BigFloat(-0.7436438870371587), BigFloat(0.1318259042053119)));
    }

    // 2. PickInterestingTarget selects valid boundary targets across all 4 fractal types and varies with seed
    for (FractalType type : GetAllFractalTypes())
    {
        MandelbrotConfig cfg;
        cfg.fractalType = type;
        cfg.width = 64;
        cfg.height = 48;
        const auto& bms = GetBookmarks(type);
        cfg.SetCenter(bms[0].centerX, bms[0].centerY);
        cfg.zoom = bms[0].zoom;
        cfg.maxIterations = 300;
        cfg.smooth = true;

        std::vector<float> iters;
        engine.Compute(cfg, iters);

        BigFloat tx1, ty1;
        ASSERT_TRUE(MandelbrotEngine::PickInterestingTarget(cfg, iters, tx1, ty1, 101));
        ASSERT_FALSE(MandelbrotEngine::ShouldRetargetZoom(cfg, iters, tx1, ty1));

        // Simulate smooth zoom + pan toward (tx1, ty1) and verify it never produces a 100% solid screen
        MandelbrotConfig animCfg = cfg;
        for (int frame = 0; frame < 20; ++frame)
        {
            const double blend = std::min(1.0, (frame + 1) * 0.1);
            ASSERT_TRUE(MandelbrotEngine::StepZoomAnimation(animCfg, tx1, ty1, 1.18, blend));
        }

        engine.Compute(animCfg, iters);
        int interiorCount = 0;
        float minExt = 1e30f;
        float maxExt = -1e30f;
        for (float v : iters)
        {
            if (v < 0.0f) interiorCount++;
            else
            {
                minExt = std::min(minExt, v);
                maxExt = std::max(maxExt, v);
            }
        }
        // Screen must not be 100% interior or 100% flat exterior after zooming in on the refined boundary target
        ASSERT_LT(interiorCount, static_cast<int>(iters.size() * 90 / 100));
        if (interiorCount == 0)
        {
            ASSERT_GT(maxExt - minExt, 2.0f);
        }
    }
}
