#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "AutoExplore.h"
#include "DeepReferenceCache.h"
#include "FractalCatalog.h"
#include "FractalConfig.h"
#include "PixelSampling.h"
#include "ViewportNavigation.h"

namespace Mandelbrot
{
    class ColorTheme;
    class GpuFractalSolver;
    using MandelbrotConfig = FractalConfig;

    class MandelbrotEngine
    {
    public:
        static constexpr double DeepZoomThreshold = Mandelbrot::DeepZoomThreshold;
        static constexpr double MaxZoom = Mandelbrot::MaxZoom;
        static constexpr double MinZoom = Mandelbrot::MinZoom;

        using ExecutionMode = Mandelbrot::ExecutionMode;
        using ComplexOrbitPt = ComplexOrbitPoint;

        MandelbrotEngine();
        explicit MandelbrotEngine(ExecutionMode mode, bool allowSoftwareGpu = false);
        ~MandelbrotEngine();
        MandelbrotEngine(const MandelbrotEngine& other);
        MandelbrotEngine& operator=(const MandelbrotEngine& other);
        MandelbrotEngine(MandelbrotEngine&& other) noexcept;
        MandelbrotEngine& operator=(MandelbrotEngine&& other) noexcept;

        ExecutionMode GetExecutionMode() const noexcept { return m_executionMode; }
        void SetExecutionMode(ExecutionMode mode) noexcept { m_executionMode = mode; }
        bool IsGpuAvailable();

        // Computes fractional iteration values into outIterations (size: width * height)
        // Optional sparse subpixel samples refine boundaries and detailed regions. Center
        // samples stay unchanged for navigation; elapsed time includes the additional work.
        double Compute(
            const FractalConfig& config,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr,
            std::vector<SubpixelSamples>* subpixelSamples = nullptr);

        // High-precision Perturbation Theory & Dynamic Rebasing solver for deep zooms
        double ComputeDeepZoom(
            const FractalConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr,
            std::vector<SubpixelSamples>* subpixelSamples = nullptr);

        // Prepares and caches the reference orbit and BLA table for a full-frame deep view
        // before rendering horizontal movie bands.
        void PrepareDeepReference(
            const FractalConfig& config,
            const std::atomic<bool>* cancelToken = nullptr);

        // Fused GPU movie frame rendering helpers (boundary detection, DispatchIndirect 4x
        // subpixels, supersampled palette coloring, and linear-light temporal blending on GPU).
        bool CanUseGpu(const FractalConfig& config);
        bool BeginGpuMovieFrame(int width, int height);
        bool RenderGpuMovieBand(
            const FractalConfig& bandConfig,
            int topHalo,
            int visibleRows,
            int destRowOffset,
            int temporalStep,
            const ColorTheme& theme,
            float frequency,
            float phaseOffset,
            bool invert,
            uint32_t interiorColor,
            uint32_t* ioBandPixels,
            const std::atomic<bool>* cancelToken = nullptr);
        bool FinishGpuMovieFrame(int width, int height, uint32_t* outPixels);

        static void ZoomViewport(
            FractalConfig& config,
            double factor,
            double pivotPx,
            double pivotPy);

        static void PanViewport(
            FractalConfig& config,
            const BigFloat& startX,
            const BigFloat& startY,
            double deltaPx,
            double deltaPy);

        static bool StepZoomAnimation(
            FractalConfig& config,
            const BigFloat& targetX,
            const BigFloat& targetY,
            double zoomFactor,
            double panBlend = 1.0);

        static bool ShouldRetargetZoom(
            const FractalConfig& config,
            const std::vector<float>& iterations,
            const BigFloat& targetX,
            const BigFloat& targetY);

        static bool PickInterestingTarget(
            const FractalConfig& config,
            const std::vector<float>& iterations,
            BigFloat& outTargetX,
            BigFloat& outTargetY,
            uint64_t randomSeed = 0);

        static int RecommendMaxIterations(
            double zoom,
            FractalType type,
            int baseIterations = 300);

        static void ScreenToComplex(
            double px, double py,
            int width, int height,
            double centerX, double centerY, double zoom,
            double& outCx, double& outCy);

        static void ComplexToScreen(
            double cx, double cy,
            int width, int height,
            double centerX, double centerY, double zoom,
            double& outPx, double& outPy);

    private:
        GpuFractalSolver& EnsureGpuSolver();

        ExecutionMode m_executionMode{ ExecutionMode::Automatic };
        bool m_allowSoftwareGpu{ false };
        std::vector<float> m_sampleWeights;
        DeepReferenceCache m_deepCache;
        std::unique_ptr<GpuFractalSolver> m_gpuSolver;
    };
}
