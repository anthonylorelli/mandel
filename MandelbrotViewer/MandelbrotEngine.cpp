#include "pch.h"
#include "MandelbrotEngine.h"
#include <cmath>
#include <chrono>
#include <algorithm>
#include <random>

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
        double zoomFactor,
        double panBlend)
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

            // If target is far outside the surrounding viewport or already within 0.25 pixels of center, lock directly
            if (absDx > spanX * 2.0 || absDy > spanY * 2.0 || (pxDistX < 0.25 && pxDistY < 0.25))
            {
                config.SetCenter(targetX, targetY);
            }
            else
            {
                const double stepRatio = (zoomFactor >= 1.0) ? (oldZoom / newZoom) : (newZoom / oldZoom);
                const double clampedBlend = std::clamp(panBlend, 0.05, 1.0);
                const double decay = std::pow(stepRatio, 3.0 * clampedBlend);
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

        inline bool SameBasin(float a, float b, FractalType type) noexcept
        {
            if ((a < 0.0f) != (b < 0.0f)) return false;
            if (type == FractalType::Newton && a >= 0.0f)
            {
                return static_cast<int>(a / 1000000.0f) == static_cast<int>(b / 1000000.0f);
            }
            return true;
        }

        template <typename EvalPixelFn>
        void ExecuteRenderPass(
            int width,
            int height,
            int renderStride,
            bool edgeRefine,
            bool smooth,
            FractalType type,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken,
            EvalPixelFn&& evalPixel)
        {
            const int stride = std::clamp(renderStride, 1, 8);
            if (stride <= 1 || width < 8 || height < 8)
            {
                // Expensive rows vary widely in cost; dispatch one at a time to avoid a long tail.
                #pragma omp parallel for schedule(dynamic, 1)
                for (int py = 0; py < height; ++py)
                {
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        continue;
                    }

                    float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                    for (int px = 0; px < width; ++px)
                    {
                        rowPtr[px] = evalPixel(px, py);
                    }
                }
                return;
            }

            // Pass 1: Evaluate coarse grid vertices (stride x stride, including right and bottom borders).
            // Only coarse vertex coordinates are written in Pass 1.
            const int numCoarseY = (height - 1 + stride - 1) / stride + 1;
            #pragma omp parallel for schedule(dynamic, 1)
            for (int cyIdx = 0; cyIdx < numCoarseY; ++cyIdx)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    continue;
                }

                const int py = std::min(cyIdx * stride, height - 1);
                float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                for (int px = 0; px < width; px += stride)
                {
                    rowPtr[px] = evalPixel(px, py);
                }
                if ((width - 1) % stride != 0)
                {
                    rowPtr[width - 1] = evalPixel(width - 1, py);
                }
            }

            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                return;
            }

            // Pass 2: Each cell [x0, x1] x [y0, y1] writes strictly to its disjoint ownership region
            // [x0, writeXEnd) x [y0, writeYEnd) (skipping the 4 coarse corners already computed in Pass 1).
            // This guarantees zero OpenMP thread data races and 100% deterministic edge evaluation.
            const int numCellsY = numCoarseY - 1;
            const float smoothThreshold = edgeRefine ? (1.25f * static_cast<float>(stride)) : 24.0f;

            #pragma omp parallel for schedule(dynamic, 1)
            for (int cyIdx = 0; cyIdx < numCellsY; ++cyIdx)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    continue;
                }

                const int y0 = cyIdx * stride;
                const int y1 = std::min(y0 + stride, height - 1);
                if (y0 >= y1) continue;

                const int writeYEnd = (y1 == height - 1) ? height : y1;
                const int yPrev = std::max(0, y0 - stride);
                const int yNext = std::min(height - 1, y1 + stride);
                const float invDy = 1.0f / static_cast<float>(y1 - y0);

                for (int x0 = 0; x0 < width - 1; x0 += stride)
                {
                    const int x1 = std::min(x0 + stride, width - 1);
                    if (x0 >= x1) continue;

                    const int writeXEnd = (x1 == width - 1) ? width : x1;
                    const int xPrev = std::max(0, x0 - stride);
                    const int xNext = std::min(width - 1, x1 + stride);

                    const float v00 = outIterations[static_cast<size_t>(y0) * width + x0];
                    const float v10 = outIterations[static_cast<size_t>(y0) * width + x1];
                    const float v01 = outIterations[static_cast<size_t>(y1) * width + x0];
                    const float v11 = outIterations[static_cast<size_t>(y1) * width + x1];

                    auto isCoarseCorner = [&](int px, int py) noexcept -> bool
                    {
                        return (py == y0 || py == y1) && (px == x0 || px == x1);
                    };

                    const int xm = (x0 + x1) / 2;
                    const int ym = (y0 + y1) / 2;
                    float midpointValue = 0.0f;
                    bool hasMidpoint = false;
                    auto sampleMidpoint = [&]() -> float
                    {
                        if (!hasMidpoint)
                        {
                            midpointValue = isCoarseCorner(xm, ym)
                                ? outIterations[static_cast<size_t>(ym) * width + xm]
                                : evalPixel(xm, ym);
                            hasMidpoint = true;
                        }
                        return midpointValue;
                    };

                    // Case 1: All 4 corners are interior (-1.0f).
                    // When edgeRefine is enabled, also check the 1-ring of already-computed coarse vertices so cells
                    // grazing the interior/exterior boundary never overwrite a 1-pixel exterior indentation with -1.0f.
                    if (v00 < 0.0f && v10 < 0.0f && v01 < 0.0f && v11 < 0.0f)
                    {
                        bool interiorSafe = true;
                        if (edgeRefine)
                        {
                            const float nLeft0 = outIterations[static_cast<size_t>(y0) * width + xPrev];
                            const float nRight0 = outIterations[static_cast<size_t>(y0) * width + xNext];
                            const float nLeft1 = outIterations[static_cast<size_t>(y1) * width + xPrev];
                            const float nRight1 = outIterations[static_cast<size_t>(y1) * width + xNext];
                            const float nTop0 = outIterations[static_cast<size_t>(yPrev) * width + x0];
                            const float nBot0 = outIterations[static_cast<size_t>(yNext) * width + x0];
                            const float nTop1 = outIterations[static_cast<size_t>(yPrev) * width + x1];
                            const float nBot1 = outIterations[static_cast<size_t>(yNext) * width + x1];
                            interiorSafe = (nLeft0 < 0.0f && nRight0 < 0.0f && nLeft1 < 0.0f && nRight1 < 0.0f &&
                                            nTop0 < 0.0f && nBot0 < 0.0f && nTop1 < 0.0f && nBot1 < 0.0f);
                            if (interiorSafe)
                            {
                                // Similar corner and neighbor values can still conceal a thin exterior filament.
                                interiorSafe = sampleMidpoint() < 0.0f;
                            }
                        }

                        if (interiorSafe)
                        {
                            for (int py = y0; py < writeYEnd; ++py)
                            {
                                float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                                for (int px = x0; px < writeXEnd; ++px)
                                {
                                    if (!isCoarseCorner(px, py))
                                    {
                                        rowPtr[px] = -1.0f;
                                    }
                                }
                            }
                            continue;
                        }
                    }

                    // Case 2: All 4 corners escaped in the same basin
                    if (v00 >= 0.0f && v10 >= 0.0f && v01 >= 0.0f && v11 >= 0.0f &&
                        SameBasin(v00, v10, type) && SameBasin(v00, v01, type) && SameBasin(v00, v11, type))
                    {
                        const float minV = std::min({ v00, v10, v01, v11 });
                        const float maxV = std::max({ v00, v10, v01, v11 });

                        if (!smooth)
                        {
                            // Discrete mode: fast-fill constant integer band confirmed by neighbors; refine cells crossing an integer step
                            const float nL = outIterations[static_cast<size_t>(y0) * width + xPrev];
                            const float nR = outIterations[static_cast<size_t>(y0) * width + xNext];
                            const float nT = outIterations[static_cast<size_t>(yPrev) * width + x0];
                            const float nB = outIterations[static_cast<size_t>(yNext) * width + x0];
                            if (minV == maxV && (!edgeRefine || (nL == v00 && nR == v00 && nT == v00 && nB == v00)))
                            {
                                for (int py = y0; py < writeYEnd; ++py)
                                {
                                    float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                                    for (int px = x0; px < writeXEnd; ++px)
                                    {
                                        if (!isCoarseCorner(px, py))
                                        {
                                            rowPtr[px] = v00;
                                        }
                                    }
                                }
                                continue;
                            }
                        }
                        else if (maxV - minV <= smoothThreshold || (!edgeRefine && stride <= 2))
                        {
                            // Interpolate only smooth regions. Corner values alone can miss ridges or an
                            // interior pocket, so verify the midpoint before trusting the coarse samples.
                            const float invDx = 1.0f / static_cast<float>(x1 - x0);
                            bool canInterpolate = true;
                            if (edgeRefine)
                            {
                                const float vm = sampleMidpoint();
                                const float tx = static_cast<float>(xm - x0) * invDx;
                                const float ty = static_cast<float>(ym - y0) * invDy;
                                const float top = v00 + tx * (v10 - v00);
                                const float bot = v01 + tx * (v11 - v01);
                                const float interpolated = top + ty * (bot - top);
                                canInterpolate = vm >= 0.0f && SameBasin(v00, vm, type) &&
                                    std::abs(vm - interpolated) <= 0.25f;
                            }
                            if (canInterpolate)
                            {
                                for (int py = y0; py < writeYEnd; ++py)
                                {
                                    const float ty = static_cast<float>(py - y0) * invDy;
                                    float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                                    for (int px = x0; px < writeXEnd; ++px)
                                    {
                                        if (isCoarseCorner(px, py)) continue;
                                        const float tx = static_cast<float>(px - x0) * invDx;
                                        const float top = v00 + tx * (v10 - v00);
                                        const float bot = v01 + tx * (v11 - v01);
                                        rowPtr[px] = top + ty * (bot - top);
                                    }
                                }
                                continue;
                            }
                        }
                        else if (!edgeRefine)
                        {
                            // Smooth mode with stride >= 3 on a steep filament: sample the single cell midpoint
                            // to preserve filament ridges without evaluating all stride*stride pixels.
                            const float vm = sampleMidpoint();
                            if (vm >= 0.0f && SameBasin(v00, vm, type))
                            {
                                const float invDx = 1.0f / static_cast<float>(x1 - x0);
                                const float avg4 = 0.25f * (v00 + v10 + v01 + v11);
                                for (int py = y0; py < writeYEnd; ++py)
                                {
                                    const float ty = static_cast<float>(py - y0) * invDy;
                                    float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                                    for (int px = x0; px < writeXEnd; ++px)
                                    {
                                        if (isCoarseCorner(px, py)) continue;
                                        if (px == xm && py == ym)
                                        {
                                            rowPtr[px] = vm;
                                            continue;
                                        }
                                        const float tx = static_cast<float>(px - x0) * invDx;
                                        const float top = v00 + tx * (v10 - v00);
                                        const float bot = v01 + tx * (v11 - v01);
                                        const float bilinear = top + ty * (bot - top);
                                        const float centerWeight = std::max(0.0f, 1.0f - 2.0f * std::max(std::abs(tx - 0.5f), std::abs(ty - 0.5f)));
                                        rowPtr[px] = std::max(0.0f, bilinear + centerWeight * (vm - avg4));
                                    }
                                }
                                continue;
                            }
                        }
                    }

                    // Case 3: Resolve boundaries, steep gradients, and failed midpoint checks per pixel.
                    if (edgeRefine)
                    {
                        for (int py = y0; py < writeYEnd; ++py)
                        {
                            float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                            for (int px = x0; px < writeXEnd; ++px)
                            {
                                if (!isCoarseCorner(px, py))
                                {
                                    // A failed midpoint check still produced an exact sample worth keeping.
                                    rowPtr[px] = (hasMidpoint && py == ym && px == xm)
                                        ? midpointValue : evalPixel(px, py);
                                }
                            }
                        }
                    }
                    else
                    {
                        float vm = v00;
                        if (x1 - x0 >= 2 && y1 - y0 >= 2)
                        {
                            vm = sampleMidpoint();
                        }

                        const float invDx = 1.0f / static_cast<float>(x1 - x0);
                        for (int py = y0; py < writeYEnd; ++py)
                        {
                            const float ty = static_cast<float>(py - y0) * invDy;
                            float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                            for (int px = x0; px < writeXEnd; ++px)
                            {
                                if (isCoarseCorner(px, py)) continue;
                                if (px == xm && py == ym && x1 - x0 >= 2 && y1 - y0 >= 2)
                                {
                                    rowPtr[px] = vm;
                                    continue;
                                }

                                const float tx = static_cast<float>(px - x0) * invDx;
                                // Nearest sample among corners and midpoint to avoid blending -1.0f with >= 0.0f
                                if (x1 - x0 >= 2 && y1 - y0 >= 2 &&
                                    std::abs(tx - 0.5f) < 0.25f && std::abs(ty - 0.5f) < 0.25f)
                                {
                                    rowPtr[px] = vm;
                                }
                                else if (ty < 0.5f)
                                {
                                    rowPtr[px] = (tx < 0.5f) ? v00 : v10;
                                }
                                else
                                {
                                    rowPtr[px] = (tx < 0.5f) ? v01 : v11;
                                }
                            }
                        }
                    }
                }
            }
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

    namespace
    {
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
            if (type == FractalType::Mandelbrot && IsInMainCardioidOrBulb(cx, cy))
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
            double checkZr = 0.0;
            double checkZi = 0.0;
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
            }

            if (iter >= maxIter)
            {
                return -1.0f; // Inside set
            }

            if (smooth)
            {
                int smoothIter = iter;
                for (int extra = 0; extra < 3 && (zr2 + zi2) < 1e100; ++extra)
                {
                    if (type == FractalType::Mandelbrot)
                    {
                        zi = 2.0 * zr * zi + cy;
                    }
                    else if (type == FractalType::BurningShip)
                    {
                        zi = -2.0 * std::abs(zr * zi) + cy;
                    }
                    else
                    {
                        zi = -2.0 * zr * zi + cy;
                    }
                    zr = zr2 - zi2 + cx;
                    zr2 = zr * zr;
                    zi2 = zi * zi;
                    smoothIter++;
                }

                const double mod2 = zr2 + zi2;
                const double logZn = std::log(mod2) * 0.5;
                const double nu = smoothIter + 1.0 - std::log(logZn) * invLn2;
                return static_cast<float>(nu > 0.0 ? nu : 0.0);
            }

            return static_cast<float>(iter);
        }

        struct PatchStats
        {
            int totalCount{ 0 };
            int interiorCount{ 0 };
            int exteriorCount{ 0 };
            int distinctBasins{ 0 };
            float interiorRatio{ 0.0f };
            float minSteps{ 0.0f };
            float maxSteps{ 0.0f };
            float stepsRange{ 0.0f };
            float logStepsRange{ 0.0f };
            float stepsStdDev{ 0.0f };
        };

        inline void DecodeIterationValue(float v, FractalType type, int& outBasin, float& outSteps) noexcept
        {
            if (v < 0.0f || std::isnan(v))
            {
                outBasin = -1;
                outSteps = 0.0f;
                return;
            }

            if (type == FractalType::Newton)
            {
                outBasin = std::clamp(static_cast<int>(v / 1000000.0f), 0, 2);
                outSteps = std::fmod(v, 1000000.0f);
            }
            else
            {
                outBasin = 0;
                outSteps = v;
            }
        }

        PatchStats AnalyzePatchStats(
            const std::vector<float>& iterations,
            int width,
            int height,
            FractalType type,
            int x0,
            int y0,
            int x1,
            int y1) noexcept
        {
            PatchStats stats{};
            x0 = std::clamp(x0, 0, width - 1);
            x1 = std::clamp(x1, x0, width - 1);
            y0 = std::clamp(y0, 0, height - 1);
            y1 = std::clamp(y1, y0, height - 1);

            const int stepX = std::max(1, (x1 - x0) / 20);
            const int stepY = std::max(1, (y1 - y0) / 20);

            int basinCounts[3] = { 0, 0, 0 };
            double sumSteps = 0.0;
            double sumStepsSq = 0.0;
            float minSteps = 1e30f;
            float maxSteps = -1e30f;

            for (int py = y0; py <= y1; py += stepY)
            {
                const float* row = &iterations[static_cast<size_t>(py) * width];
                for (int px = x0; px <= x1; px += stepX)
                {
                    stats.totalCount++;
                    int basin = -1;
                    float steps = 0.0f;
                    DecodeIterationValue(row[px], type, basin, steps);
                    if (basin < 0)
                    {
                        stats.interiorCount++;
                    }
                    else
                    {
                        stats.exteriorCount++;
                        basinCounts[basin]++;
                        minSteps = std::min(minSteps, steps);
                        maxSteps = std::max(maxSteps, steps);
                        sumSteps += static_cast<double>(steps);
                        sumStepsSq += static_cast<double>(steps) * static_cast<double>(steps);
                    }
                }
            }

            if (stats.totalCount > 0)
            {
                stats.interiorRatio = static_cast<float>(stats.interiorCount) / static_cast<float>(stats.totalCount);
            }

            if (stats.exteriorCount > 0)
            {
                stats.minSteps = minSteps;
                stats.maxSteps = maxSteps;
                stats.stepsRange = maxSteps - minSteps;
                stats.logStepsRange = 1.6f * std::log((10.0f + std::max(0.0f, maxSteps)) / (10.0f + std::max(0.0f, minSteps)));
                const double mean = sumSteps / static_cast<double>(stats.exteriorCount);
                const double var = std::max(0.0, (sumStepsSq / static_cast<double>(stats.exteriorCount)) - mean * mean);
                stats.stepsStdDev = static_cast<float>(std::sqrt(var));

                const int minBasinCount = std::max(1, stats.exteriorCount / 32);
                for (int b = 0; b < 3; ++b)
                {
                    if (basinCounts[b] >= minBasinCount)
                    {
                        stats.distinctBasins++;
                    }
                }
            }

            return stats;
        }

        // Refines (cx, cy) onto the fractal boundary / high-iteration ridge via 2D boundary bisection.
        void RefineBoundaryCoordinate(
            FractalType type,
            int maxIterations,
            bool smooth,
            double& ioCx,
            double& ioCy,
            double initialStep) noexcept
        {
            const int evalIter = std::clamp(std::max(maxIterations, 600), 200, 4000);
            double step = initialStep;
            double cx = ioCx;
            double cy = ioCy;

            double lastExteriorCx = cx;
            double lastExteriorCy = cy;
            bool hasExterior = false;

            for (int level = 0; level < 18; ++level)
            {
                const double minSafeStep = 1e-14 * std::max(1.0, std::max(std::abs(cx), std::abs(cy)));
                if (step < minSafeStep)
                {
                    break;
                }

                float gridVal[3][3];
                int gridBasin[3][3];
                float gridSteps[3][3];

                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const double gx = cx + static_cast<double>(dx) * step;
                        const double gy = cy + static_cast<double>(dy) * step;
                        const float v = EvalStandardPoint(type, gx, gy, evalIter, smooth);
                        gridVal[dy + 1][dx + 1] = v;
                        DecodeIterationValue(v, type, gridBasin[dy + 1][dx + 1], gridSteps[dy + 1][dx + 1]);
                    }
                }

                // Look for an exterior point adjacent to an interior point (or different Newton basin) with highest iteration count
                bool foundBoundaryPair = false;
                float bestBoundarySteps = -1.0f;
                double nextCx = cx;
                double nextCy = cy;

                float bestPureExteriorSteps = -1.0f;
                double bestPureExtCx = cx;
                double bestPureExtCy = cy;

                for (int iy = 0; iy < 3; ++iy)
                {
                    for (int ix = 0; ix < 3; ++ix)
                    {
                        const int b = gridBasin[iy][ix];
                        if (b < 0) continue; // Skip interior points as primary anchor

                        const float s = gridSteps[iy][ix];
                        const double px = cx + static_cast<double>(ix - 1) * step;
                        const double py = cy + static_cast<double>(iy - 1) * step;

                        if (s > bestPureExteriorSteps)
                        {
                            bestPureExteriorSteps = s;
                            bestPureExtCx = px;
                            bestPureExtCy = py;
                        }

                        // Check 8-neighbors in the 3x3 grid for interior or different root basin
                        for (int ny = std::max(0, iy - 1); ny <= std::min(2, iy + 1); ++ny)
                        {
                            for (int nx = std::max(0, ix - 1); nx <= std::min(2, ix + 1); ++nx)
                            {
                                if (nx == ix && ny == iy) continue;
                                const int nb = gridBasin[ny][nx];
                                const bool isBoundaryEdge = (type == FractalType::Newton) ? (nb != b) : (nb < 0);
                                if (isBoundaryEdge && s > bestBoundarySteps)
                                {
                                    bestBoundarySteps = s;
                                    foundBoundaryPair = true;
                                    const double npx = cx + static_cast<double>(nx - 1) * step;
                                    const double npy = cy + static_cast<double>(ny - 1) * step;
                                    // 2D bisection midpoint between exterior point and interior/other-basin neighbor
                                    nextCx = 0.5 * (px + npx);
                                    nextCy = 0.5 * (py + npy);
                                    lastExteriorCx = px;
                                    lastExteriorCy = py;
                                    hasExterior = true;
                                }
                            }
                        }
                    }
                }

                if (foundBoundaryPair)
                {
                    cx = nextCx;
                    cy = nextCy;
                }
                else if (bestPureExteriorSteps >= 0.0f)
                {
                    cx = bestPureExtCx;
                    cy = bestPureExtCy;
                    lastExteriorCx = bestPureExtCx;
                    lastExteriorCy = bestPureExtCy;
                    hasExterior = true;
                }
                else
                {
                    // All 9 points were interior; revert to last known exterior coordinate
                    if (hasExterior)
                    {
                        cx = lastExteriorCx;
                        cy = lastExteriorCy;
                    }
                    break;
                }

                step *= 0.5;
            }

            ioCx = cx;
            ioCy = cy;
        }
    }

    bool MandelbrotEngine::ShouldRetargetZoom(
        const MandelbrotConfig& config,
        const std::vector<float>& iterations,
        const BigFloat& targetX,
        const BigFloat& targetY)
    {
        if (config.width < 8 || config.height < 8) return false;
        const size_t totalPixels = static_cast<size_t>(config.width) * static_cast<size_t>(config.height);
        if (iterations.size() < totalPixels) return false;

        const double width = static_cast<double>(config.width);
        const double height = static_cast<double>(config.height);
        const double aspect = height / width;
        const double zoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double spanX = 3.0 / zoom;
        const double spanY = spanX * aspect;

        const double dx = (targetX - config.hpCenterX).ToDouble();
        const double dy = (targetY - config.hpCenterY).ToDouble();
        const double normX = 0.5 + (dx / spanX);
        const double normY = 0.5 - (dy / spanY);

        // If the target coordinate lies outside the active viewport, retarget to an in-viewport spot
        if (normX < 0.05 || normX > 0.95 || normY < 0.05 || normY > 0.95)
        {
            return true;
        }

        const int halfW = std::max(3, config.width / 8);
        const int halfH = std::max(3, config.height / 8);
        const int coreW = std::max(2, config.width / 16);
        const int coreH = std::max(2, config.height / 16);

        const int cx = std::clamp(static_cast<int>(std::round(normX * (width - 1.0))), halfW, config.width - 1 - halfW);
        const int cy = std::clamp(static_cast<int>(std::round(normY * (height - 1.0))), halfH, config.height - 1 - halfH);

        const PatchStats outer = AnalyzePatchStats(
            iterations, config.width, config.height, config.fractalType,
            cx - halfW, cy - halfH, cx + halfW, cy + halfH);

        const PatchStats inner = AnalyzePatchStats(
            iterations, config.width, config.height, config.fractalType,
            std::max(0, cx - coreW), std::max(0, cy - coreH),
            std::min(config.width - 1, cx + coreW), std::min(config.height - 1, cy + coreH));

        // 1. Check if zooming into a solid interior bulb (black screen)
        if (outer.interiorRatio > 0.85f || (inner.interiorRatio > 0.94f && outer.interiorRatio > 0.60f))
        {
            return true;
        }

        // 2. Check if zooming into a uniform exterior / single-basin area (solid exterior color)
        if (config.fractalType == FractalType::Newton)
        {
            if (outer.distinctBasins <= 1 && (outer.stepsRange < 2.5f || outer.stepsStdDev < 0.45f))
            {
                return true;
            }
            if (inner.distinctBasins <= 1 && inner.stepsRange < 1.2f && outer.stepsRange < 4.0f)
            {
                return true;
            }
        }
        else
        {
            if (outer.interiorRatio < 0.02f &&
                (outer.stepsRange < 2.2f || outer.logStepsRange < 0.14f || outer.stepsStdDev < 0.40f))
            {
                return true;
            }
            if (inner.interiorRatio < 0.01f && (inner.stepsRange < 0.8f || inner.logStepsRange < 0.05f) &&
                outer.interiorRatio < 0.05f && (outer.stepsRange < 3.5f || outer.logStepsRange < 0.22f))
            {
                return true;
            }
        }

        return false;
    }

    bool MandelbrotEngine::PickInterestingTarget(
        const MandelbrotConfig& config,
        const std::vector<float>& iterations,
        BigFloat& outTargetX,
        BigFloat& outTargetY,
        uint64_t randomSeed)
    {
        if (config.width <= 0 || config.height <= 0) return false;

        std::mt19937_64 rng;
        if (randomSeed != 0)
        {
            rng.seed(randomSeed);
        }
        else
        {
            std::random_device rd;
            const auto nowTicks = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            rng.seed((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ nowTicks);
        }

        const int w = config.width;
        const int h = config.height;
        const double width = static_cast<double>(w);
        const double height = static_cast<double>(h);
        const double aspect = height / width;
        const double zoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double spanX = 3.0 / zoom;
        const double spanY = spanX * aspect;

        const size_t totalPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        if (w >= 8 && h >= 8 && iterations.size() >= totalPixels)
        {
            const int halfW = std::max(3, w / 8);
            const int halfH = std::max(3, h / 8);
            const int coreW = std::max(2, w / 16);
            const int coreH = std::max(2, h / 16);

            const int minCx = std::max(halfW, static_cast<int>(w * 0.16));
            const int maxCx = std::min(w - 1 - halfW, static_cast<int>(w * 0.84));
            const int minCy = std::max(halfH, static_cast<int>(h * 0.16));
            const int maxCy = std::min(h - 1 - halfH, static_cast<int>(h * 0.84));

            struct CandidatePatch
            {
                int cx;
                int cy;
                double score;
            };
            std::vector<CandidatePatch> candidates;

            const int gridStepsX = 12;
            const int gridStepsY = 9;
            const int strideX = std::max(1, (maxCx - minCx) / gridStepsX);
            const int strideY = std::max(1, (maxCy - minCy) / gridStepsY);

            for (int cy = minCy; cy <= maxCy; cy += strideY)
            {
                for (int cx = minCx; cx <= maxCx; cx += strideX)
                {
                    const PatchStats outer = AnalyzePatchStats(
                        iterations, w, h, config.fractalType,
                        cx - halfW, cy - halfH, cx + halfW, cy + halfH);

                    const PatchStats inner = AnalyzePatchStats(
                        iterations, w, h, config.fractalType,
                        std::max(0, cx - coreW), std::max(0, cy - coreH),
                        std::min(w - 1, cx + coreW), std::min(h - 1, cy + coreH));

                    // Reject patches that are too close to solid color thresholds (hysteresis margin)
                    if (outer.interiorRatio > 0.68f || inner.interiorRatio > 0.82f)
                    {
                        continue;
                    }

                    double score = 0.0;
                    if (config.fractalType == FractalType::Newton)
                    {
                        if (outer.distinctBasins <= 1 && (outer.stepsRange < 4.5f || outer.stepsStdDev < 0.85f))
                        {
                            continue;
                        }
                        if (inner.distinctBasins <= 1 && inner.stepsRange < 2.2f)
                        {
                            continue;
                        }

                        score += static_cast<double>(outer.distinctBasins) * 25.0;
                        score += static_cast<double>(inner.distinctBasins) * 20.0;
                        score += std::min(60.0, static_cast<double>(outer.stepsStdDev) * 4.0);
                        score += std::min(40.0, static_cast<double>(outer.maxSteps) * 0.5);
                    }
                    else
                    {
                        const bool hasBoundary = (outer.interiorRatio >= 0.03f && outer.stepsRange >= 3.0f);
                        const bool hasStrongFilaments = (outer.stepsRange >= 5.0f && outer.logStepsRange >= 0.28f &&
                            outer.stepsStdDev >= 0.90f && inner.stepsRange >= 2.0f && inner.logStepsRange >= 0.10f);
                        if (!hasBoundary && !hasStrongFilaments)
                        {
                            continue;
                        }

                        // Reward patches with an active interior/exterior shoreline (ideal ratio ~10%..40% interior)
                        if (outer.interiorRatio >= 0.03f && outer.interiorRatio <= 0.55f)
                        {
                            const double distFromIdeal = std::abs(static_cast<double>(outer.interiorRatio) - 0.22);
                            score += 45.0 * std::max(0.2, 1.0 - distFromIdeal * 2.5);
                        }
                        if (inner.interiorRatio >= 0.02f && inner.interiorRatio <= 0.60f)
                        {
                            score += 30.0;
                        }

                        // Reward rich iteration variance and deep filament iteration counts
                        score += std::min(60.0, static_cast<double>(outer.stepsStdDev) * 2.5);
                        score += std::min(40.0, static_cast<double>(inner.stepsStdDev) * 2.5);
                        const double iterFrac = static_cast<double>(outer.maxSteps) / static_cast<double>(std::max(50, config.maxIterations));
                        score += std::min(35.0, iterFrac * 35.0);
                    }

                    if (score > 0.0)
                    {
                        candidates.push_back({ cx, cy, score });
                    }
                }
            }

            if (!candidates.empty())
            {
                std::sort(candidates.begin(), candidates.end(),
                    [](const CandidatePatch& a, const CandidatePatch& b) { return a.score > b.score; });

                const double topScore = candidates.front().score;
                std::vector<CandidatePatch> topPool;
                std::vector<double> weights;
                const size_t maxPoolSize = std::min<size_t>(candidates.size(), 16);
                for (size_t i = 0; i < maxPoolSize; ++i)
                {
                    if (candidates[i].score >= topScore * 0.55)
                    {
                        topPool.push_back(candidates[i]);
                        weights.push_back(candidates[i].score * candidates[i].score);
                    }
                }

                std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
                const CandidatePatch chosen = topPool[dist(rng)];

                // Pinpoint the highest-interest boundary/filament pixel pair inside the chosen patch
                const int searchX0 = std::max(1, chosen.cx - coreW);
                const int searchX1 = std::min(w - 2, chosen.cx + coreW);
                const int searchY0 = std::max(1, chosen.cy - coreH);
                const int searchY1 = std::min(h - 2, chosen.cy + coreH);

                double bestPixelScore = -1.0;
                int bestPx = chosen.cx;
                int bestPy = chosen.cy;
                int bestNx = chosen.cx;
                int bestNy = chosen.cy;

                std::uniform_real_distribution<double> jitterDist(0.95, 1.05);

                for (int py = searchY0; py <= searchY1; ++py)
                {
                    for (int px = searchX0; px <= searchX1; ++px)
                    {
                        int basin = -1;
                        float steps = 0.0f;
                        DecodeIterationValue(iterations[static_cast<size_t>(py) * w + px], config.fractalType, basin, steps);
                        if (basin < 0) continue; // Anchor on exterior side of boundary

                        bool bordersBoundary = false;
                        float maxNeighborDiff = 0.0f;
                        int boundaryNx = px;
                        int boundaryNy = py;

                        for (int ny = py - 1; ny <= py + 1; ++ny)
                        {
                            for (int nx = px - 1; nx <= px + 1; ++nx)
                            {
                                if (nx == px && ny == py) continue;
                                int nBasin = -1;
                                float nSteps = 0.0f;
                                DecodeIterationValue(iterations[static_cast<size_t>(ny) * w + nx], config.fractalType, nBasin, nSteps);

                                const bool isEdge = (config.fractalType == FractalType::Newton)
                                    ? (nBasin >= 0 && nBasin != basin)
                                    : (nBasin < 0);

                                if (isEdge)
                                {
                                    bordersBoundary = true;
                                    boundaryNx = nx;
                                    boundaryNy = ny;
                                }
                                else if (nBasin >= 0)
                                {
                                    const float diff = std::abs(steps - nSteps);
                                    if (diff > maxNeighborDiff)
                                    {
                                        maxNeighborDiff = diff;
                                        if (!bordersBoundary)
                                        {
                                            boundaryNx = nx;
                                            boundaryNy = ny;
                                        }
                                    }
                                }
                            }
                        }

                        double pxScore = (static_cast<double>(steps) + static_cast<double>(maxNeighborDiff) * 3.0) * jitterDist(rng);
                        if (bordersBoundary)
                        {
                            pxScore += 10000.0;
                        }

                        if (pxScore > bestPixelScore)
                        {
                            bestPixelScore = pxScore;
                            bestPx = px;
                            bestPy = py;
                            bestNx = boundaryNx;
                            bestNy = boundaryNy;
                        }
                    }
                }

                // Sub-pixel initial placement along the boundary edge (biased slightly toward exterior pixel)
                const double subPx = 0.6 * static_cast<double>(bestPx) + 0.4 * static_cast<double>(bestNx);
                const double subPy = 0.6 * static_cast<double>(bestPy) + 0.4 * static_cast<double>(bestNy);
                double offsetX = ((subPx + 0.5) / width - 0.5) * spanX;
                double offsetY = (0.5 - (subPy + 0.5) / height) * spanY;

                if (config.zoom < DeepZoomThreshold)
                {
                    double refinedCx = config.centerX + offsetX;
                    double refinedCy = config.centerY + offsetY;
                    const double pixelStep = (spanX / width) * 0.75;
                    RefineBoundaryCoordinate(
                        config.fractalType,
                        config.maxIterations,
                        config.smooth,
                        refinedCx,
                        refinedCy,
                        pixelStep);
                    offsetX = refinedCx - config.centerX;
                    offsetY = refinedCy - config.centerY;
                }

                outTargetX = config.hpCenterX + BigFloat(offsetX);
                outTargetY = config.hpCenterY + BigFloat(offsetY);
                return true;
            }
        }

        // Fallback when the entire current viewport is already a solid color:
        // Pick an interesting landmark from the fractal's bookmarks, add a small boundary perturbation, and refine onto the boundary.
        const auto& bookmarks = GetBookmarks(config.fractalType);
        if (!bookmarks.empty())
        {
            const size_t startIdx = (bookmarks.size() > 1) ? 1 : 0;
            std::uniform_int_distribution<size_t> bmDist(startIdx, bookmarks.size() - 1);
            const auto& bm = bookmarks[bmDist(rng)];

            double targetCx = bm.centerX;
            double targetCy = bm.centerY;
            const double bmSpan = 3.0 / std::max(1.0, bm.zoom);
            std::uniform_real_distribution<double> offsetDist(-0.15 * bmSpan, 0.15 * bmSpan);
            targetCx += offsetDist(rng);
            targetCy += offsetDist(rng);

            RefineBoundaryCoordinate(
                config.fractalType,
                std::max(config.maxIterations, bm.maxIterations),
                config.smooth,
                targetCx,
                targetCy,
                bmSpan * 0.05);

            outTargetX = BigFloat(targetCx);
            outTargetY = BigFloat(targetCy);
            return true;
        }

        outTargetX = config.hpCenterX;
        outTargetY = config.hpCenterY;
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

        auto evalStandardPixel = [&](int px, int py) -> float
        {
            const double cy = config.centerY + (halfH - (static_cast<double>(py) + 0.5)) * dy;
            const double cx = config.centerX + ((static_cast<double>(px) + 0.5) - halfW) * dx;
            return EvalStandardPoint(type, cx, cy, maxIter, smooth);
        };

        ExecuteRenderPass(
            width,
            height,
            config.renderStride,
            config.edgeRefine,
            smooth,
            type,
            outIterations,
            cancelToken,
            evalStandardPixel);

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
        // plus ~96 guard bits (3 limbs), rounded up to even limb counts and clamped to [4, BigFloat::N].
        const int rawLimbs = static_cast<int>(std::ceil((std::log2(clampedZoom) + 96.0) / 32.0));
        const int activeLimbs = std::clamp((rawLimbs + 1) & ~1, 4, BigFloat::N);

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

            auto evalNewtonPixel = [&](int px, int py) -> float
            {
                const double dcy = static_cast<double>(refPy - py) * stepY;
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

                        const double sx = rx + zr;
                        const double sy = ry + zi;
                        const double numX = dx * sx - dy * sy;
                        const double numY = dx * sy + dy * sx;

                        const double pxTerm = rx * zr - ry * zi;
                        const double pyTerm = rx * zi + ry * zr;
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
                    return -1.0f;
                }

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
                return rootIndex * 1000000.0f + smoothVal;
            };

            ExecuteRenderPass(
                width,
                height,
                config.renderStride,
                config.edgeRefine,
                smooth,
                type,
                outIterations,
                cancelToken,
                evalNewtonPixel);

            const auto endTime = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
            return elapsed.count();
        }

        // Escape-time fractals (Mandelbrot, Burning Ship, Tricorn):
        // High-precision reference orbit + hardware double perturbation with Zhuoran's dynamic rebasing,
        // cross-frame reference orbit caching, and fast sample-grid reference search when the center escapes early.
        constexpr double bailoutRadius2 = 16.0;
        const double centerPosX = (static_cast<double>(width) - 1.0) * 0.5;
        const double centerPosY = (static_cast<double>(height) - 1.0) * 0.5;

        auto stepBigFloatOrbit = [&](const BigFloat& cRefX,
                                     const BigFloat& cRefY,
                                     int startM,
                                     int targetMaxIter,
                                     BigFloat& ioZx,
                                     BigFloat& ioZy,
                                     std::vector<ComplexOrbitPt>& outOrbit,
                                     bool& outEscaped) -> int
        {
            if (static_cast<int>(outOrbit.size()) <= targetMaxIter)
            {
                outOrbit.resize(static_cast<size_t>(targetMaxIter) + 1);
            }

            int len = startM;
            outEscaped = false;

            for (int m = startM; m <= targetMaxIter; ++m)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    return len;
                }

                const double rx = ioZx.ToDouble();
                const double ry = ioZy.ToDouble();
                outOrbit[m] = { rx, ry };
                len = m;

                if (rx * rx + ry * ry > bailoutRadius2)
                {
                    outEscaped = true;
                    break;
                }
                if (m == targetMaxIter)
                {
                    break;
                }

                const BigFloat Zx2 = ioZx.Sqr(activeLimbs);
                const BigFloat Zy2 = ioZy.Sqr(activeLimbs);
                const BigFloat Zxy = ioZx.Mul(ioZy, activeLimbs);
                const BigFloat twoZxy = Zxy + Zxy;

                if (type == FractalType::Mandelbrot)
                {
                    ioZy = twoZxy + cRefY;
                    ioZx = Zx2 - Zy2 + cRefX;
                }
                else if (type == FractalType::BurningShip)
                {
                    ioZy = -twoZxy.Abs() + cRefY;
                    ioZx = Zx2 - Zy2 + cRefX;
                }
                else // FractalType::Tricorn
                {
                    ioZy = -twoZxy + cRefY;
                    ioZx = Zx2 - Zy2 + cRefX;
                }
            }
            return len;
        };

        double refPosX = centerPosX;
        double refPosY = centerPosY;
        int bestRefLen = 0;
        bool reusedUnescapedCache = false;

        // Check if we can reuse or incrementally extend the cached reference orbit from the previous frame
        if (m_hasCachedOrbit &&
            m_cachedType == type &&
            m_cachedActiveLimbs >= activeLimbs &&
            !m_cachedOrbit.empty())
        {
            const double diffX = (m_cachedRefX - centerX).ToDouble();
            const double diffY = (m_cachedRefY - centerY).ToDouble();

            if (!m_cachedEscaped &&
                std::abs(diffX) <= spanX * 0.40 &&
                std::abs(diffY) <= spanY * 0.40)
            {
                if (maxIter > m_cachedMaxIter)
                {
                    const int targetExtendIter = maxIter + 32;
                    bool escapedNow = false;
                    m_cachedRefLen = stepBigFloatOrbit(
                        m_cachedRefX,
                        m_cachedRefY,
                        m_cachedMaxIter,
                        targetExtendIter,
                        m_cachedEndZx,
                        m_cachedEndZy,
                        m_cachedOrbit,
                        escapedNow);
                    m_cachedMaxIter = targetExtendIter;
                    m_cachedEscaped = escapedNow;
                }

                if (!m_cachedEscaped || m_cachedRefLen >= maxIter)
                {
                    refPosX = centerPosX + diffX / stepX;
                    refPosY = centerPosY - diffY / stepY;
                    bestRefLen = maxIter;
                    reusedUnescapedCache = true;
                }
            }
            else if (m_cachedEscaped &&
                     std::abs(diffX) <= spanX * 0.35 &&
                     std::abs(diffY) <= spanY * 0.35 &&
                     m_cachedSearchZoom > 0.0 &&
                     clampedZoom >= m_cachedSearchZoom * 0.65 &&
                     clampedZoom <= m_cachedSearchZoom * 1.50)
            {
                // Reuse the best reference orbit already found for this exterior neighborhood across consecutive
                // zoom frames so we don't re-run the 8x8 search and 1024-bit BigFloat orbits on every frame.
                refPosX = centerPosX + diffX / stepX;
                refPosY = centerPosY - diffY / stepY;
                bestRefLen = std::min(m_cachedRefLen, maxIter);
                reusedUnescapedCache = true;
            }
        }

        if (!reusedUnescapedCache)
        {
            BigFloat bestRefCx = centerX;
            BigFloat bestRefCy = centerY;
            BigFloat bestEndZx(0.0);
            BigFloat bestEndZy(0.0);
            bool bestEscaped = false;

            const double cachedDiffX = m_hasCachedOrbit ? (m_cachedRefX - centerX).ToDouble() : 1e300;
            const double cachedDiffY = m_hasCachedOrbit ? (m_cachedRefY - centerY).ToDouble() : 1e300;

            // Reuse the cached orbit as our starting reference if it is at the current center or still well within the central viewport
            if (m_hasCachedOrbit &&
                m_cachedType == type &&
                m_cachedActiveLimbs >= activeLimbs &&
                !m_cachedOrbit.empty() &&
                ((m_cachedRefX == centerX && m_cachedRefY == centerY) ||
                 (std::abs(cachedDiffX) <= spanX * 0.30 && std::abs(cachedDiffY) <= spanY * 0.30)))
            {
                if (!m_cachedEscaped && maxIter > m_cachedMaxIter)
                {
                    const int targetExtendIter = maxIter + 32;
                    m_cachedRefLen = stepBigFloatOrbit(
                        m_cachedRefX,
                        m_cachedRefY,
                        m_cachedMaxIter,
                        targetExtendIter,
                        m_cachedEndZx,
                        m_cachedEndZy,
                        m_cachedOrbit,
                        m_cachedEscaped);
                    m_cachedMaxIter = targetExtendIter;
                }
                bestRefCx = m_cachedRefX;
                bestRefCy = m_cachedRefY;
                refPosX = centerPosX + cachedDiffX / stepX;
                refPosY = centerPosY - cachedDiffY / stepY;
                bestRefLen = std::min(m_cachedRefLen, maxIter);
                bestEndZx = m_cachedEndZx;
                bestEndZy = m_cachedEndZy;
                bestEscaped = (bestRefLen < maxIter) && m_cachedEscaped;
            }
            else
            {
                bestEndZx.SetZero();
                bestEndZy.SetZero();
                bestRefLen = stepBigFloatOrbit(
                    centerX,
                    centerY,
                    0,
                    maxIter,
                    bestEndZx,
                    bestEndZy,
                    m_cachedOrbit,
                    bestEscaped);
            }

            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                return 0.0;
            }

            // If the center reference orbit escaped before maxIter, probe an 8x8 sample grid across the viewport
            // (up to 2 upgrades) to find a reference pixel genuinely inside the |z| < 2 disk (mod2 < 4.0) at m == bestRefLen.
            if (bestRefLen < maxIter && width >= 4 && height >= 4)
            {
                std::vector<ComplexOrbitPt> candidateOrbit(static_cast<size_t>(maxIter) + 1);
                constexpr int gridN = 8;

                for (int attempt = 0; attempt < 2 && bestRefLen < maxIter; ++attempt)
                {
                    int nextCandPx = -1;
                    int nextCandPy = -1;
                    double minMod2AtRefEnd = 4.0;
                    const ComplexOrbitPt* orbitPtr = m_cachedOrbit.data();

                    for (int gy = 0; gy < gridN; ++gy)
                    {
                        const int spy = ((2 * gy + 1) * height) / (2 * gridN);
                        const double dcy = (refPosY - static_cast<double>(spy)) * stepY;

                        for (int gx = 0; gx < gridN; ++gx)
                        {
                            const int spx = ((2 * gx + 1) * width) / (2 * gridN);
                            const double dcx = (static_cast<double>(spx) - refPosX) * stepX;
                            if (dcx == 0.0 && dcy == 0.0) continue;

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
                                else
                                {
                                    const double nextDx = sx * dx - sy * dy + dcx;
                                    const double nextDy = -(sx * dy + sy * dx) + dcy;
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

                    const double candOffsetX = (static_cast<double>(nextCandPx) - centerPosX) * stepX;
                    const double candOffsetY = (centerPosY - static_cast<double>(nextCandPy)) * stepY;
                    const BigFloat candCx = centerX + BigFloat(candOffsetX);
                    const BigFloat candCy = centerY + BigFloat(candOffsetY);
                    BigFloat candEndZx(0.0);
                    BigFloat candEndZy(0.0);
                    bool candEscaped = false;

                    const int candLen = stepBigFloatOrbit(
                        candCx,
                        candCy,
                        0,
                        maxIter,
                        candEndZx,
                        candEndZy,
                        candidateOrbit,
                        candEscaped);

                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        return 0.0;
                    }

                    if (candLen > bestRefLen)
                    {
                        m_cachedOrbit.swap(candidateOrbit);
                        bestRefLen = candLen;
                        refPosX = static_cast<double>(nextCandPx);
                        refPosY = static_cast<double>(nextCandPy);
                        bestRefCx = candCx;
                        bestRefCy = candCy;
                        bestEndZx = candEndZx;
                        bestEndZy = candEndZy;
                        bestEscaped = candEscaped;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            m_hasCachedOrbit = true;
            m_cachedType = type;
            m_cachedRefX = bestRefCx;
            m_cachedRefY = bestRefCy;
            m_cachedEndZx = bestEndZx;
            m_cachedEndZy = bestEndZy;
            m_cachedActiveLimbs = activeLimbs;
            m_cachedRefLen = bestRefLen;
            m_cachedMaxIter = std::max(maxIter, m_cachedMaxIter);
            m_cachedSearchZoom = clampedZoom;
            m_cachedEscaped = bestEscaped;
        }

        const int refLen = bestRefLen;
        const ComplexOrbitPt* orbitPtr = m_cachedOrbit.data();
        const double refCxDouble = m_cachedRefX.ToDouble();
        const double refCyDouble = m_cachedRefY.ToDouble();

        auto evalDeepPixel = [&](int px, int py) -> float
        {
            const double dcy = (refPosY - static_cast<double>(py)) * stepY;
            const double dcx = (static_cast<double>(px) - refPosX) * stepX;
            double dx = 0.0;
            double dy = 0.0;
            int m = 0;
            int iter = 0;
            double escapedZx = 0.0;
            double escapedZy = 0.0;
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
                        escapedZx = zx;
                        escapedZy = zy;
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

                    const double sx = rx + zx;
                    const double sy = ry + zy;
                    const double nextDx = sx * dx - sy * dy + dcx;
                    const double nextDy = sx * dy + sy * dx + dcy;
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
                        escapedZx = zx;
                        escapedZy = zy;
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

                    const double sx = rx + zx;
                    const double sy = ry + zy;
                    const double daX = DiffAbs(rx, dx);
                    const double daY = DiffAbs(ry, dy);
                    const double nextDx = sx * dx - sy * dy + dcx;
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
                        escapedZx = zx;
                        escapedZy = zy;
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

                    const double sx = rx + zx;
                    const double sy = ry + zy;
                    const double nextDx = sx * dx - sy * dy + dcx;
                    const double nextDy = -(sx * dy + sy * dx) + dcy;
                    dx = nextDx;
                    dy = nextDy;
                    m++;
                    iter++;
                }
                break;
            }

            if (iter >= maxIter)
            {
                return -1.0f;
            }

            if (smooth)
            {
                // Perform up to 3 extra post-bailout iterations so |z| is large enough (|z| >= ~65536)
                // that the +c term in z_{n+1} = z_n^2 + c has zero float-precision effect (< 1e-9) across
                // integer iteration boundaries, matching standard Compute() with zero contour seams.
                const double cxApprox = refCxDouble + dcx;
                const double cyApprox = refCyDouble + dcy;
                double zr = escapedZx;
                double zi = escapedZy;
                double zr2 = zr * zr;
                double zi2 = zi * zi;
                int smoothIter = iter;

                for (int extra = 0; extra < 3 && (zr2 + zi2) < 1e100; ++extra)
                {
                    if (type == FractalType::Mandelbrot)
                    {
                        zi = 2.0 * zr * zi + cyApprox;
                    }
                    else if (type == FractalType::BurningShip)
                    {
                        zi = -2.0 * std::abs(zr * zi) + cyApprox;
                    }
                    else
                    {
                        zi = -2.0 * zr * zi + cyApprox;
                    }
                    zr = zr2 - zi2 + cxApprox;
                    zr2 = zr * zr;
                    zi2 = zi * zi;
                    smoothIter++;
                }

                const double mod2 = zr2 + zi2;
                const double logZn = std::log(mod2) * 0.5;
                const double nu = smoothIter + 1.0 - std::log(logZn) * invLn2;
                return static_cast<float>(nu > 0.0 ? nu : 0.0);
            }

            return static_cast<float>(iter);
        };

        ExecuteRenderPass(
            width,
            height,
            config.renderStride,
            config.edgeRefine,
            smooth,
            type,
            outIterations,
            cancelToken,
            evalDeepPixel);

        const auto endTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }
}
