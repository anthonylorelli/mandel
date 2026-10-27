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
            { L"Deep Spiral Core", L"Deep zoom revealing dense cosmic spiral clusters", -0.7436438870371587, 0.1318259042053119, 500000.0, 2000 }
        };

        static const std::vector<BookmarkLocation> burningShipBookmarks = {
            { L"Burning Ship Overview", L"Full view of the burning ship with glowing masts", -0.45, 0.5, 1.2, 400 },
            { L"The Grand Mast", L"Towering mast and rigging formations rising into the sky", -1.75, 0.03, 35.0, 800 },
            { L"The Armada Fleet", L"Endless miniature burning ships aligned along the horizon", -1.762, 0.028, 250.0, 1200 },
            { L"Celtic Cross", L"Intricate cross-shaped filigree deep within the ship framework", -1.861, 0.0, 800.0, 1500 },
            { L"Central Mast & Rigging", L"Towering spire and intricate ember rigging", -0.48, 0.86, 20.0, 600 },
            { L"Keel & Burning Ribs", L"Structural skeletal beam embers along the bottom keel", -0.32, -0.15, 15.0, 500 },
            { L"Ghost Ship Satellite", L"Self-similar phantom ship adrift in deep fractal space", -1.7495, 0.0255, 1800.0, 2000 }
        };

        static const std::vector<BookmarkLocation> tricornBookmarks = {
            { L"Tricorn Overview", L"Iconic three-cornered cardioid boundary and antennas", -0.4, 0.0, 1.0, 300 },
            { L"Triple Corner Cusp", L"Sharp triangular pinch point connecting perimeter ridges", 0.25, 0.0, 15.0, 500 },
            { L"Cauliflower Valley", L"Curving cauliflower plumes with anti-holomorphic symmetry", 0.0, 0.30, 25.0, 800 },
            { L"Filament Valley Web", L"Intricate filament network connecting outer cusps", 0.20, 0.50, 30.0, 800 },
            { L"Upper Horn Antenna", L"Towering crystal antenna horn reaching into outer space", 0.45, 1.00, 20.0, 600 },
            { L"Mini Tricorn Satellite", L"Perfect miniature replica of the main tricorn body", -1.40, 0.0, 100.0, 1000 },
            { L"Deep Tricorn Core", L"Dense geometric crystalline lattice in deep zoom", -1.4011, 0.0, 3000.0, 1500 }
        };

        static const std::vector<BookmarkLocation> newtonBookmarks = {
            { L"Overview", L"Full 3-fold symmetric view of the three attraction basins", 0.0, 0.0, 1.0, 100 },
            { L"Origin Rosette", L"Intricate self-similar rosettes where all three basins meet", 0.0, 0.0, 8.0, 150 },
            { L"Deep Origin Core", L"Infinite recursive rosette petals meeting at the singularity", 0.0, 0.0, 60.0, 250 },
            { L"Triple Basin Junction", L"Pinched boundary filaments where all three colors converge", -0.75, 0.0, 15.0, 200 },
            { L"Basin Boundary Web", L"Interlocking fractal webs along the domain threshold", -0.60, 0.20, 15.0, 200 },
            { L"Outer Pinched Filament", L"Needle-thin pinched filigree between alternating lobes", -1.40, 0.0, 15.0, 200 },
            { L"Interlocking Lobe Vortex", L"Swirling vortex of interleaved color basins", -1.20, 0.20, 15.0, 200 }
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

    void MandelbrotEngine::ZoomViewport(
        MandelbrotConfig& config,
        double factor,
        double pivotPx,
        double pivotPy)
    {
        if (config.width <= 0 || config.height <= 0 || factor <= 0.0) return;

        // Ensure hpCenterX/Y are synced if caller only set double centerX/Y
        if (std::abs(config.hpCenterX.ToDouble() - config.centerX) > 1e-14 ||
            std::abs(config.hpCenterY.ToDouble() - config.centerY) > 1e-14)
        {
            config.hpCenterX = BigFloat(config.centerX);
            config.hpCenterY = BigFloat(config.centerY);
        }

        const double oldZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double newZoom = std::clamp(oldZoom * factor, MinZoom, MaxZoom);
        const double effectiveFactor = newZoom / oldZoom;

        const double width = static_cast<double>(config.width);
        const double height = static_cast<double>(config.height);
        const double aspect = height / width;

        const double oldSpanX = 3.0 / oldZoom;
        const double oldSpanY = oldSpanX * aspect;

        // Offset of pivot relative to current center in complex plane (represented in double without adding centerX!)
        const double relX = ((pivotPx + 0.5) / width - 0.5) * oldSpanX;
        const double relY = (0.5 - (pivotPy + 0.5) / height) * oldSpanY;

        // To keep the pivot fixed on screen: newCenter = oldCenter + rel * (1 - 1 / effectiveFactor)
        const double scaleDiff = 1.0 - (1.0 / effectiveFactor);
        const double shiftX = relX * scaleDiff;
        const double shiftY = relY * scaleDiff;

        config.hpCenterX = config.hpCenterX + BigFloat(shiftX);
        config.hpCenterY = config.hpCenterY + BigFloat(shiftY);
        config.centerX = config.hpCenterX.ToDouble();
        config.centerY = config.hpCenterY.ToDouble();
        config.zoom = newZoom;
    }

    void MandelbrotEngine::PanViewport(
        MandelbrotConfig& config,
        const BigFloat& startX,
        const BigFloat& startY,
        double deltaPx,
        double deltaPy)
    {
        if (config.width <= 0 || config.height <= 0) return;

        const double width = static_cast<double>(config.width);
        const double height = static_cast<double>(config.height);
        const double aspect = height / width;
        const double zoom = std::clamp(config.zoom, MinZoom, MaxZoom);

        const double spanX = 3.0 / zoom;
        const double spanY = spanX * aspect;

        const double shiftX = -(deltaPx / width) * spanX;
        const double shiftY = (deltaPy / height) * spanY;

        config.hpCenterX = startX + BigFloat(shiftX);
        config.hpCenterY = startY + BigFloat(shiftY);
        config.centerX = config.hpCenterX.ToDouble();
        config.centerY = config.hpCenterY.ToDouble();
    }

    bool MandelbrotEngine::StepZoomAnimation(
        MandelbrotConfig& config,
        const BigFloat& targetX,
        const BigFloat& targetY,
        double zoomFactor)
    {
        if (zoomFactor <= 0.0 || std::isnan(zoomFactor) || std::isinf(zoomFactor) || zoomFactor == 1.0)
        {
            return false;
        }

        const double oldZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        if ((zoomFactor > 1.0 && oldZoom >= MaxZoom) || (zoomFactor < 1.0 && oldZoom <= MinZoom))
        {
            config.SetCenter(targetX, targetY);
            return false;
        }

        const double newZoom = std::clamp(oldZoom * zoomFactor, MinZoom, MaxZoom);
        const BigFloat dx = config.hpCenterX - targetX;
        const BigFloat dy = config.hpCenterY - targetY;

        if (dx.IsZero() && dy.IsZero())
        {
            config.SetCenter(targetX, targetY);
        }
        else
        {
            const double width = static_cast<double>(std::max(1, config.width));
            const double height = static_cast<double>(std::max(1, config.height));
            const double aspect = height / width;
            const double spanX = 3.0 / oldZoom;
            const double spanY = spanX * aspect;

            const double absDx = std::abs(dx.ToDouble());
            const double absDy = std::abs(dy.ToDouble());
            const double pxDistX = (absDx / spanX) * width;
            const double pxDistY = (absDy / spanY) * height;

            // If target is outside the central viewport or already within 0.25 pixels of center, lock directly
            if (absDx > spanX * 0.45 || absDy > spanY * 0.45 || (pxDistX < 0.25 && pxDistY < 0.25))
            {
                config.SetCenter(targetX, targetY);
            }
            else
            {
                const double stepRatio = (zoomFactor >= 1.0) ? (oldZoom / newZoom) : (newZoom / oldZoom);
                const double decay = stepRatio * stepRatio * stepRatio;
                const BigFloat bfDecay(decay);
                config.SetCenter(targetX + dx * bfDecay, targetY + dy * bfDecay);
            }
        }

        config.zoom = newZoom;
        return (newZoom > MinZoom && newZoom < MaxZoom);
    }

    int MandelbrotEngine::RecommendMaxIterations(
        double zoom,
        FractalType type,
        int baseIterations)
    {
        const double clampedZoom = std::max(1.0, zoom);
        const double log10Zoom = std::log10(clampedZoom);

        if (type == FractalType::Newton)
        {
            // Newton's method converges quadratically once separated, needing a gentler iteration growth
            const int scaled = static_cast<int>(std::round(100.0 + 45.0 * log10Zoom + 2.5 * log10Zoom * log10Zoom));
            return std::clamp(std::max(baseIterations, scaled), 50, 50000);
        }

        // Escape-time fractals require progressively higher iteration budgets as filaments thin out at depth
        const int scaled = static_cast<int>(std::round(300.0 + 100.0 * log10Zoom + 8.0 * log10Zoom * log10Zoom));
        return std::clamp(std::max(baseIterations, scaled), 50, 50000);
    }

    namespace
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
        if (config.width <= 0 || config.height <= 0) return 0.0;

        if (config.zoom >= DeepZoomThreshold)
        {
            BigFloat hpX = config.hpCenterX;
            BigFloat hpY = config.hpCenterY;
            // Sync if caller only modified double centerX / centerY
            if (std::abs(hpX.ToDouble() - config.centerX) > 1e-14 ||
                std::abs(hpY.ToDouble() - config.centerY) > 1e-14)
            {
                hpX = BigFloat(config.centerX);
                hpY = BigFloat(config.centerY);
            }
            return ComputeDeepZoom(config, hpX, hpY, outIterations, cancelToken);
        }

        const auto startTime = std::chrono::high_resolution_clock::now();

        const int width = config.width;
        const int height = config.height;
        const int maxIter = config.maxIterations;
        const bool smooth = config.smooth;
        const FractalType type = config.fractalType;

        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (outIterations.size() != totalPixels)
        {
            outIterations.resize(totalPixels);
        }

        const double aspect = static_cast<double>(height) / static_cast<double>(width);
        const double spanX = 3.0 / (config.zoom > 1e-12 ? config.zoom : 1e-12);
        const double spanY = spanX * aspect;

        const double dx = spanX / static_cast<double>(width);
        const double dy = spanY / static_cast<double>(height);
        const double halfW = static_cast<double>(width) * 0.5;
        const double halfH = static_cast<double>(height) * 0.5;

        constexpr double bailoutRadius2 = 16.0; // R=4, R^2=16 for smooth coloring
        const double invLn2 = 1.0 / std::log(2.0);

        #pragma omp parallel for schedule(dynamic, 16)
        for (int py = 0; py < height; ++py)
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                continue;
            }

            const double cy = config.centerY + (halfH - (static_cast<double>(py) + 0.5)) * dy;
            float* rowPtr = &outIterations[static_cast<size_t>(py) * width];

            for (int px = 0; px < width; ++px)
            {
                const double cx = config.centerX + ((static_cast<double>(px) + 0.5) - halfW) * dx;

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

    double MandelbrotEngine::ComputeDeepZoom(
        const MandelbrotConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
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
        const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double spanX = 3.0 / clampedZoom;
        const double spanY = spanX * aspect;
        const double stepX = spanX / static_cast<double>(width);
        const double stepY = spanY / static_cast<double>(height);
        const double invLn2 = 1.0 / std::log(2.0);

        // Adaptive precision limb count: only multiply the limbs required for the current zoom depth
        // plus ~96 guard bits (3 limbs), clamped to [4, BigFloat::N].
        const int activeLimbs = std::clamp(
            static_cast<int>(std::ceil((std::log2(clampedZoom) + 96.0) / 32.0)),
            4,
            BigFloat::N);

        if (type == FractalType::Newton)
        {
            // Reference pixel for Newton's Fractal: avoid exact origin singularity if center is (0, 0)
            int refPx = width / 2;
            int refPy = height / 2;
            const double centerMod2 = centerX.ToDouble() * centerX.ToDouble() + centerY.ToDouble() * centerY.ToDouble();
            if (centerMod2 < spanX * spanX * 0.25)
            {
                refPx = width / 3;
                refPy = height / 3;
            }

            const double refOffsetX = ((static_cast<double>(refPx) + 0.5) / static_cast<double>(width) - 0.5) * spanX;
            const double refOffsetY = (0.5 - (static_cast<double>(refPy) + 0.5) / static_cast<double>(height)) * spanY;
            const BigFloat cRefX = centerX + BigFloat(refOffsetX);
            const BigFloat cRefY = centerY + BigFloat(refOffsetY);

            std::vector<double> refX(static_cast<size_t>(maxIter) + 1);
            std::vector<double> refY(static_cast<size_t>(maxIter) + 1);
            int refLen = 0;

            BigFloat Zx = cRefX;
            BigFloat Zy = cRefY;
            const BigFloat oneThird = BigFloat(1.0) / BigFloat(3.0);
            const BigFloat twoThirds = BigFloat(2.0) / BigFloat(3.0);

            for (int m = 0; m <= maxIter; ++m)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return 0.0;
                }

                refX[m] = Zx.ToDouble();
                refY[m] = Zy.ToDouble();
                refLen = m;

                if (m == maxIter) break;

                const BigFloat Zx2 = Zx.Sqr(activeLimbs);
                const BigFloat Zy2 = Zy.Sqr(activeLimbs);
                const BigFloat R2 = Zx2 + Zy2;
                if (R2.IsZero()) break;

                const BigFloat R4 = R2.Sqr(activeLimbs);
                if (R4.IsZero()) break;
                const BigFloat invR4 = R4.Reciprocal();

                const BigFloat Zxy = Zx.Mul(Zy, activeLimbs);
                const BigFloat twoZxy = Zxy + Zxy;

                const BigFloat nextZx = Zx.Mul(twoThirds, activeLimbs) + (Zx2 - Zy2).Mul(invR4, activeLimbs).Mul(oneThird, activeLimbs);
                const BigFloat nextZy = Zy.Mul(twoThirds, activeLimbs) - twoZxy.Mul(invR4, activeLimbs).Mul(oneThird, activeLimbs);
                Zx = nextZx;
                Zy = nextZy;
            }

            constexpr double ROOT1_Y = 0.86602540378443864676;
            constexpr double TOLERANCE_SQ = 1e-8;

            #pragma omp parallel for schedule(dynamic, 16)
            for (int py = 0; py < height; ++py)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    continue;
                }

                const double dcy = static_cast<double>(refPy - py) * stepY;
                float* rowPtr = &outIterations[static_cast<size_t>(py) * width];

                for (int px = 0; px < width; ++px)
                {
                    const double dcx = static_cast<double>(px - refPx) * stepX;

                    double dx = dcx;
                    double dy = dcy;
                    double zr = 0.0;
                    double zi = 0.0;
                    int m = 0;
                    bool inPerturbation = (refLen > 0);

                    if (!inPerturbation)
                    {
                        zr = refX[0] + dx;
                        zi = refY[0] + dy;
                    }

                    int iter = 0;
                    int rootIndex = -1;
                    double distSq = 0.0;

                    while (iter < maxIter)
                    {
                        if (inPerturbation)
                        {
                            const double rx = refX[m];
                            const double ry = refY[m];
                            zr = rx + dx;
                            zi = ry + dy;

                            const double r2 = zr * zr + zi * zi;
                            if (r2 < 1e-140)
                            {
                                break;
                            }

                            const double d0 = (zr - 1.0) * (zr - 1.0) + zi * zi;
                            if (d0 < TOLERANCE_SQ)
                            {
                                rootIndex = 0;
                                distSq = d0;
                                break;
                            }

                            const double d1 = (zr + 0.5) * (zr + 0.5) + (zi - ROOT1_Y) * (zi - ROOT1_Y);
                            if (d1 < TOLERANCE_SQ)
                            {
                                rootIndex = 1;
                                distSq = d1;
                                break;
                            }

                            const double d2 = (zr + 0.5) * (zr + 0.5) + (zi + ROOT1_Y) * (zi + ROOT1_Y);
                            if (d2 < TOLERANCE_SQ)
                            {
                                rootIndex = 2;
                                distSq = d2;
                                break;
                            }

                            const double dz2 = dx * dx + dy * dy;
                            const double refR2 = rx * rx + ry * ry;

                            // Once |dz| has separated macroscopically (>= 1e-3 * |Z_m|), transition to standard double
                            if (m >= refLen || refR2 < 1e-140 || dz2 >= 1e-6 * refR2)
                            {
                                inPerturbation = false;
                                continue;
                            }

                            // Perturbation step for Newton's method:
                            // dz_{n+1} = (2/3)*dz - dz * (2*Z_m + dz) / (3 * Z_m^2 * (Z_m + dz)^2)
                            const double sx = 2.0 * rx + dx;
                            const double sy = 2.0 * ry + dy;
                            const double numX = dx * sx - dy * sy;
                            const double numY = dx * sy + dy * sx;

                            // p = Z_m * (Z_m + dz) = rx*zr - ry*zi + i*(rx*zi + ry*zr)
                            const double pxTerm = rx * zr - ry * zi;
                            const double pyTerm = rx * zi + ry * zr;
                            // q = p^2 = Z_m^2 * (Z_m + dz)^2
                            const double qx = pxTerm * pxTerm - pyTerm * pyTerm;
                            const double qy = 2.0 * pxTerm * pyTerm;
                            const double qMod2 = qx * qx + qy * qy;

                            if (qMod2 < 1e-280)
                            {
                                inPerturbation = false;
                                continue;
                            }

                            const double inv3QMod2 = 1.0 / (3.0 * qMod2);
                            const double fracX = (numX * qx + numY * qy) * inv3QMod2;
                            const double fracY = (numY * qx - numX * qy) * inv3QMod2;

                            dx = (2.0 / 3.0) * dx - fracX;
                            dy = (2.0 / 3.0) * dy - fracY;
                            m++;
                            iter++;
                        }
                        else
                        {
                            const double zr2 = zr * zr;
                            const double zi2 = zi * zi;
                            const double r2 = zr2 + zi2;

                            if (r2 < 1e-140)
                            {
                                break;
                            }

                            const double d0 = (zr - 1.0) * (zr - 1.0) + zi2;
                            if (d0 < TOLERANCE_SQ)
                            {
                                rootIndex = 0;
                                distSq = d0;
                                break;
                            }

                            const double d1 = (zr + 0.5) * (zr + 0.5) + (zi - ROOT1_Y) * (zi - ROOT1_Y);
                            if (d1 < TOLERANCE_SQ)
                            {
                                rootIndex = 1;
                                distSq = d1;
                                break;
                            }

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
                    }

                    if (rootIndex < 0 || iter >= maxIter)
                    {
                        rowPtr[px] = -1.0f;
                    }
                    else
                    {
                        float smoothVal = static_cast<float>(iter);
                        if (smooth)
                        {
                            const double dist = std::max(std::sqrt(distSq), 1e-12);
                            constexpr double logEps = -9.210340371976184;
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
            }

            const auto endTime = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
            return elapsed.count();
        }

        // Escape-time fractals (Mandelbrot, Burning Ship, Tricorn):
        // High-precision reference orbit + hardware double perturbation with Zhuoran's dynamic rebasing
        // and fast sample-grid reference search when the center pixel escapes early.
        constexpr double bailoutRadius2 = 16.0;

        struct ComplexOrbitPt
        {
            double x;
            double y;
        };

        auto computeRefOrbit = [&](int candidatePx, int candidatePy, std::vector<ComplexOrbitPt>& outOrbit) -> int
        {
            const double refOffsetX = ((static_cast<double>(candidatePx) + 0.5) / static_cast<double>(width) - 0.5) * spanX;
            const double refOffsetY = (0.5 - (static_cast<double>(candidatePy) + 0.5) / static_cast<double>(height)) * spanY;
            const BigFloat cRefX = centerX + BigFloat(refOffsetX);
            const BigFloat cRefY = centerY + BigFloat(refOffsetY);

            BigFloat Zx(0.0);
            BigFloat Zy(0.0);
            int len = 0;

            for (int m = 0; m <= maxIter; ++m)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return 0;
                }

                const double rx = Zx.ToDouble();
                const double ry = Zy.ToDouble();
                outOrbit[m] = { rx, ry };
                len = m;

                if (rx * rx + ry * ry > bailoutRadius2 || m == maxIter)
                {
                    break;
                }

                const BigFloat Zx2 = Zx.Sqr(activeLimbs);
                const BigFloat Zy2 = Zy.Sqr(activeLimbs);
                const BigFloat Zxy = Zx.Mul(Zy, activeLimbs);
                const BigFloat twoZxy = Zxy + Zxy;

                if (type == FractalType::Mandelbrot)
                {
                    Zy = twoZxy + cRefY;
                    Zx = Zx2 - Zy2 + cRefX;
                }
                else if (type == FractalType::BurningShip)
                {
                    Zy = -twoZxy.Abs() + cRefY;
                    Zx = Zx2 - Zy2 + cRefX;
                }
                else // FractalType::Tricorn
                {
                    Zy = -twoZxy + cRefY;
                    Zx = Zx2 - Zy2 + cRefX;
                }
            }
            return len;
        };

        std::vector<ComplexOrbitPt> bestOrbit(static_cast<size_t>(maxIter) + 1);
        int refPx = width / 2;
        int refPy = height / 2;
        int bestRefLen = computeRefOrbit(refPx, refPy, bestOrbit);

        if (cancelToken && cancelToken->load(std::memory_order_relaxed))
        {
            return 0.0;
        }

        // If the center pixel escaped before maxIter, probe an 8x8 sample grid across the viewport
        // (up to 3 upgrades) to find a reference pixel near a deeper minibrot/filament (minimum |z| at m == bestRefLen).
        if (bestRefLen < maxIter && width >= 4 && height >= 4)
        {
            std::vector<ComplexOrbitPt> candidateOrbit(static_cast<size_t>(maxIter) + 1);
            constexpr int gridN = 8;

            for (int attempt = 0; attempt < 3 && bestRefLen < maxIter; ++attempt)
            {
                int nextCandPx = -1;
                int nextCandPy = -1;
                double minMod2AtRefEnd = 1e300;
                const ComplexOrbitPt* orbitPtr = bestOrbit.data();

                for (int gy = 0; gy < gridN; ++gy)
                {
                    const int spy = ((2 * gy + 1) * height) / (2 * gridN);
                    const double dcy = static_cast<double>(refPy - spy) * stepY;

                    for (int gx = 0; gx < gridN; ++gx)
                    {
                        const int spx = ((2 * gx + 1) * width) / (2 * gridN);
                        if (spx == refPx && spy == refPy) continue;

                        const double dcx = static_cast<double>(spx - refPx) * stepX;
                        double dx = 0.0;
                        double dy = 0.0;
                        int m = 0;
                        int iter = 0;

                        while (iter < maxIter)
                        {
                            const double rx = orbitPtr[m].x;
                            const double ry = orbitPtr[m].y;
                            const double zx = rx + dx;
                            const double zy = ry + dy;
                            const double mod2 = zx * zx + zy * zy;

                            if (mod2 > bailoutRadius2)
                            {
                                break;
                            }

                            if (m == bestRefLen)
                            {
                                if (mod2 < minMod2AtRefEnd)
                                {
                                    minMod2AtRefEnd = mod2;
                                    nextCandPx = spx;
                                    nextCandPy = spy;
                                }
                                break;
                            }

                            const double dz2 = dx * dx + dy * dy;
                            if (m > 0 && mod2 < dz2)
                            {
                                dx = zx;
                                dy = zy;
                                m = 0;
                                continue;
                            }

                            if (type == FractalType::Mandelbrot)
                            {
                                const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                                const double nextDy = 2.0 * (rx * dy + ry * dx + dx * dy) + dcy;
                                dx = nextDx;
                                dy = nextDy;
                            }
                            else if (type == FractalType::BurningShip)
                            {
                                const double daX = DiffAbs(rx, dx);
                                const double daY = DiffAbs(ry, dy);
                                const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                                const double nextDy = -2.0 * (std::abs(rx) * daY + std::abs(ry) * daX + daX * daY) + dcy;
                                dx = nextDx;
                                dy = nextDy;
                            }
                            else
                            {
                                const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                                const double nextDy = -2.0 * (rx * dy + ry * dx + dx * dy) + dcy;
                                dx = nextDx;
                                dy = nextDy;
                            }

                            m++;
                            iter++;
                        }
                    }
                }

                if (nextCandPx < 0)
                {
                    break;
                }

                const int candLen = computeRefOrbit(nextCandPx, nextCandPy, candidateOrbit);
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return 0.0;
                }

                if (candLen > bestRefLen)
                {
                    bestOrbit.swap(candidateOrbit);
                    bestRefLen = candLen;
                    refPx = nextCandPx;
                    refPy = nextCandPy;
                }
                else
                {
                    break;
                }
            }
        }

        const int refLen = bestRefLen;
        const ComplexOrbitPt* orbitPtr = bestOrbit.data();

        #pragma omp parallel for schedule(dynamic, 16)
        for (int py = 0; py < height; ++py)
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                continue;
            }

            const double dcy = static_cast<double>(refPy - py) * stepY;
            float* rowPtr = &outIterations[static_cast<size_t>(py) * width];

            for (int px = 0; px < width; ++px)
            {
                const double dcx = static_cast<double>(px - refPx) * stepX;
                double dx = 0.0;
                double dy = 0.0;
                int m = 0;
                int iter = 0;
                double escapedMod2 = 0.0;

                switch (type)
                {
                case FractalType::Mandelbrot:
                    while (iter < maxIter)
                    {
                        double rx = orbitPtr[m].x;
                        double ry = orbitPtr[m].y;
                        const double zx = rx + dx;
                        const double zy = ry + dy;
                        const double mod2 = zx * zx + zy * zy;

                        if (mod2 > bailoutRadius2)
                        {
                            escapedMod2 = mod2;
                            break;
                        }

                        const double dz2 = dx * dx + dy * dy;
                        if (m == refLen || (m > 0 && mod2 < dz2))
                        {
                            dx = zx;
                            dy = zy;
                            m = 0;
                            rx = 0.0;
                            ry = 0.0;
                        }

                        const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                        const double nextDy = 2.0 * (rx * dy + ry * dx + dx * dy) + dcy;
                        dx = nextDx;
                        dy = nextDy;
                        m++;
                        iter++;
                    }
                    break;

                case FractalType::BurningShip:
                    while (iter < maxIter)
                    {
                        double rx = orbitPtr[m].x;
                        double ry = orbitPtr[m].y;
                        const double zx = rx + dx;
                        const double zy = ry + dy;
                        const double mod2 = zx * zx + zy * zy;

                        if (mod2 > bailoutRadius2)
                        {
                            escapedMod2 = mod2;
                            break;
                        }

                        const double dz2 = dx * dx + dy * dy;
                        if (m == refLen || (m > 0 && mod2 < dz2))
                        {
                            dx = zx;
                            dy = zy;
                            m = 0;
                            rx = 0.0;
                            ry = 0.0;
                        }

                        const double daX = DiffAbs(rx, dx);
                        const double daY = DiffAbs(ry, dy);
                        const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                        const double nextDy = -2.0 * (std::abs(rx) * daY + std::abs(ry) * daX + daX * daY) + dcy;
                        dx = nextDx;
                        dy = nextDy;
                        m++;
                        iter++;
                    }
                    break;

                case FractalType::Tricorn:
                default:
                    while (iter < maxIter)
                    {
                        double rx = orbitPtr[m].x;
                        double ry = orbitPtr[m].y;
                        const double zx = rx + dx;
                        const double zy = ry + dy;
                        const double mod2 = zx * zx + zy * zy;

                        if (mod2 > bailoutRadius2)
                        {
                            escapedMod2 = mod2;
                            break;
                        }

                        const double dz2 = dx * dx + dy * dy;
                        if (m == refLen || (m > 0 && mod2 < dz2))
                        {
                            dx = zx;
                            dy = zy;
                            m = 0;
                            rx = 0.0;
                            ry = 0.0;
                        }

                        const double nextDx = 2.0 * (rx * dx - ry * dy) + (dx * dx - dy * dy) + dcx;
                        const double nextDy = -2.0 * (rx * dy + ry * dx + dx * dy) + dcy;
                        dx = nextDx;
                        dy = nextDy;
                        m++;
                        iter++;
                    }
                    break;
                }

                if (iter >= maxIter)
                {
                    rowPtr[px] = -1.0f;
                }
                else
                {
                    if (smooth)
                    {
                        const double logZn = std::log(escapedMod2) * 0.5;
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

        const auto endTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }
}
