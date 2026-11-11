#pragma once
#include <atomic>
#include <cstdint>
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
    using MandelbrotConfig = FractalConfig;

    class MandelbrotEngine
    {
    public:
        static constexpr double DeepZoomThreshold = Mandelbrot::DeepZoomThreshold;
        static constexpr double MaxZoom = Mandelbrot::MaxZoom;
        static constexpr double MinZoom = Mandelbrot::MinZoom;

        using ExecutionMode = Mandelbrot::ExecutionMode;
        using ComplexOrbitPt = ComplexOrbitPoint;

        MandelbrotEngine() = default;
        explicit MandelbrotEngine(ExecutionMode mode) noexcept : m_executionMode(mode) {}

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
        ExecutionMode m_executionMode{ ExecutionMode::Automatic };
        std::vector<float> m_sampleWeights;
        DeepReferenceCache m_deepCache;
    };
}
