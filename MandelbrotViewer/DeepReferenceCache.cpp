#include "pch.h"
#include "DeepReferenceCache.h"
#include "FractalFormulas.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot
{
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

        BigFloat hpX = config.hpCenterX;
        BigFloat hpY = config.hpCenterY;
        if (std::abs(hpX.ToDouble() - config.centerX) > 1e-14 ||
            std::abs(hpY.ToDouble() - config.centerY) > 1e-14)
        {
            hpX = BigFloat(config.centerX);
            hpY = BigFloat(config.centerY);
        }

        DeepReferenceState unusedState{};
        Ensure(config, hpX, hpY, executionMode, unusedState, cancelToken);
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
        const int activeLimbs = std::clamp((rawLimbs + 1) & ~1, 4, BigFloat::N);

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
                else if (type == FractalType::Tricorn)
                {
                    ioZy = -twoZxy + cRefY;
                    ioZx = Zx2 - Zy2 + cRefX;
                }
                else if (type == FractalType::Celtic)
                {
                    ioZy = twoZxy + cRefY;
                    ioZx = (Zx2 - Zy2).Abs() + cRefX;
                }
                else if (type == FractalType::Buffalo)
                {
                    ioZy = -twoZxy.Abs() + cRefY;
                    ioZx = (Zx2 - Zy2).Abs() + cRefX;
                }
                else if (type == FractalType::CubicMultibrot)
                {
                    const BigFloat threeZx2 = Zx2 + Zx2 + Zx2;
                    const BigFloat threeZy2 = Zy2 + Zy2 + Zy2;
                    const BigFloat nextZx = ioZx.Mul(Zx2 - threeZy2, activeLimbs) + cRefX;
                    const BigFloat nextZy = ioZy.Mul(threeZx2 - Zy2, activeLimbs) + cRefY;
                    ioZx = nextZx;
                    ioZy = nextZy;
                }
                else // FractalType::Phoenix
                {
                    const BigFloat nextZx = Zx2 - Zy2 + phoenixC - prevZx.Mul(phoenixHalf, activeLimbs);
                    const BigFloat nextZy = twoZxy - prevZy.Mul(phoenixHalf, activeLimbs);
                    prevZx = ioZx;
                    prevZy = ioZy;
                    ioZx = nextZx;
                    ioZy = nextZy;
                }
            }
            return len;
        };

        double refPosX = centerPosX;
        double refPosY = centerPosY;
        int bestRefLen = 0;
        bool reusedUnescapedCache = false;
        // Include full-frame vertical span and 8x8 search candidate margin (+/- 0.4375 * span)
        // so reference orbits selected near the outer grid columns/rows are reused across all bands.
        const double cacheSpanY = std::max(spanY * 2.0, spanX * 2.0);

        // Check if we can reuse or incrementally extend the cached reference orbit from the previous frame/band
        if (m_hasCachedOrbit &&
            m_cachedType == type &&
            m_cachedActiveLimbs >= activeLimbs &&
            !m_cachedOrbit.empty())
        {
            const double diffX = (m_cachedRefX - centerX).ToDouble();
            const double diffY = (m_cachedRefY - centerY).ToDouble();

            if (!m_cachedEscaped &&
                std::abs(diffX) <= spanX * 0.48 &&
                std::abs(diffY) <= cacheSpanY * 0.48 &&
                (type != FractalType::Phoenix || maxIter <= m_cachedMaxIter))
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
                    ++m_cachedOrbitVersion;
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
                     std::abs(diffX) <= spanX * 0.45 &&
                     std::abs(diffY) <= cacheSpanY * 0.45 &&
                     m_cachedSearchZoom > 0.0 &&
                     clampedZoom >= m_cachedSearchZoom * 0.65 &&
                     clampedZoom <= m_cachedSearchZoom * 1.50)
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

            const double cachedDiffX = m_hasCachedOrbit ? (m_cachedRefX - centerX).ToDouble() : 1e300;
            const double cachedDiffY = m_hasCachedOrbit ? (m_cachedRefY - centerY).ToDouble() : 1e300;

            // Reuse the cached orbit as our starting reference if it is at the current center or still well within the central viewport
            if (m_hasCachedOrbit &&
                m_cachedType == type &&
                m_cachedActiveLimbs >= activeLimbs &&
                !m_cachedOrbit.empty() &&
                (type != FractalType::Phoenix || maxIter <= m_cachedMaxIter) &&
                ((m_cachedRefX == centerX && m_cachedRefY == centerY) ||
                 (std::abs(cachedDiffX) <= spanX * 0.45 && std::abs(cachedDiffY) <= cacheSpanY * 0.45)))
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
                    ++m_cachedOrbitVersion;
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
                ++m_cachedOrbitVersion;
            }

            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                return false;
            }

            // If the center reference orbit escaped before maxIter, probe an 8x8 sample grid across the viewport
            // (up to 2 upgrades) to find a reference pixel genuinely inside the |z| < 2 disk (mod2 < 4.0) at m == bestRefLen.
            if (bestRefLen < maxIter && width >= 4 && height >= 4)
            {
                std::vector<ComplexOrbitPoint> candidateOrbit(static_cast<size_t>(maxIter) + 1);
                constexpr int gridN = 8;

                for (int attempt = 0; attempt < 2 && bestRefLen < maxIter; ++attempt)
                {
                    int nextCandPx = -1;
                    int nextCandPy = -1;
                    double minMod2AtRefEnd = 4.0;
                    const ComplexOrbitPoint* orbitPtr = m_cachedOrbit.data();

                    for (int gy = 0; gy < gridN; ++gy)
                    {
                        if (cancelToken && cancelToken->load(std::memory_order_relaxed)) return false;
                        const int spy = ((2 * gy + 1) * height) / (2 * gridN);
                        const double dcy = (refPosY - static_cast<double>(spy)) * stepY;

                        for (int gx = 0; gx < gridN; ++gx)
                        {
                            const int spx = ((2 * gx + 1) * width) / (2 * gridN);
                            const double dcx = (static_cast<double>(spx) - refPosX) * stepX;
                            if (dcx == 0.0 && dcy == 0.0) continue;

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
