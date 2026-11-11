#pragma once
#include "FractalCatalog.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot::detail
{
    // Computes |A + a| - |A| without catastrophic cancellation when |a| << |A|
    inline double DiffAbs(double A, double a) noexcept
    {
        const double sum = A + a;
        if ((A >= 0.0 && sum >= 0.0) || (A < 0.0 && sum < 0.0))
        {
            return A >= 0.0 ? a : -a;
        }
        return std::abs(sum) - std::abs(A);
    }

    // Analytic interior check for the quadratic Mandelbrot set cardioid and period-2 bulb
    inline bool IsInMandelbrotCardioidOrBulb(double x, double y) noexcept
    {
        // Main Cardioid test
        const double x_minus_quarter = x - 0.25;
        const double y2 = y * y;
        const double q = x_minus_quarter * x_minus_quarter + y2;
        if (q * (q + x_minus_quarter) <= 0.25 * y2) return true;

        // Period-2 bulb test: (x + 1)^2 + y^2 <= 1/16 (0.0625)
        const double x_plus_1 = x + 1.0;
        if (x_plus_1 * x_plus_1 + y2 <= 0.0625) return true;

        return false;
    }

    // Advances up to 3 post-bailout iterations and computes continuous normalized iteration count
    inline float ComputeSmoothEscapeIteration(
        FractalType type,
        int iter,
        double zr,
        double zi,
        double prevZr,
        double prevZi,
        double cx,
        double cy,
        double invLn2) noexcept
    {
        double zr2 = zr * zr;
        double zi2 = zi * zi;
        int smoothIter = iter;
        const double maxExtraMod2 = (type == FractalType::CubicMultibrot) ? 1e60 : 1e100;

        for (int extra = 0; extra < 3 && (zr2 + zi2) < maxExtraMod2; ++extra)
        {
            if (type == FractalType::Mandelbrot)
            {
                zi = 2.0 * zr * zi + cy;
                zr = zr2 - zi2 + cx;
            }
            else if (type == FractalType::BurningShip)
            {
                zi = -2.0 * std::abs(zr * zi) + cy;
                zr = zr2 - zi2 + cx;
            }
            else if (type == FractalType::Tricorn)
            {
                zi = -2.0 * zr * zi + cy;
                zr = zr2 - zi2 + cx;
            }
            else if (type == FractalType::Celtic)
            {
                zi = 2.0 * zr * zi + cy;
                zr = std::abs(zr2 - zi2) + cx;
            }
            else if (type == FractalType::Buffalo)
            {
                zi = -2.0 * std::abs(zr * zi) + cy;
                zr = std::abs(zr2 - zi2) + cx;
            }
            else if (type == FractalType::CubicMultibrot)
            {
                const double nextZr = zr * (zr2 - 3.0 * zi2) + cx;
                zi = zi * (3.0 * zr2 - zi2) + cy;
                zr = nextZr;
            }
            else // FractalType::Phoenix
            {
                const double nextZr = zr2 - zi2 + 0.5667 - 0.5 * prevZr;
                const double nextZi = 2.0 * zr * zi - 0.5 * prevZi;
                prevZr = zr;
                prevZi = zi;
                zr = nextZr;
                zi = nextZi;
            }
            zr2 = zr * zr;
            zi2 = zi * zi;
            smoothIter++;
        }

        constexpr double invLn3 = 0.91023922662683739361; // 1.0 / ln(3)
        const double invLnDegree = (type == FractalType::CubicMultibrot) ? invLn3 : invLn2;
        const double mod2 = zr2 + zi2;
        const double logZn = std::log(mod2) * 0.5;
        const double nu = smoothIter + 1.0 - std::log(logZn) * invLnDegree;
        return static_cast<float>(nu > 0.0 ? nu : 0.0);
    }

    // Advances one hardware-double perturbation step relative to reference state (rx, ry)
    inline void StepPerturbationDelta(
        FractalType type,
        double rx,
        double ry,
        double zx,
        double zy,
        double dcx,
        double dcy,
        double& dx,
        double& dy,
        double& prevDx,
        double& prevDy) noexcept
    {
        const double sx = rx + zx;
        const double sy = ry + zy;
        if (type == FractalType::Mandelbrot)
        {
            const double nextDx = sx * dx - sy * dy + dcx;
            const double nextDy = sx * dy + sy * dx + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else if (type == FractalType::BurningShip)
        {
            const double daX = DiffAbs(rx, dx);
            const double daY = DiffAbs(ry, dy);
            const double nextDx = sx * dx - sy * dy + dcx;
            const double nextDy = -2.0 * (std::abs(rx) * daY + std::abs(ry) * daX + daX * daY) + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else if (type == FractalType::Tricorn)
        {
            const double nextDx = sx * dx - sy * dy + dcx;
            const double nextDy = -(sx * dy + sy * dx) + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else if (type == FractalType::Celtic)
        {
            const double nextDx = DiffAbs(rx * rx - ry * ry, sx * dx - sy * dy) + dcx;
            const double nextDy = sx * dy + sy * dx + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else if (type == FractalType::Buffalo)
        {
            const double daX = DiffAbs(rx, dx);
            const double daY = DiffAbs(ry, dy);
            const double nextDx = DiffAbs(rx * rx - ry * ry, sx * dx - sy * dy) + dcx;
            const double nextDy = -2.0 * (std::abs(rx) * daY + std::abs(ry) * daX + daX * daY) + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else if (type == FractalType::CubicMultibrot)
        {
            const double pxTerm = (rx * rx - ry * ry) + (rx * zx - ry * zy) + (zx * zx - zy * zy);
            const double pyTerm = 2.0 * rx * ry + (rx * zy + ry * zx) + 2.0 * zx * zy;
            const double nextDx = pxTerm * dx - pyTerm * dy + dcx;
            const double nextDy = pxTerm * dy + pyTerm * dx + dcy;
            dx = nextDx;
            dy = nextDy;
        }
        else // FractalType::Phoenix
        {
            const double nextDx = sx * dx - sy * dy - 0.5 * prevDx;
            const double nextDy = sx * dy + sy * dx - 0.5 * prevDy;
            prevDx = dx;
            prevDy = dy;
            dx = nextDx;
            dy = nextDy;
        }
    }

    inline float EvalStandardPoint(
        FractalType type,
        double cx,
        double cy,
        int maxIter,
        bool smooth) noexcept
    {
        constexpr double bailoutRadius2 = 16.0; // R=4, R^2=16 for smooth coloring
        constexpr double invLn2 = 1.44269504088896340736; // 1.0 / ln(2)

        // Fast check for points in cardioid or main bulb (Mandelbrot only)
        if (type == FractalType::Mandelbrot && IsInMandelbrotCardioidOrBulb(cx, cy))
        {
            return -1.0f; // Inside set
        }

        if (type == FractalType::Newton)
        {
            double zr = cx;
            double zi = cy;
            int iter = 0;
            int rootIndex = -1;
            double distSq = 0.0;

            constexpr double ROOT1_Y = 0.86602540378443864676;
            constexpr double TOLERANCE_SQ = 1e-8;

            while (iter < maxIter)
            {
                const double zr2 = zr * zr;
                const double zi2 = zi * zi;
                const double r2 = zr2 + zi2;

                if (r2 < 1e-140)
                {
                    break;
                }

                // Check distance squared to Root 0: (1, 0)
                const double d0 = (zr - 1.0) * (zr - 1.0) + zi2;
                if (d0 < TOLERANCE_SQ)
                {
                    rootIndex = 0;
                    distSq = d0;
                    break;
                }

                // Check distance squared to Root 1: (-0.5, +ROOT1_Y)
                const double d1 = (zr + 0.5) * (zr + 0.5) + (zi - ROOT1_Y) * (zi - ROOT1_Y);
                if (d1 < TOLERANCE_SQ)
                {
                    rootIndex = 1;
                    distSq = d1;
                    break;
                }

                // Check distance squared to Root 2: (-0.5, -ROOT1_Y)
                const double d2 = (zr + 0.5) * (zr + 0.5) + (zi + ROOT1_Y) * (zi + ROOT1_Y);
                if (d2 < TOLERANCE_SQ)
                {
                    rootIndex = 2;
                    distSq = d2;
                    break;
                }

                const double invR4 = 1.0 / (r2 * r2);
                const double nextZr = (2.0 / 3.0) * zr + (1.0 / 3.0) * (zr2 - zi2) * invR4;
                const double nextZi = (2.0 / 3.0) * zi - (1.0 / 3.0) * (2.0 * zr * zi) * invR4;

                zr = nextZr;
                zi = nextZi;
                iter++;
            }

            if (rootIndex < 0 || iter >= maxIter)
            {
                return -1.0f; // Did not converge
            }

            float smoothVal = static_cast<float>(iter);
            if (smooth)
            {
                const double dist = std::max(std::sqrt(distSq), 1e-12);
                constexpr double logEps = -9.210340371976184; // log(1e-4)
                const double ratio = std::log(dist) / logEps;
                if (ratio > 1.0)
                {
                    const double frac = std::log(ratio) * invLn2;
                    smoothVal = static_cast<float>(std::max(0.0, iter - frac));
                }
            }
            return rootIndex * 1000000.0f + smoothVal;
        }

        double zr = 0.0;
        double zi = 0.0;
        double zr2 = 0.0;
        double zi2 = 0.0;
        double prevZr = 0.0;
        double prevZi = 0.0;
        double checkZr = 0.0;
        double checkZi = 0.0;
        double checkPrevZr = 0.0;
        double checkPrevZi = 0.0;
        int checkPeriod = 8;
        int stepsSinceCheck = 0;
        int iter = 0;

        switch (type)
        {
        case FractalType::Mandelbrot:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                zi = 2.0 * zr * zi + cy;
                zr = zr2 - zi2 + cx;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::BurningShip:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                zi = -2.0 * std::abs(zr * zi) + cy;
                zr = zr2 - zi2 + cx;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::Tricorn:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                zi = -2.0 * zr * zi + cy;
                zr = zr2 - zi2 + cx;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::Celtic:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                zi = 2.0 * zr * zi + cy;
                zr = std::abs(zr2 - zi2) + cx;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::Buffalo:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                zi = -2.0 * std::abs(zr * zi) + cy;
                zr = std::abs(zr2 - zi2) + cx;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::CubicMultibrot:
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                const double nextZr = zr * (zr2 - 3.0 * zi2) + cx;
                zi = zi * (3.0 * zr2 - zi2) + cy;
                zr = nextZr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::Phoenix:
            zr = cy;
            zi = cx;
            zr2 = zr * zr;
            zi2 = zi * zi;
            while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
            {
                const double nextZr = zr2 - zi2 + 0.5667 - 0.5 * prevZr;
                const double nextZi = 2.0 * zr * zi - 0.5 * prevZi;
                prevZr = zr;
                prevZi = zi;
                zr = nextZr;
                zi = nextZi;
                zr2 = zr * zr;
                zi2 = zi * zi;
                iter++;
                if (zr == checkZr && zi == checkZi && prevZr == checkPrevZr && prevZi == checkPrevZi)
                {
                    iter = maxIter;
                    break;
                }
                if (++stepsSinceCheck == checkPeriod)
                {
                    checkZr = zr;
                    checkZi = zi;
                    checkPrevZr = prevZr;
                    checkPrevZi = prevZi;
                    stepsSinceCheck = 0;
                    if (checkPeriod < 1024) checkPeriod <<= 1;
                }
            }
            break;

        case FractalType::Newton:
            break;
        }

        if (iter >= maxIter)
        {
            return -1.0f; // Inside set
        }

        if (smooth)
        {
            return ComputeSmoothEscapeIteration(type, iter, zr, zi, prevZr, prevZi, cx, cy, invLn2);
        }

        return static_cast<float>(iter);
    }
}
