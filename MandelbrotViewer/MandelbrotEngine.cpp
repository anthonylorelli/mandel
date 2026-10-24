#include "pch.h"
#include "MandelbrotEngine.h"
#include <cmath>
#include <chrono>
#include <algorithm>

namespace Mandelbrot
{
    std::wstring GetFractalTypeName(FractalType type)
    {
        switch (type)
        {
        case FractalType::Mandelbrot:  return L"Mandelbrot Set";
        case FractalType::BurningShip: return L"Burning Ship";
        case FractalType::Tricorn:     return L"Tricorn (Mandelbar)";
        case FractalType::Newton:      return L"Newton's Fractal";
        default:                       return L"Unknown";
        }
    }

    const std::vector<FractalType>& GetAllFractalTypes()
    {
        static const std::vector<FractalType> types = {
            FractalType::Mandelbrot,
            FractalType::BurningShip,
            FractalType::Tricorn,
            FractalType::Newton
        };
        return types;
    }

    const std::vector<BookmarkLocation>& GetBookmarks(FractalType type)
    {
        static const std::vector<BookmarkLocation> mandelbrotBookmarks = {
            { L"Overview", L"Full view of the iconic Mandelbrot set", -0.75, 0.0, 1.0, 300 },
            { L"Seahorse Valley", L"Intricate spiral filaments connecting cardioid and bulb", -0.7436438870371587, 0.1318259042053119, 85000.0, 1000 },
            { L"Elephant Valley", L"Elephant trunk formations on the East side of main cardioid", 0.274, 0.482, 250.0, 500 },
            { L"Triple Spiral", L"Hypnotic three-arm vortex of fractal energy", -0.088, 0.654, 1200.0, 800 },
            { L"Mini Mandelbrot Satellite", L"Self-similar satellite island along the primary spike", -1.768778833, -0.001738994, 30000.0, 1500 },
            { L"Scepter Valley", L"Branching candelabras and crowned antennae", -1.25066, 0.02012, 1000.0, 600 },
            { L"Deep Spiral Core", L"Deep zoom revealing dense cosmic spiral clusters", -0.743643135, 0.131825963, 1500000.0, 2500 }
        };

        static const std::vector<BookmarkLocation> burningShipBookmarks = {
            { L"Burning Ship Overview", L"Full view of the burning ship with glowing masts", -0.45, 0.5, 1.2, 400 },
            { L"The Grand Mast", L"Towering mast and rigging formations rising into the sky", -1.75, 0.03, 35.0, 800 },
            { L"The Armada Fleet", L"Endless miniature burning ships aligned along the horizon", -1.762, 0.028, 250.0, 1200 },
            { L"Celtic Cross", L"Intricate cross-shaped filigree deep within the ship framework", -1.861, 0.005, 800.0, 1500 },
            { L"Ship Bow & Ribs", L"Structural skeletal beams and burning embers", -0.47, 0.58, 15.0, 600 },
            { L"Ghost Ship Satellite", L"Self-similar phantom ship adrift in deep fractal space", -1.7495, 0.0255, 1800.0, 2000 }
        };

        static const std::vector<BookmarkLocation> tricornBookmarks = {
            { L"Tricorn Overview", L"Iconic three-cornered cardioid boundary and antennas", -0.4, 0.0, 1.0, 300 },
            { L"Triple Corner Cusp", L"Sharp triangular pinch point connecting perimeter ridges", 0.25, 0.0, 15.0, 500 },
            { L"Cauliflower Valley", L"Curving cauliflower plumes with anti-holomorphic symmetry", -0.75, 0.35, 80.0, 800 },
            { L"Mini Tricorn Satellite", L"Perfect miniature replica of the main tricorn body", -1.41, 0.0, 120.0, 1200 },
            { L"Antenna Spirals", L"Needle-thin branching spirals emanating from outer spikes", -0.12, 0.72, 300.0, 1000 },
            { L"Deep Tricorn Core", L"Dense geometric crystalline lattice in deep zoom", -1.4113, 0.002, 25000.0, 2500 }
        };

        static const std::vector<BookmarkLocation> newtonBookmarks = {
            { L"Overview", L"Full 3-fold symmetric view of the three attraction basins", 0.0, 0.0, 1.0, 100 },
            { L"Origin Rosette Cusp", L"Infinite self-similar rosettes where all three basins meet", 0.0, 0.0, 25.0, 200 },
            { L"Triple Junction Vortex", L"Intense whorls and alternating color tendrils on the boundary", -0.765, 0.0, 60.0, 300 },
            { L"Feathered Filament Border", L"Delicate feather-like lace separating convergence domains", 0.38, 0.65, 120.0, 400 },
            { L"Miniature Tri-Star", L"Self-similar three-point satellite embedded in boundary webs", -0.3252, 0.5638, 1500.0, 500 },
            { L"Deep Chaos Spiral", L"Ultra-deep fractal spiral vortex on the basin threshold", -0.50125, 0.86498, 8000.0, 800 }
        };

        switch (type)
        {
        case FractalType::BurningShip:
            return burningShipBookmarks;
        case FractalType::Tricorn:
            return tricornBookmarks;
        case FractalType::Newton:
            return newtonBookmarks;
        case FractalType::Mandelbrot:
        default:
            return mandelbrotBookmarks;
        }
    }

