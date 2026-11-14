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
                std::string narrowName;
                for (wchar_t ch : bm.name) narrowName.push_back(static_cast<char>(ch));
                std::ostringstream ss;
                ss << "Bookmark renders as solid black (" << insidePct << "%): " << narrowName;
                throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__);
            }

            // NONE should be 100% flat unescaped background
            if (insideCount == 0 && !escaped.empty())
            {
                float minV = *std::min_element(escaped.begin(), escaped.end());
                float maxV = *std::max_element(escaped.begin(), escaped.end());
                if (maxV - minV < 0.5f)
                {
                    std::string narrowName;
                    for (wchar_t ch : bm.name) narrowName.push_back(static_cast<char>(ch));
                    std::ostringstream ss;
                    ss << "Bookmark renders as flat background with no fractal features: " << narrowName;
                    throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__);
                }
            }

            // Zoomed-in bookmarks must not have most of the window filled by a flat exterior color area
            if (bm.zoom > 2.0 && type != FractalType::Newton && !escaped.empty())
            {
                const float minV = *std::min_element(escaped.begin(), escaped.end());
                int flatLowCount = 0;
                for (float v : escaped)
                {
                    if (v < minV + 5.0f) ++flatLowCount;
                }
                const double flatLowPct = static_cast<double>(flatLowCount) / (w * h) * 100.0;
                if (flatLowPct >= 60.0)
                {
                    std::string narrowName;
                    for (wchar_t ch : bm.name) narrowName.push_back(static_cast<char>(ch));
                    std::ostringstream ss;
                    ss << "Bookmark has most of the window filled by a solid exterior color ("
                       << flatLowPct << "%): " << narrowName;
                    throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__);
                }
            }
        }
    }
}

TEST_CASE(MandelbrotEngine, FractalTypeHelpers)
{
    const auto& types = GetAllFractalTypes();
    ASSERT_EQ(static_cast<size_t>(8), types.size());

    for (auto type : types)
    {
        std::wstring name = GetFractalTypeName(type);
        ASSERT_FALSE(name.empty());
    }
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
