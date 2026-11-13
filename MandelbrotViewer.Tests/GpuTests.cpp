#include "pch.h"
#include "TestHarness.h"
#include "ColorTheme.h"
#include "GpuFractalSolver.h"
#include "MandelbrotEngine.h"
#include "MovieExport.h"
#include "MoviePlan.h"

using namespace Mandelbrot;

namespace
{
    double FilteredPixelError(
        const std::vector<uint32_t>& a,
        const std::vector<uint32_t>& b,
        int width,
        int height)
    {
        double squared = 0.0;
        for (int y = 1; y < height - 1; ++y)
        {
            for (int x = 1; x < width - 1; ++x)
            {
                for (int shift : { 0, 8, 16 })
                {
                    double delta = 0.0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const size_t i = static_cast<size_t>(y + dy) * static_cast<size_t>(width) + static_cast<size_t>(x + dx);
                            delta += static_cast<int>((a[i] >> shift) & 255) - static_cast<int>((b[i] >> shift) & 255);
                        }
                    }
                    squared += (delta * delta) / 81.0;
                }
            }
        }
        return std::sqrt(squared / ((width - 2.0) * (height - 2.0) * 3.0));
    }

    void AssertValidSubpixels(const std::vector<SubpixelSamples>& samples, size_t totalPixels)
    {
        for (size_t i = 0; i < samples.size(); ++i)
        {
            ASSERT_LT(samples[i].pixelIndex, totalPixels);
            ASSERT_GT(samples[i].weight, 0.0f);
            ASSERT_LE(samples[i].weight, 1.0f);
            if (i > 0)
            {
                ASSERT_LT(samples[i - 1].pixelIndex, samples[i].pixelIndex);
            }
            for (float v : samples[i].iterations)
            {
                ASSERT_TRUE(std::isfinite(v));
            }
        }
    }

    void CompareCpuAndGpuRender(
        MandelbrotEngine& cpu,
        MandelbrotEngine& gpu,
        const FractalConfig& cfg,
        double maxFilteredError = 4.5)
    {
        std::vector<float> cpuIters, gpuIters;
        std::vector<SubpixelSamples> cpuSubs, gpuSubs;
        cpu.Compute(cfg, cpuIters, nullptr, &cpuSubs);
        gpu.Compute(cfg, gpuIters, nullptr, &gpuSubs);

        const size_t total = static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height);
        ASSERT_EQ(total, cpuIters.size());
        ASSERT_EQ(total, gpuIters.size());
        AssertValidSubpixels(gpuSubs, total);

        ColorTheme theme;
        std::vector<uint32_t> cpuPixels(total), gpuPixels(total);
        theme.Recolor(cpuIters, cpuPixels.data(), total, 1.0f, 0.0f, false, 0xFF000000, cfg.fractalType, cfg.width, &cpuSubs, false);
        theme.Recolor(gpuIters, gpuPixels.data(), total, 1.0f, 0.0f, false, 0xFF000000, cfg.fractalType, cfg.width, &gpuSubs, false);

        const double err = FilteredPixelError(cpuPixels, gpuPixels, cfg.width, cfg.height);
        ASSERT_LT(err, maxFilteredError);
    }
}

