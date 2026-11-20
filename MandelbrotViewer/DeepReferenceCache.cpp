#include "pch.h"
#include "DeepReferenceCache.h"
#include "FractalFormulas.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot
{
    void DeepReferenceCache::SetMovieHint(
        double maxZoom,
        const BigFloat* anchorX,
        const BigFloat* anchorY) noexcept
    {
        if (std::isfinite(maxZoom) && maxZoom >= 16.0)
        {
            const double clampedMax = std::clamp(maxZoom, MinZoom, MaxZoom);
            const int rawLimbs = static_cast<int>(std::ceil((std::log2(clampedMax) + 96.0) / 32.0));
            m_minActiveLimbs = std::clamp((rawLimbs + 1) & ~1, 4, BigFloat::N);
        }
        else
        {
            m_minActiveLimbs = 0;
        }

        if (anchorX && anchorY)
        {
            m_hasPreferredAnchor = true;
            m_anchorX = *anchorX;
            m_anchorY = *anchorY;
        }
        else
        {
            m_hasPreferredAnchor = false;
            m_anchorX.SetZero();
            m_anchorY.SetZero();
        }
    }

    void DeepReferenceCache::ClearMovieHint() noexcept
    {
        m_minActiveLimbs = 0;
        m_hasPreferredAnchor = false;
        m_anchorX.SetZero();
        m_anchorY.SetZero();
        m_preparedStepX = 0.0;
        m_preparedSpanX = 0.0;
        m_preparedSpanY = 0.0;
    }

    void DeepReferenceCache::Invalidate() noexcept
    {
        m_hasCachedOrbit = false;
        m_cachedMandelbrotBla = {};
        m_cachedBlaVersion = 0;
        m_cachedBlaRefLen = 0;
        m_cachedBlaCMax = 0.0;
        m_preparedStepX = 0.0;
        m_preparedSpanX = 0.0;
        m_preparedSpanY = 0.0;
    }

    void DeepReferenceCache::Prepare(
        const FractalConfig& config,
        ExecutionMode executionMode,
        const std::atomic<bool>* cancelToken)
    {
        const bool gpuEarlyPerturb = (executionMode == ExecutionMode::Gpu) &&
            (config.fractalType == FractalType::Mandelbrot ||
             config.fractalType == FractalType::Tricorn ||
             config.fractalType == FractalType::BurningShip ||
             config.fractalType == FractalType::Buffalo);
        const double minZoom = gpuEarlyPerturb ? 16.0 : DeepZoomThreshold;
        if (config.width <= 0 || config.height <= 0 || config.juliaSet ||
            config.zoom < minZoom || config.fractalType == FractalType::Newton)
        {
            return;
        }

        BigFloat hpX;
        BigFloat hpY;
        config.ResolveHpCenter(hpX, hpY);

        DeepReferenceState unusedState{};
        if (Ensure(config, hpX, hpY, executionMode, unusedState, cancelToken))
        {
            const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
            const double spanX = 3.0 / clampedZoom;
            const double aspect = static_cast<double>(config.height) / static_cast<double>(config.width);
            m_preparedSpanX = spanX;
            m_preparedSpanY = spanX * aspect;
            m_preparedStepX = spanX / static_cast<double>(config.width);
        }
    }

    bool DeepReferenceCache::Ensure(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        ExecutionMode executionMode,
        DeepReferenceState& outState,
        const std::atomic<bool>* cancelToken)
    {
        const int width = config.width;
        const int height = config.height;
        const int maxIter = config.maxIterations;
        const FractalType type = config.fractalType;

        const double aspect = static_cast<double>(height) / static_cast<double>(width);
        const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double spanX = 3.0 / clampedZoom;
        const double spanY = spanX * aspect;
        const double stepX = spanX / static_cast<double>(width);
        const double stepY = spanY / static_cast<double>(height);

        const int rawLimbs = static_cast<int>(std::ceil((std::log2(clampedZoom) + 96.0) / 32.0));
        const int activeLimbs = std::clamp(std::max((rawLimbs + 1) & ~1, m_minActiveLimbs), 4, BigFloat::N);

        constexpr double bailoutRadius2 = 16.0;
        const double centerPosX = (static_cast<double>(width) - 1.0) * 0.5;
        const double centerPosY = (static_cast<double>(height) - 1.0) * 0.5;

        auto stepBigFloatOrbit = [&](const BigFloat& cRefX,
                                     const BigFloat& cRefY,
                                     int startM,
                                     int targetMaxIter,
                                     BigFloat& ioZx,
                                     BigFloat& ioZy,
                                     std::vector<ComplexOrbitPoint>& outOrbit,
                                     bool& outEscaped) -> int
        {
            if (static_cast<int>(outOrbit.size()) <= targetMaxIter)
            {
                outOrbit.resize(static_cast<size_t>(targetMaxIter) + 1);
            }

            int len = startM;
            outEscaped = false;
            BigFloat prevZx(0.0);
            BigFloat prevZy(0.0);
            const BigFloat phoenixC(0.5667);
            const BigFloat phoenixHalf(0.5);
            if (type == FractalType::Phoenix && startM == 0)
            {
                ioZx = cRefY;
                ioZy = cRefX;
            }

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
                const BigFloat twoZxy = Zxy.Add(Zxy, activeLimbs);

                if (type == FractalType::Mandelbrot)
                {
                    ioZy = twoZxy.Add(cRefY, activeLimbs);
                    ioZx = Zx2.Sub(Zy2, activeLimbs).Add(cRefX, activeLimbs);
                }
                else if (type == FractalType::BurningShip)
                {
                    ioZy = (-twoZxy.Abs()).Add(cRefY, activeLimbs);
                    ioZx = Zx2.Sub(Zy2, activeLimbs).Add(cRefX, activeLimbs);
                }
                else if (type == FractalType::Tricorn)
                {
                    ioZy = (-twoZxy).Add(cRefY, activeLimbs);
                    ioZx = Zx2.Sub(Zy2, activeLimbs).Add(cRefX, activeLimbs);
                }
                else if (type == FractalType::Celtic)
                {
                    ioZy = twoZxy.Add(cRefY, activeLimbs);
                    ioZx = Zx2.Sub(Zy2, activeLimbs).Abs().Add(cRefX, activeLimbs);
                }
                else if (type == FractalType::Buffalo)
                {
                    ioZy = (-twoZxy.Abs()).Add(cRefY, activeLimbs);
                    ioZx = Zx2.Sub(Zy2, activeLimbs).Abs().Add(cRefX, activeLimbs);
                }
                else if (type == FractalType::CubicMultibrot)
                {
                    const BigFloat threeZx2 = Zx2.Add(Zx2, activeLimbs).Add(Zx2, activeLimbs);
                    const BigFloat threeZy2 = Zy2.Add(Zy2, activeLimbs).Add(Zy2, activeLimbs);
                    const BigFloat nextZx = ioZx.Mul(Zx2.Sub(threeZy2, activeLimbs), activeLimbs).Add(cRefX, activeLimbs);
                    const BigFloat nextZy = ioZy.Mul(threeZx2.Sub(Zy2, activeLimbs), activeLimbs).Add(cRefY, activeLimbs);
                    ioZx = nextZx;
                    ioZy = nextZy;
                }
                else // FractalType::Phoenix
                {
                    const BigFloat nextZx = Zx2.Sub(Zy2, activeLimbs).Add(phoenixC, activeLimbs).Sub(prevZx.Mul(phoenixHalf, activeLimbs), activeLimbs);
                    const BigFloat nextZy = twoZxy.Sub(prevZy.Mul(phoenixHalf, activeLimbs), activeLimbs);
                    prevZx = ioZx;
                    prevZy = ioZy;
                    ioZx = nextZx;
                    ioZy = nextZy;
                }
            }
            return len;
        };

        auto extendTarget = [](int iterBudget) noexcept -> int
        {
            return std::max(iterBudget + 256, iterBudget + iterBudget / 8);
        };

        double refPosX = centerPosX;
        double refPosY = centerPosY;
        int bestRefLen = 0;
        bool reusedUnescapedCache = false;
        // Include full-frame horizontal/vertical span and 8x8 search candidate margin (+/- 0.4375 * span)
        // so reference orbits prepared for a full frame/keyframe are reused across all bands and side strips.
        const bool samePreparedGrid =
            (m_preparedStepX > 0.0 && std::abs(stepX - m_preparedStepX) <= m_preparedStepX * 1e-4);
        const double maxReuseDiffX = samePreparedGrid ? std::max(spanX * 0.48, m_preparedSpanX) : (spanX * 0.48);
        const double maxReuseDiffY = samePreparedGrid
            ? std::max({ spanY * 0.96, spanX * 0.96, m_preparedSpanY })
            : (std::max(spanY * 2.0, spanX * 2.0) * 0.48);

        // Check if we can reuse or incrementally extend the cached reference orbit from the previous frame/band
        if (m_hasCachedOrbit &&
            m_cachedType == type &&
            m_cachedActiveLimbs >= activeLimbs &&
            !m_cachedOrbit.empty())
        {
            const double diffX = (m_cachedRefX - centerX).ToDouble();
            const double diffY = (m_cachedRefY - centerY).ToDouble();

            if (!m_cachedEscaped &&
                std::abs(diffX) <= maxReuseDiffX &&
                std::abs(diffY) <= maxReuseDiffY &&
                (type != FractalType::Phoenix || maxIter <= m_cachedMaxIter))
            {
                if (maxIter > m_cachedMaxIter)
                {
                    const int targetExtendIter = extendTarget(maxIter);
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
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        m_hasCachedOrbit = false;
                        return false;
                    }
                    m_cachedMaxIter = targetExtendIter;
                    m_cachedEscaped = escapedNow;
                    ++m_cachedOrbitVersion;
                }

                if (!m_cachedEscaped || m_cachedRefLen >= maxIter)
                {
                    refPosX = centerPosX + diffX / stepX;
                    refPosY = centerPosY - diffY / stepY;
                    bestRefLen = (!m_cachedEscaped && m_cachedRefLen >= maxIter && m_cachedRefLen <= maxIter * 2 + 512)
                        ? m_cachedRefLen
                        : maxIter;
                    reusedUnescapedCache = true;
                }
            }
            else if (m_cachedEscaped &&
                     std::abs(diffX) <= maxReuseDiffX &&
                     std::abs(diffY) <= maxReuseDiffY &&
                     (samePreparedGrid ||
                      (m_cachedSearchZoom > 0.0 &&
                       clampedZoom >= m_cachedSearchZoom * 0.50 &&
                       clampedZoom <= m_cachedSearchZoom * 2.00)))
            {
                // Reuse the best reference orbit already found for this exterior neighborhood across consecutive
                // zoom frames and bands so we don't re-run the 8x8 search and 1024-bit BigFloat orbits.
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
            const int initTargetMaxIter = (m_minActiveLimbs > 0 && maxIter >= 64)
                ? extendTarget(maxIter)
                : maxIter;
            int bestComputedMaxIter = initTargetMaxIter;

            const double cachedDiffX = m_hasCachedOrbit ? (m_cachedRefX - centerX).ToDouble() : 1e300;
            const double cachedDiffY = m_hasCachedOrbit ? (m_cachedRefY - centerY).ToDouble() : 1e300;

            // Reuse the cached orbit as our starting reference if it is at the current center or still well within the central viewport
            if (m_hasCachedOrbit &&
                m_cachedType == type &&
                m_cachedActiveLimbs >= activeLimbs &&
                !m_cachedOrbit.empty() &&
                (type != FractalType::Phoenix || maxIter <= m_cachedMaxIter) &&
                ((m_cachedRefX == centerX && m_cachedRefY == centerY) ||
                 (std::abs(cachedDiffX) <= maxReuseDiffX && std::abs(cachedDiffY) <= maxReuseDiffY)))
            {
                if (!m_cachedEscaped && maxIter > m_cachedMaxIter)
                {
                    const int targetExtendIter = extendTarget(maxIter);
                    m_cachedRefLen = stepBigFloatOrbit(
                        m_cachedRefX,
                        m_cachedRefY,
                        m_cachedMaxIter,
                        targetExtendIter,
                        m_cachedEndZx,
                        m_cachedEndZy,
                        m_cachedOrbit,
                        m_cachedEscaped);
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        m_hasCachedOrbit = false;
                        return false;
                    }
                    m_cachedMaxIter = targetExtendIter;
                    ++m_cachedOrbitVersion;
                }
                bestRefCx = m_cachedRefX;
                bestRefCy = m_cachedRefY;
                refPosX = centerPosX + cachedDiffX / stepX;
                refPosY = centerPosY - cachedDiffY / stepY;
                bestRefLen = (!m_cachedEscaped && m_cachedRefLen >= maxIter && m_cachedRefLen <= maxIter * 2 + 512)
                    ? m_cachedRefLen
                    : std::min(m_cachedRefLen, maxIter);
                bestEndZx = m_cachedEndZx;
                bestEndZy = m_cachedEndZy;
                bestEscaped = (bestRefLen < maxIter) && m_cachedEscaped;
                bestComputedMaxIter = m_cachedMaxIter;
            }
            else
            {
                // For fixed-target zoom movies, anchor the reference orbit directly on the final
                // zoom target once it lies inside the viewport so it stays reusable for all deeper frames.
                if (m_hasPreferredAnchor)
                {
                    const double anchorDiffX = (m_anchorX - centerX).ToDouble();
                    const double anchorDiffY = (m_anchorY - centerY).ToDouble();
                    if (std::abs(anchorDiffX) <= spanX * 0.45 && std::abs(anchorDiffY) <= spanY * 0.45)
                    {
                        bestRefCx = m_anchorX;
                        bestRefCy = m_anchorY;
                        refPosX = centerPosX + anchorDiffX / stepX;
                        refPosY = centerPosY - anchorDiffY / stepY;
                    }
                }

                bestEndZx.SetZero();
                bestEndZy.SetZero();
                bestRefLen = stepBigFloatOrbit(
                    bestRefCx,
                    bestRefCy,
                    0,
                    initTargetMaxIter,
                    bestEndZx,
                    bestEndZy,
                    m_cachedOrbit,
                    bestEscaped);
                ++m_cachedOrbitVersion;
                bestComputedMaxIter = initTargetMaxIter;
            }

            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                m_hasCachedOrbit = false;
                return false;
            }

            // If the center reference orbit escaped before maxIter, probe an 8x8 sample grid across the viewport
            // (up to 2 upgrades) to find a reference pixel genuinely inside the |z| < 2 disk (mod2 < 4.0) at m == bestRefLen.
            if (bestRefLen < maxIter && width >= 4 && height >= 4)
            {
                std::vector<ComplexOrbitPoint> candidateOrbit(static_cast<size_t>(initTargetMaxIter) + 1);
                constexpr int gridN = 8;
                constexpr int gridTotal = gridN * gridN;

                for (int attempt = 0; attempt < 2 && bestRefLen < maxIter; ++attempt)
                {
                    double candMod2[gridTotal];
                    int candPxArr[gridTotal];
                    int candPyArr[gridTotal];
                    for (int g = 0; g < gridTotal; ++g)
                    {
                        candMod2[g] = 4.0;
                        candPxArr[g] = -1;
                        candPyArr[g] = -1;
                    }
                    const ComplexOrbitPoint* orbitPtr = m_cachedOrbit.data();
                    const int curRefLen = bestRefLen;

                    #pragma omp parallel for schedule(dynamic, 1)
                    for (int g = 0; g < gridTotal; ++g)
                    {
                        if (cancelToken && cancelToken->load(std::memory_order_relaxed)) continue;
                        const int gy = g / gridN;
                        const int gx = g % gridN;
                        const int spy = ((2 * gy + 1) * height) / (2 * gridN);
                        const int spx = ((2 * gx + 1) * width) / (2 * gridN);
                        const double dcy = (refPosY - static_cast<double>(spy)) * stepY;
                        const double dcx = (static_cast<double>(spx) - refPosX) * stepX;
                        if (dcx == 0.0 && dcy == 0.0) continue;

                        if (type == FractalType::Mandelbrot)
                        {
                            double dx = 0.0, dy = 0.0;
                            int m = 0;
                            int iter = 0;
                            while (iter < maxIter)
                            {
                                double rx = orbitPtr[m].x;
                                double ry = orbitPtr[m].y;
                                const double zx = rx + dx;
                                const double zy = ry + dy;
                                const double mod2 = zx * zx + zy * zy;
                                if (mod2 > bailoutRadius2) break;
                                if (m == curRefLen)
                                {
                                    if (mod2 < 4.0)
                                    {
                                        candMod2[g] = mod2;
                                        candPxArr[g] = spx;
                                        candPyArr[g] = spy;
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
                                const double nextDx = sx * dx - sy * dy + dcx;
                                dy = sx * dy + sy * dx + dcy;
                                dx = nextDx;
                                ++m;
                                ++iter;
                            }
                        }
                        else
                        {
                            double dx = (type == FractalType::Phoenix) ? dcy : 0.0;
                            double dy = (type == FractalType::Phoenix) ? dcx : 0.0;
                            double prevDx = 0.0;
                            double prevDy = 0.0;
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

                                if (m == curRefLen)
                                {
                                    if (mod2 < 4.0)
                                    {
                                        candMod2[g] = mod2;
                                        candPxArr[g] = spx;
                                        candPyArr[g] = spy;
                                    }
                                    break;
                                }

                                const double dz2 = dx * dx + dy * dy;
                                if (type != FractalType::Phoenix && m > 0 && mod2 < dz2)
                                {
                                    dx = zx;
                                    dy = zy;
                                    m = 0;
                                    continue;
                                }

                                detail::StepPerturbationDelta(
                                    type, rx, ry, zx, zy, dcx, dcy, dx, dy, prevDx, prevDy);

                                m++;
                                iter++;
                            }
                        }
                    }

                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        return false;
                    }

                    int nextCandPx = -1;
                    int nextCandPy = -1;
                    double minMod2AtRefEnd = 4.0;
                    for (int g = 0; g < gridTotal; ++g)
                    {
                        if (candPxArr[g] >= 0 && candMod2[g] < minMod2AtRefEnd)
                        {
                            minMod2AtRefEnd = candMod2[g];
                            nextCandPx = candPxArr[g];
                            nextCandPy = candPyArr[g];
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
                        initTargetMaxIter,
                        candEndZx,
                        candEndZy,
                        candidateOrbit,
                        candEscaped);

                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        return false;
                    }

                    if (candLen > bestRefLen)
                    {
                        m_cachedOrbit.swap(candidateOrbit);
                        ++m_cachedOrbitVersion;
                        bestRefLen = candLen;
                        refPosX = static_cast<double>(nextCandPx);
                        refPosY = static_cast<double>(nextCandPy);
                        bestRefCx = candCx;
                        bestRefCy = candCy;
                        bestEndZx = candEndZx;
                        bestEndZy = candEndZy;
                        bestEscaped = candEscaped;
                        bestComputedMaxIter = initTargetMaxIter;
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
            m_cachedMaxIter = bestComputedMaxIter;
            m_cachedSearchZoom = clampedZoom;
            m_cachedEscaped = bestEscaped;
        }

        if (executionMode == ExecutionMode::Automatic && type == FractalType::Mandelbrot)
        {
            // Include the quarter-pixel halo used by optional subpixel refinement.
            const double maxCx = std::max(std::abs(-.25 - refPosX), std::abs(width - .75 - refPosX)) * stepX;
            const double maxCy = std::max(std::abs(refPosY + .25), std::abs(refPosY - (height - .75))) * stepY;
            const double cMax = std::hypot(maxCx, maxCy);
            if (m_cachedBlaVersion != m_cachedOrbitVersion ||
                m_cachedBlaRefLen != bestRefLen ||
                cMax > m_cachedBlaCMax * (1.0 + 1e-12) ||
                cMax < m_cachedBlaCMax * 0.25)
            {
                m_cachedMandelbrotBla.Build(m_cachedOrbit.data(), bestRefLen, cMax, cancelToken);
                if (cancelToken && cancelToken->load(std::memory_order_relaxed)) return false;
                m_cachedBlaVersion = m_cachedOrbitVersion;
                m_cachedBlaRefLen = bestRefLen;
                m_cachedBlaCMax = cMax;
            }
        }
        else
        {
            m_cachedMandelbrotBla = {};
            m_cachedBlaVersion = 0;
            m_cachedBlaRefLen = 0;
            m_cachedBlaCMax = 0.0;
        }

        outState.refPosX = refPosX;
        outState.refPosY = refPosY;
        outState.refLen = bestRefLen;
        outState.refCxDouble = m_cachedRefX.ToDouble();
        outState.refCyDouble = m_cachedRefY.ToDouble();
        outState.orbitVersion = m_cachedOrbitVersion;
        outState.orbit = m_cachedOrbit.data();
        outState.mandelbrotBla = &m_cachedMandelbrotBla;
        return true;
    }
}
