#include "pch.h"
#include "TestHarness.h"
#include "MandelbrotEngine.h"
#include "DeepReferenceCache.h"

using namespace Mandelbrot;

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

TEST_CASE(MandelbrotEngine, Compute_Celtic_Buffalo_Cubic_Phoenix_Formulas)
{
    MandelbrotEngine engine;
    const FractalType newTypes[] = {
        FractalType::Celtic,
        FractalType::Buffalo,
        FractalType::CubicMultibrot,
        FractalType::Phoenix
    };

    for (FractalType type : newTypes)
    {
        MandelbrotConfig config;
        config.fractalType = type;
        config.width = 40;
        config.height = 40;
        config.maxIterations = 300;
        config.smooth = true;

        std::vector<float> iters;
        if (type == FractalType::Phoenix)
        {
            // Overview contains both interior feather basins and high-iteration plumage
            config.SetCenter(0.0, 0.0);
            config.zoom = 1.0;
            engine.Compute(config, iters);
            bool foundInteriorOrPlume = false;
            for (float v : iters)
            {
                if (v < 0.0f || v > 25.0f) { foundInteriorOrPlume = true; break; }
            }
            ASSERT_TRUE(foundInteriorOrPlume);
        }
        else
        {
            // Origin (0, 0) is inside Celtic, Buffalo, and CubicMultibrot
            config.SetCenter(0.0, 0.0);
            config.zoom = 5.0;
            engine.Compute(config, iters);
            ASSERT_EQ(-1.0f, iters[20 * 40 + 20]);
        }

        // Far exterior (3.0, 3.0) escapes quickly
        config.SetCenter(3.0, 3.0);
        config.zoom = 5.0;
        engine.Compute(config, iters);
        ASSERT_GT(iters[20 * 40 + 20], 0.0f);
        ASSERT_LT(iters[20 * 40 + 20], 5.0f);
    }
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

TEST_CASE(MandelbrotEngine, ResolveHpCenterPreservesUntouchedAxisPrecision)
{
    FractalConfig cfg;
    const BigFloat baseX(-0.75);
    const BigFloat tinyDelta(1e-40);
    const BigFloat hpX = baseX + tinyDelta;
    const BigFloat hpY(0.125);
    cfg.SetCenter(hpX, hpY);

    // Modify only centerY as a double; centerX remains unchanged.
    cfg.centerY = 0.25;

    BigFloat resolvedX, resolvedY;
    cfg.ResolveHpCenter(resolvedX, resolvedY);

    // X must retain its 1e-40 deep offset instead of being clobbered to double precision.
    ASSERT_TRUE(resolvedX == hpX);
    ASSERT_FALSE((resolvedX - baseX).IsZero());
    // Y must sync to the updated double value.
    ASSERT_NEAR(0.25, resolvedY.ToDouble(), 1e-15);

    // Symmetrically, modifying only centerX preserves high-precision Y.
    const BigFloat hpYDeep = BigFloat(0.125) + tinyDelta;
    cfg.SetCenter(baseX, hpYDeep);
    cfg.centerX = -0.5;
    cfg.ResolveHpCenter(resolvedX, resolvedY);
    ASSERT_NEAR(-0.5, resolvedX.ToDouble(), 1e-15);
    ASSERT_TRUE(resolvedY == hpYDeep);
}

TEST_CASE(MandelbrotEngine, DeepReferenceCacheInvalidateForcesRecomputation)
{
    DeepReferenceCache cache;
    FractalConfig cfg;
    cfg.width = 64;
    cfg.height = 48;
    cfg.zoom = 1e20;
    cfg.maxIterations = 300;
    cfg.SetCenter(-0.7436438870371587, 0.1318259042053119);

    DeepReferenceState s0{};
    ASSERT_TRUE(cache.Ensure(cfg, cfg.hpCenterX, cfg.hpCenterY, ExecutionMode::Automatic, s0));
    const uint64_t v0 = s0.orbitVersion;
    ASSERT_GT(v0, 0ull);
    ASSERT_GT(s0.refLen, 0);

    // Subsequent call without invalidation reuses the cached orbit version.
    DeepReferenceState s1{};
    ASSERT_TRUE(cache.Ensure(cfg, cfg.hpCenterX, cfg.hpCenterY, ExecutionMode::Automatic, s1));
    ASSERT_EQ(v0, s1.orbitVersion);

    // After Invalidate(), the next Ensure recomputes a fresh orbit with a newer version.
    cache.Invalidate();
    DeepReferenceState s2{};
    ASSERT_TRUE(cache.Ensure(cfg, cfg.hpCenterX, cfg.hpCenterY, ExecutionMode::Automatic, s2));
    ASSERT_GT(s2.orbitVersion, v0);
    ASSERT_EQ(s0.refLen, s2.refLen);
}