TEST_CASE(Gpu, ShallowShadersMatchCpuAcrossAllFormulasAndJulia)
{
    MandelbrotEngine cpu(ExecutionMode::Automatic);
    MandelbrotEngine gpu(ExecutionMode::Gpu, true);
    ASSERT_TRUE(gpu.IsGpuAvailable());

    for (FractalType type : GetAllFractalTypes())
    {
        const auto& overview = GetBookmarks(type).front();
        for (bool smooth : { true, false })
        {
            FractalConfig cfg;
            cfg.fractalType = type;
            cfg.width = 129;
            cfg.height = 97;
            cfg.SetCenter(overview.centerX, overview.centerY);
            cfg.zoom = std::min(overview.zoom, 4.0);
            cfg.maxIterations = 256;
            cfg.smooth = smooth;
            CompareCpuAndGpuRender(cpu, gpu, cfg, smooth ? 4.5 : 5.5);
        }
    }

    // Shallow Julia sets for Mandelbrot and Burning Ship
    {
        FractalConfig juliaM;
        juliaM.fractalType = FractalType::Mandelbrot;
        juliaM.juliaSet = true;
        juliaM.juliaX = BigFloat(-0.8);
        juliaM.juliaY = BigFloat(0.156);
        juliaM.width = 129;
        juliaM.height = 97;
        juliaM.SetCenter(0.0, 0.0);
        juliaM.zoom = 1.2;
        juliaM.maxIterations = 256;
        juliaM.smooth = true;
        CompareCpuAndGpuRender(cpu, gpu, juliaM, 4.5);

        FractalConfig juliaBs = juliaM;
        juliaBs.fractalType = FractalType::BurningShip;
        juliaBs.juliaX = BigFloat(-1.755);
        juliaBs.juliaY = BigFloat(-0.028);
        CompareCpuAndGpuRender(cpu, gpu, juliaBs, 5.0);
    }
}

TEST_CASE(Gpu, PerturbationAndBlaShadersMatchCpuFromMidToUltraDeepZoom)
{
    MandelbrotEngine cpu(ExecutionMode::Automatic);
    MandelbrotEngine gpu(ExecutionMode::Gpu, true);
    ASSERT_TRUE(gpu.IsGpuAvailable());

    const BigFloat seahorseX("-0.743643887037158704752191506114774");
    const BigFloat seahorseY("0.131825904205311970493132056385139");
    const BigFloat needleX("-1.74975914513036651613063305841522638950626266680180909383610018508190914266589615");
    const BigFloat needleY("0.00000000368513794929231850000185950626266680180909383610018508190914266589615");

    // 1. Mid-zoom LDS perturbation (zoom = 8.5e4)
    {
        FractalConfig cfg;
        cfg.width = 129;
        cfg.height = 97;
        cfg.SetCenter(seahorseX, seahorseY);
        cfg.zoom = 8.5e4;
        cfg.maxIterations = 800;
        cfg.smooth = true;
        CompareCpuAndGpuRender(cpu, gpu, cfg, 4.5);
    }

    // 2. Deep-zoom LDS + float32 BLA perturbation (zoom = 1e16 and 1e28)
    for (double zoom : { 1e16, 1e28 })
    {
        FractalConfig cfg;
        cfg.width = 129;
        cfg.height = 97;
        cfg.SetCenter(seahorseX, seahorseY);
        cfg.zoom = zoom;
        cfg.maxIterations = 1400;
        cfg.smooth = true;
        CompareCpuAndGpuRender(cpu, gpu, cfg, 4.5);
    }

    // 3. Ultra-deep FloatExp + FloatExp BLA perturbation (zoom = 1e60 and 1e150)
    {
        FractalConfig cfg;
        cfg.width = 97;
        cfg.height = 73;
        cfg.SetCenter(needleX, needleY);
        cfg.zoom = 1e60;
        cfg.maxIterations = 2000;
        cfg.smooth = true;
        CompareCpuAndGpuRender(cpu, gpu, cfg, 4.5);

        FractalConfig tip;
        tip.width = 65;
        tip.height = 49;
        tip.SetCenter(0.0, 1.0);
        tip.zoom = 1e150;
        tip.maxIterations = 1200;
        tip.smooth = true;
        CompareCpuAndGpuRender(cpu, gpu, tip, 4.5);
    }

    // 4. Non-Mandelbrot GPU perturbation formulas & seamless CPU fallback
    for (FractalType type : { FractalType::BurningShip, FractalType::Tricorn, FractalType::Celtic,
                              FractalType::Buffalo, FractalType::CubicMultibrot,
                              FractalType::Phoenix, FractalType::Newton })
    {
        const auto& bm = GetBookmarks(type);
        const auto& loc = (bm.size() > 1) ? bm[1] : bm.front();
        for (double zoom : { 400.0, 2e6 })
        {
            FractalConfig cfg;
            cfg.fractalType = type;
            cfg.width = 97;
            cfg.height = 73;
            cfg.SetCenter(loc.centerX, loc.centerY);
            cfg.zoom = zoom;
            cfg.maxIterations = 400;
            cfg.smooth = true;
            CompareCpuAndGpuRender(cpu, gpu, cfg, 4.5);
        }
    }

    // Uniform deep interior must be exact -1.0f with zero subpixel refinements
    {
        FractalConfig interior;
        interior.width = 65;
        interior.height = 49;
        interior.SetCenter(0.0, 0.0);
        interior.zoom = 1e40;
        interior.maxIterations = 600;
        std::vector<float> iters;
        std::vector<SubpixelSamples> subs;
        gpu.Compute(interior, iters, nullptr, &subs);
        ASSERT_TRUE(subs.empty());
        for (float v : iters) ASSERT_EQ(-1.0f, v);
    }

    // Pre-canceled token leaves subpixels empty
    {
        FractalConfig cfg;
        cfg.width = 128;
        cfg.height = 96;
        cfg.SetCenter(-0.75, 0.1);
        cfg.zoom = 100.0;
        cfg.maxIterations = 500;
        std::atomic<bool> cancel{ true };
        std::vector<float> iters;
        std::vector<SubpixelSamples> subs;
        gpu.Compute(cfg, iters, &cancel, &subs);
        ASSERT_TRUE(subs.empty());
    }
}

