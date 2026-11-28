#pragma once
#include "FractalCatalog.h"
#include "HighPrecision.h"
#include <array>
#include <cmath>

namespace Mandelbrot
{
    inline constexpr double DeepZoomThreshold = 1e11;
    inline constexpr double MaxZoom = 1e300;
    inline constexpr double MinZoom = 0.15;
    inline constexpr float NewtonRootStride = 1000000.0f;

    // Exterior orbit coloring style. EscapeTime is the classic smooth iteration coloring;
    // StripeAverage and OrbitTrap* evaluate geometric properties along each escaped orbit.
    enum class ColoringMode
    {
        EscapeTime = 0,
        StripeAverage = 1,
        OrbitTrapPoint = 2,
        OrbitTrapCross = 3,
        OrbitTrapRing = 4
    };

    inline constexpr std::array<ColoringMode, 5> GetAllColoringModes() noexcept
    {
        return {
            ColoringMode::EscapeTime,
            ColoringMode::StripeAverage,
            ColoringMode::OrbitTrapPoint,
            ColoringMode::OrbitTrapCross,
            ColoringMode::OrbitTrapRing
        };
    }

    inline constexpr wchar_t const* GetColoringModeName(ColoringMode mode) noexcept
    {
        switch (mode)
        {
        case ColoringMode::StripeAverage:  return L"Stripe Average";
        case ColoringMode::OrbitTrapPoint: return L"Orbit Trap (Origin Point)";
        case ColoringMode::OrbitTrapCross: return L"Orbit Trap (Cross / Stalks)";
        case ColoringMode::OrbitTrapRing:  return L"Orbit Trap (Unit Ring)";
        case ColoringMode::EscapeTime:
        default:                           return L"Escape Time (Standard)";
        }
    }

    // Automatic selects supported CPU optimizations, including guarded deep-zoom
    // iteration skipping. Scalar retains unaccelerated reference arithmetic.
    // Gpu uses DirectCompute shaders with automatic CPU fallback when needed.
    enum class ExecutionMode
    {
        Automatic,
        Scalar,
        Gpu
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
        ColoringMode coloringMode{ ColoringMode::EscapeTime };
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

        // Syncs each high-precision coordinate independently when a caller only modified
        // double centerX or centerY, preserving high-precision bits on the untouched axis.
        void ResolveHpCenter(BigFloat& outX, BigFloat& outY) const noexcept
        {
            if (std::abs(hpCenterX.ToDouble() - centerX) > 1e-14)
            {
                outX = BigFloat(centerX);
            }
            else if (&outX != &hpCenterX)
            {
                outX = hpCenterX;
            }
            if (std::abs(hpCenterY.ToDouble() - centerY) > 1e-14)
            {
                outY = BigFloat(centerY);
            }
            else if (&outY != &hpCenterY)
            {
                outY = hpCenterY;
            }
        }
    };
}
