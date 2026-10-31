#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>
#include "HighPrecision.h"
#include "PixelSampling.h"

namespace Mandelbrot
{
    enum class FractalType
    {
        Mandelbrot = 0,
        BurningShip,
        Tricorn,
        Newton
    };

    std::wstring GetFractalTypeName(FractalType type);
    const std::vector<FractalType>& GetAllFractalTypes();

    struct MandelbrotConfig
    {
        FractalType fractalType{ FractalType::Mandelbrot };
        double centerX{ -0.75 };
        double centerY{ 0.0 };
        BigFloat hpCenterX{ -0.75 };
        BigFloat hpCenterY{ 0.0 };
        double zoom{ 1.0 }; // Viewport span = 3.0 / zoom
        int width{ 1024 };
        int height{ 768 };
        int maxIterations{ 500 };
        bool smooth{ true };
        int renderStride{ 1 };   // 1 = full per-pixel; 2..4 = adaptive coarse grid + smooth/edge reconstruction
        bool edgeRefine{ true }; // When renderStride > 1, refine high-contrast boundary cells per-pixel
        bool juliaSet{ false }; // Quadratic dynamical plane, available as a Mandelbrot companion.
        BigFloat juliaX{ -0.75 };
        BigFloat juliaY{ 0.0 };

        void SetCenter(double x, double y)
        {
            centerX = x;
            centerY = y;
            hpCenterX = BigFloat(x);
            hpCenterY = BigFloat(y);
        }

        void SetCenter(const BigFloat& x, const BigFloat& y)
        {
            hpCenterX = x;
            hpCenterY = y;
            centerX = x.ToDouble();
            centerY = y.ToDouble();
        }
    };

    struct BookmarkLocation
    {
        std::wstring name;
        std::wstring description;
        double centerX;
        double centerY;
        double zoom;
        int maxIterations;
    };

    const std::vector<BookmarkLocation>& GetBookmarks(FractalType type = FractalType::Mandelbrot);

    class MandelbrotEngine
    {
    public:
        static constexpr double DeepZoomThreshold = 1e11;
        static constexpr double MaxZoom = 1e300;
        static constexpr double MinZoom = 0.15;

        MandelbrotEngine() = default;

        // Computes fractional iteration values into outIterations (size: width * height)
        // Optional sparse subpixel samples refine boundaries and detailed regions. Center
        // samples stay unchanged for navigation; elapsed time includes the additional work.
        double Compute(
            const MandelbrotConfig& config,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr,
            std::vector<SubpixelSamples>* subpixelSamples = nullptr);

        // High-precision Perturbation Theory & Dynamic Rebasing solver for deep zooms
        double ComputeDeepZoom(
            const MandelbrotConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr,
            std::vector<SubpixelSamples>* subpixelSamples = nullptr);

        // High-precision cursor-centered zoom without double cancellation
        static void ZoomViewport(
            MandelbrotConfig& config,
            double factor,
            double pivotPx,
            double pivotPy);

        // High-precision viewport pan without double cancellation
        static void PanViewport(
            MandelbrotConfig& config,
            const BigFloat& startX,
            const BigFloat& startY,
            double deltaPx,
            double deltaPy);

        // Smoothly steps an animated zoom (in or out) toward a target complex coordinate.
        // zoomFactor > 1.0 zooms in; zoomFactor < 1.0 zooms out.
        // panBlend in [0.0, 1.0] controls lateral pan acceleration (e.g., ramping 0->1 after retargeting for smooth ease-in).
        // Returns true if the zoom step was applied, or false if zoom reached [MinZoom, MaxZoom] bounds.
        static bool StepZoomAnimation(
            MandelbrotConfig& config,
            const BigFloat& targetX,
            const BigFloat& targetY,
            double zoomFactor,
            double panBlend = 1.0);

        // Evaluates whether zooming further into (targetX, targetY) is approaching a uniform/solid-color region
        // (such as an all-interior bulb or a flat-gradient exterior area) and a new target spot should be chosen.
        static bool ShouldRetargetZoom(
            const MandelbrotConfig& config,
            const std::vector<float>& iterations,
            const BigFloat& targetX,
            const BigFloat& targetY);

        // Randomly picks an interesting fractal boundary/filament coordinate to zoom in on,
        // refined to sub-pixel boundary precision so it stays interesting for as long as possible.
        static bool PickInterestingTarget(
            const MandelbrotConfig& config,
            const std::vector<float>& iterations,
            BigFloat& outTargetX,
            BigFloat& outTargetY,
            uint64_t randomSeed = 0);

        // Recommends an iteration budget scaled to the current magnification depth
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

        struct ComplexOrbitPt
        {
            double x;
            double y;
        };

    private:
        double ComputeJulia(const MandelbrotConfig& config, std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken, std::vector<SubpixelSamples>* subpixelSamples);
        std::vector<float> m_sampleWeights;
        // Cached high-precision reference orbit across consecutive deep-zoom frames
        bool m_hasCachedOrbit{ false };
        FractalType m_cachedType{ FractalType::Mandelbrot };
        BigFloat m_cachedRefX{};
        BigFloat m_cachedRefY{};
        BigFloat m_cachedEndZx{};
        BigFloat m_cachedEndZy{};
        int m_cachedActiveLimbs{ 0 };
        int m_cachedRefLen{ 0 };
        int m_cachedMaxIter{ 0 };
        double m_cachedSearchZoom{ 0.0 };
        bool m_cachedEscaped{ false };
        std::vector<ComplexOrbitPt> m_cachedOrbit;
    };
}