TEST_CASE(Gpu, MovieRenderMatchesCpuAndRemainsBandIndependent)
{
    MoviePlan gpuPlan;
    gpuPlan.width = 96;
    gpuPlan.height = 64;
    gpuPlan.fps = 24;
    gpuPlan.seconds = 1;
    gpuPlan.motionSmoothing = true;
    gpuPlan.useGpu = true;
    gpuPlan.start.SetCenter(
        BigFloat("-0.743643887037158704752191506114774"),
        BigFloat("0.131825904205311970493132056385139"));
    gpuPlan.targetX = gpuPlan.start.hpCenterX;
    gpuPlan.targetY = gpuPlan.start.hpCenterY;
    gpuPlan.start.zoom = 2.0;
    gpuPlan.endZoom = 1e20;
    gpuPlan.start.maxIterations = 900;

    MoviePlan cpuPlan = gpuPlan;
    cpuPlan.useGpu = false;

    std::atomic<bool> cancel{};
    MandelbrotEngine sharedGpu(ExecutionMode::Gpu, true);
    MandelbrotEngine sharedCpu(ExecutionMode::Automatic);

    for (double frame : { 0.0, 8.0, 18.0, 23.0 })
    {
        const auto gpuWhole = RenderMovieFrame(gpuPlan, frame, cancel, {}, gpuPlan.height, &sharedGpu);
        const auto gpuBanded = RenderMovieFrame(gpuPlan, frame, cancel, {}, 8, &sharedGpu);
        const auto cpuWhole = RenderMovieFrame(cpuPlan, frame, cancel, {}, cpuPlan.height, &sharedCpu);

        ASSERT_EQ(gpuWhole.size(), gpuBanded.size());
        for (size_t i = 0; i < gpuWhole.size(); ++i)
        {
            for (int shift : { 0, 8, 16 })
            {
                ASSERT_NEAR((gpuWhole[i] >> shift) & 255, (gpuBanded[i] >> shift) & 255, 1);
            }
        }

        const double err = FilteredPixelError(cpuWhole, gpuWhole, gpuPlan.width, gpuPlan.height);
        ASSERT_LT(err, 4.5);
    }
}
