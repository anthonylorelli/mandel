#include "pch.h"
#include "AutoExplore.h"
#include "FractalFormulas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace Mandelbrot
{
    namespace
    {
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

                int gridBasin[3][3];
                float gridSteps[3][3];

                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const double gx = cx + static_cast<double>(dx) * step;
                        const double gy = cy + static_cast<double>(dy) * step;
                        const float v = detail::EvalStandardPoint(type, gx, gy, evalIter, smooth);
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

    bool ShouldRetargetZoom(
        const FractalConfig& config,
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

    bool PickInterestingTarget(
        const FractalConfig& config,
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
}