    void MandelbrotEngine::ScreenToComplex(
        double px, double py,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outCx, double& outCy)
    {
        const double aspect = static_cast<double>(height) / static_cast<double>(width > 0 ? width : 1);
        const double spanX = 3.0 / (zoom > 1e-12 ? zoom : 1e-12);
        const double spanY = spanX * aspect;

        outCx = centerX - (spanX * 0.5) + (px + 0.5) / static_cast<double>(width) * spanX;
        // Invert Y so positive imaginary is upward
        outCy = centerY + (spanY * 0.5) - (py + 0.5) / static_cast<double>(height) * spanY;
    }

    void MandelbrotEngine::ComplexToScreen(
        double cx, double cy,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outPx, double& outPy)
    {
        const double aspect = static_cast<double>(height) / static_cast<double>(width > 0 ? width : 1);
        const double spanX = 3.0 / (zoom > 1e-12 ? zoom : 1e-12);
        const double spanY = spanX * aspect;

        outPx = ((cx - (centerX - (spanX * 0.5))) / spanX) * static_cast<double>(width) - 0.5;
        outPy = (((centerY + (spanY * 0.5)) - cy) / spanY) * static_cast<double>(height) - 0.5;
    }

    inline bool IsInMainCardioidOrBulb(double x, double y)
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

    double MandelbrotEngine::Compute(
        const MandelbrotConfig& config,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken)
    {
        const auto startTime = std::chrono::high_resolution_clock::now();

        const int width = config.width;
        const int height = config.height;
        const int maxIter = config.maxIterations;
        const bool smooth = config.smooth;
        const FractalType type = config.fractalType;

        if (width <= 0 || height <= 0) return 0.0;

        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (outIterations.size() != totalPixels)
        {
            outIterations.resize(totalPixels);
        }

        const double aspect = static_cast<double>(height) / static_cast<double>(width);
        const double spanX = 3.0 / (config.zoom > 1e-12 ? config.zoom : 1e-12);
        const double spanY = spanX * aspect;

        const double minX = config.centerX - (spanX * 0.5);
        const double maxY = config.centerY + (spanY * 0.5);
        const double dx = spanX / static_cast<double>(width);
        const double dy = spanY / static_cast<double>(height);

        constexpr double bailoutRadius2 = 16.0; // R=4, R^2=16 for smooth coloring
        const double invLn2 = 1.0 / std::log(2.0);

        #pragma omp parallel for schedule(dynamic, 16)
        for (int py = 0; py < height; ++py)
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                continue;
            }

            const double cy = maxY - (py + 0.5) * dy;
            float* rowPtr = &outIterations[static_cast<size_t>(py) * width];

            for (int px = 0; px < width; ++px)
            {
                const double cx = minX + (px + 0.5) * dx;

                // Fast check for points in cardioid or main bulb (Mandelbrot only)
                if (type == FractalType::Mandelbrot && IsInMainCardioidOrBulb(cx, cy))
                {
                    rowPtr[px] = -1.0f; // Inside set
                    continue;
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

                        if (r2 < 1e-14)
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

                        // Newton step for z^3 - 1:
                        // z_{n+1} = (2/3)*z + (1/3)*(1 / z^2)
                        // 1 / z^2 = conj(z^2) / |z|^4 = (zr2 - zi2 - i * 2 * zr * zi) / (r2 * r2)
                        const double invR4 = 1.0 / (r2 * r2);
                        const double nextZr = (2.0 / 3.0) * zr + (1.0 / 3.0) * (zr2 - zi2) * invR4;
                        const double nextZi = (2.0 / 3.0) * zi - (1.0 / 3.0) * (2.0 * zr * zi) * invR4;

                        zr = nextZr;
                        zi = nextZi;
                        iter++;
                    }

                    if (rootIndex < 0 || iter >= maxIter)
                    {
                        rowPtr[px] = -1.0f; // Did not converge
                    }
                    else
                    {
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
                        rowPtr[px] = rootIndex * 1000000.0f + smoothVal;
                    }
                }
                else
                {
                    double zr = 0.0;
                    double zi = 0.0;
                    double zr2 = 0.0;
                    double zi2 = 0.0;
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
                        }
                        break;

                    case FractalType::BurningShip:
                        while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
                        {
                            // Burning Ship: z = (|Re(z)| + i|Im(z)|)^2 + c
                            // Negating the imaginary component orients the ship upright with masts rising into view
                            zi = -2.0 * std::abs(zr * zi) + cy;
                            zr = zr2 - zi2 + cx;
                            zr2 = zr * zr;
                            zi2 = zi * zi;
                            iter++;
                        }
                        break;

                    case FractalType::Tricorn:
                        while (iter < maxIter && (zr2 + zi2) <= bailoutRadius2)
                        {
                            // Tricorn (Mandelbar): z = (conj(z))^2 + c
                            zi = -2.0 * zr * zi + cy;
                            zr = zr2 - zi2 + cx;
                            zr2 = zr * zr;
                            zi2 = zi * zi;
                            iter++;
                        }
                        break;
                    }

                    if (iter >= maxIter)
                    {
                        rowPtr[px] = -1.0f; // Inside set
                    }
                    else
                    {
                        if (smooth)
                        {
                            // Continuous potential formula:
                            // nu = iter + 1 - ln(ln(|z|)) / ln(2)
                            // With |z| = sqrt(zr2 + zi2), ln(|z|) = 0.5 * ln(zr2 + zi2)
                            const double mod2 = zr2 + zi2;
                            const double logZn = std::log(mod2) * 0.5;
                            const double nu = iter + 1.0 - std::log(logZn) * invLn2;
                            rowPtr[px] = static_cast<float>(nu > 0.0 ? nu : 0.0);
                        }
                        else
                        {
                            rowPtr[px] = static_cast<float>(iter);
                        }
                    }
                }
            }
        }

        const auto endTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }
}
