#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>
#include "HighPrecision.h"

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
        // Returns elapsed time in milliseconds
        double Compute(
            const MandelbrotConfig& config,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr);

        // High-precision Perturbation Theory & Dynamic Rebasing solver for deep zooms
        double ComputeDeepZoom(
            const MandelbrotConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken = nullptr);

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
        // Returns true if the zoom step was applied, or false if zoom reached [MinZoom, MaxZoom] bounds.
        static bool StepZoomAnimation(
            MandelbrotConfig& config,
            const BigFloat& targetX,
            const BigFloat& targetY,
            double zoomFactor);

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
