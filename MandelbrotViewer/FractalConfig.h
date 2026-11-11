#pragma once
#include "FractalCatalog.h"
#include "HighPrecision.h"

namespace Mandelbrot
{
    inline constexpr double DeepZoomThreshold = 1e11;
    inline constexpr double MaxZoom = 1e300;
    inline constexpr double MinZoom = 0.15;

    // Automatic selects supported CPU optimizations, including guarded deep-zoom
    // iteration skipping. Scalar retains unaccelerated reference arithmetic.
    enum class ExecutionMode
    {
        Automatic,
        Scalar
    };

    struct ComplexOrbitPoint
    {
        double x;
        double y;
    };

    struct FractalConfig
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
        bool juliaSet{ false };  // Quadratic dynamical plane companion view
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
}
