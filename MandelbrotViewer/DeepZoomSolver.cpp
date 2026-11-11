#include "pch.h"
#include "DeepZoomSolver.h"
#include "FractalFormulas.h"
#include "JuliaSolver.h"
#include "MandelbrotBla.h"
#include "RenderPass.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Mandelbrot
{
    namespace
    {
        double ComputeNewtonDeepZoom(
            const FractalConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            int activeLimbs,
            double spanX,
            double spanY,
            double stepX,
            double stepY,
            double invLn2,
            std::chrono::high_resolution_clock::time_point startTime,
            std::vector<float>& sampleWeights,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken,
            std::vector<SubpixelSamples>* subpixelSamples)
        {
            const int width = config.width;
            const int height = config.height;
            const int maxIter = config.maxIterations;
            const bool smooth = config.smooth;

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

            auto evalNewtonPixel = [&](double px, double py) -> float
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed)) return -1.0f;
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

            detail::ExecuteRenderPass(
                width,
                height,
                config.renderStride,
                config.edgeRefine,
                smooth,
                FractalType::Newton,
                outIterations,
                cancelToken,
                evalNewtonPixel);

            detail::RefineSubpixels(width, height, smooth, FractalType::Newton, outIterations, sampleWeights,
                subpixelSamples, cancelToken, evalNewtonPixel);

            const auto endTime = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
            return elapsed.count();
        }
    }

    double ComputeDeepZoomFrame(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        ExecutionMode executionMode,
        DeepReferenceCache& referenceCache,
        std::vector<float>& sampleWeights,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples)
    {
        if (subpixelSamples) subpixelSamples->clear();
        if (config.juliaSet)
        {
            auto julia = config;
            julia.SetCenter(centerX, centerY);
            return ComputeJuliaSet(julia, outIterations, sampleWeights, cancelToken, subpixelSamples);
        }
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
            return ComputeNewtonDeepZoom(
                config, centerX, centerY, activeLimbs,
                spanX, spanY, stepX, stepY, invLn2, startTime,
                sampleWeights, outIterations, cancelToken, subpixelSamples);
        }

        // Escape-time fractals:
        // High-precision reference orbit + hardware double perturbation with Zhuoran's dynamic rebasing,
        // cross-frame reference orbit caching, and fast sample-grid reference search when the center escapes early.
        constexpr double bailoutRadius2 = 16.0;
        DeepReferenceState refState{};
        if (!referenceCache.Ensure(config, centerX, centerY, executionMode, refState, cancelToken))
        {
            return 0.0;
        }

        const double refPosX = refState.refPosX;
        const double refPosY = refState.refPosY;
        const int refLen = refState.refLen;
        const ComplexOrbitPoint* orbitPtr = refState.orbit;
        const double refCxDouble = refState.refCxDouble;
        const double refCyDouble = refState.refCyDouble;
        const detail::BlaTable& bla = *refState.mandelbrotBla;

        auto evalDeepPixel = [&](double px, double py) -> float
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed)) return -1.0f;
            const double dcy = (refPosY - static_cast<double>(py)) * stepY;
            const double dcx = (static_cast<double>(px) - refPosX) * stepX;
            double dx = 0.0;
            double dy = 0.0;
            double prevDx = 0.0;
            double prevDy = 0.0;
            int m = 0;
            int iter = 0;
            double escapedZx = 0.0;
            double escapedZy = 0.0;
            double escapedPrevZx = 0.0;
            double escapedPrevZy = 0.0;

            switch (type)
            {
            case FractalType::Mandelbrot:
            {
                auto result = bla.Enabled()
                    ? detail::EvalMandelbrotPerturbation<true>(orbitPtr, refLen, maxIter, dcx, dcy, bla)
                    : detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcx, dcy, bla);
                if (result.replay != detail::ReplayReason::None)
                    result = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcx, dcy, bla);
                iter = result.iterations;
                escapedZx = result.x;
                escapedZy = result.y;
                break;
            }

            case FractalType::BurningShip:
            case FractalType::Tricorn:
            case FractalType::Celtic:
            case FractalType::Buffalo:
            case FractalType::CubicMultibrot:
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

                    detail::StepPerturbationDelta(
                        type, rx, ry, zx, zy, dcx, dcy, dx, dy, prevDx, prevDy);
                    m++;
                    iter++;
                }
                break;

            case FractalType::Phoenix:
            default:
            {
                dx = dcy;
                dy = dcx;
                prevDx = 0.0;
                prevDy = 0.0;
                bool inPerturbation = true;
                double zr = 0.0;
                double zi = 0.0;
                double prevZr = 0.0;
                double prevZi = 0.0;

                while (iter < maxIter)
                {
                    if (inPerturbation)
                    {
                        const double rx = orbitPtr[m].x;
                        const double ry = orbitPtr[m].y;
                        const double zx = rx + dx;
                        const double zy = ry + dy;
                        const double mod2 = zx * zx + zy * zy;

                        if (mod2 > bailoutRadius2)
                        {
                            escapedZx = zx;
                            escapedZy = zy;
                            escapedPrevZx = (m > 0) ? (orbitPtr[m - 1].x + prevDx) : 0.0;
                            escapedPrevZy = (m > 0) ? (orbitPtr[m - 1].y + prevDy) : 0.0;
                            break;
                        }

                        if (m == refLen)
                        {
                            inPerturbation = false;
                            zr = zx;
                            zi = zy;
                            prevZr = (m > 0) ? (orbitPtr[m - 1].x + prevDx) : 0.0;
                            prevZi = (m > 0) ? (orbitPtr[m - 1].y + prevDy) : 0.0;
                            continue;
                        }

                        detail::StepPerturbationDelta(
                            FractalType::Phoenix, rx, ry, zx, zy, dcx, dcy, dx, dy, prevDx, prevDy);
                        m++;
                        iter++;
                    }
                    else
                    {
                        const double mod2 = zr * zr + zi * zi;
                        if (mod2 > bailoutRadius2)
                        {
                            escapedZx = zr;
                            escapedZy = zi;
                            escapedPrevZx = prevZr;
                            escapedPrevZy = prevZi;
                            break;
                        }
                        const double nextZr = zr * zr - zi * zi + 0.5667 - 0.5 * prevZr;
                        const double nextZi = 2.0 * zr * zi - 0.5 * prevZi;
                        prevZr = zr;
                        prevZi = zi;
                        zr = nextZr;
                        zi = nextZi;
                        iter++;
                    }
                }
                break;
            }
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
                return detail::ComputeSmoothEscapeIteration(
                    type, iter, escapedZx, escapedZy, escapedPrevZx, escapedPrevZy,
                    refCxDouble + dcx, refCyDouble + dcy, invLn2);
            }

            return static_cast<float>(iter);
        };

#if defined(_M_ARM64)
        auto finishMandelbrotDeepResult = [&](const detail::PerturbationResult& res, double dcx, double dcy) noexcept -> float
        {
            if (res.iterations >= maxIter) return -1.0f;
            if (!smooth) return static_cast<float>(res.iterations);
            return detail::ComputeSmoothEscapeIteration(
                FractalType::Mandelbrot, res.iterations, res.x, res.y, 0.0, 0.0,
                refCxDouble + dcx, refCyDouble + dcy, invLn2);
        };

        auto evalDeepMandelbrotPair = [&](double px0, double px1, double py, float outSamples[2]) noexcept
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                outSamples[0] = -1.0f;
                outSamples[1] = -1.0f;
                return;
            }
            const double dcy = (refPosY - py) * stepY;
            const double dcx0 = (px0 - refPosX) * stepX;
            const double dcx1 = (px1 - refPosX) * stepX;
            detail::PerturbationResult res0, res1;
            if (bla.Enabled())
            {
                detail::EvalMandelbrotPerturbationPair<true>(orbitPtr, refLen, maxIter, dcx0, dcx1, dcy, bla, res0, res1);
                if (res0.replay != detail::ReplayReason::None && res1.replay != detail::ReplayReason::None)
                {
                    detail::EvalMandelbrotPerturbationPair<false>(orbitPtr, refLen, maxIter, dcx0, dcx1, dcy, bla, res0, res1);
                }
                else
                {
                    if (res0.replay != detail::ReplayReason::None)
                        res0 = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcx0, dcy, bla);
                    if (res1.replay != detail::ReplayReason::None)
                        res1 = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcx1, dcy, bla);
                }
            }
            else
            {
                detail::EvalMandelbrotPerturbationPair<false>(orbitPtr, refLen, maxIter, dcx0, dcx1, dcy, bla, res0, res1);
            }
            outSamples[0] = finishMandelbrotDeepResult(res0, dcx0, dcy);
            outSamples[1] = finishMandelbrotDeepResult(res1, dcx1, dcy);
        };

        auto evalDeepMandelbrotQuad = [&](const double pxCoords[4], double py01, double py23, float outSamples[4]) noexcept -> int
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed))
            {
                outSamples[0] = outSamples[1] = outSamples[2] = outSamples[3] = -1.0f;
                return 0;
            }
            const double dcy01 = (refPosY - py01) * stepY;
            const double dcy23 = (refPosY - py23) * stepY;
            const double dcxCoords[4]{
                (pxCoords[0] - refPosX) * stepX,
                (pxCoords[1] - refPosX) * stepX,
                (pxCoords[2] - refPosX) * stepX,
                (pxCoords[3] - refPosX) * stepX
            };
            detail::PerturbationResult res[4];
            int quadSteps = 0;
            if (bla.Enabled())
            {
                quadSteps = detail::EvalMandelbrotPerturbationQuad<true>(
                    orbitPtr, refLen, maxIter, dcxCoords, dcy01, dcy23, bla, res);
                const bool r0 = res[0].replay != detail::ReplayReason::None;
                const bool r1 = res[1].replay != detail::ReplayReason::None;
                const bool r2 = res[2].replay != detail::ReplayReason::None;
                const bool r3 = res[3].replay != detail::ReplayReason::None;
                if (r0 && r1 && r2 && r3)
                {
                    quadSteps += detail::EvalMandelbrotPerturbationQuad<false>(
                        orbitPtr, refLen, maxIter, dcxCoords, dcy01, dcy23, bla, res);
                }
                else
                {
                    if (r0 && r1)
                    {
                        detail::EvalMandelbrotPerturbationPair<false>(
                            orbitPtr, refLen, maxIter, dcxCoords[0], dcxCoords[1], dcy01, bla, res[0], res[1]);
                    }
                    else
                    {
                        if (r0) res[0] = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcxCoords[0], dcy01, bla);
                        if (r1) res[1] = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcxCoords[1], dcy01, bla);
                    }
                    if (r2 && r3)
                    {
                        detail::EvalMandelbrotPerturbationPair<false>(
                            orbitPtr, refLen, maxIter, dcxCoords[2], dcxCoords[3], dcy23, bla, res[2], res[3]);
                    }
                    else
                    {
                        if (r2) res[2] = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcxCoords[2], dcy23, bla);
                        if (r3) res[3] = detail::EvalMandelbrotPerturbation<false>(orbitPtr, refLen, maxIter, dcxCoords[3], dcy23, bla);
                    }
                }
            }
            else
            {
                quadSteps = detail::EvalMandelbrotPerturbationQuad<false>(
                    orbitPtr, refLen, maxIter, dcxCoords, dcy01, dcy23, bla, res);
            }
            outSamples[0] = finishMandelbrotDeepResult(res[0], dcxCoords[0], dcy01);
            outSamples[1] = finishMandelbrotDeepResult(res[1], dcxCoords[1], dcy01);
            outSamples[2] = finishMandelbrotDeepResult(res[2], dcxCoords[2], dcy23);
            outSamples[3] = finishMandelbrotDeepResult(res[3], dcxCoords[3], dcy23);
            return quadSteps;
        };

        if (executionMode == ExecutionMode::Automatic &&
            type == FractalType::Mandelbrot &&
            (config.renderStride <= 1 || width < 8 || height < 8))
        {
            constexpr int kSegmentWidth = 64;
            const int segmentsPerRow = std::max(1, (width + kSegmentWidth - 1) / kSegmentWidth);
            const int totalSegments = height * segmentsPerRow;
            #pragma omp parallel for schedule(dynamic, 1)
            for (int seg = 0; seg < totalSegments; ++seg)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed)) continue;
                const int py = seg / segmentsPerRow;
                const int pxStart = (seg % segmentsPerRow) * kSegmentWidth;
                const int pxEnd = std::min(width, pxStart + kSegmentWidth);
                float* row = outIterations.data() + static_cast<size_t>(py) * width;
                const double pyDouble = static_cast<double>(py);
                const double dcy = (refPosY - pyDouble) * stepY;
                int px = pxStart;
                while (px + 3 < pxEnd)
                {
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed)) break;
                    const double pxCoords[4]{
                        static_cast<double>(px),
                        static_cast<double>(px + 1),
                        static_cast<double>(px + 2),
                        static_cast<double>(px + 3)
                    };
                    const int quadSteps = evalDeepMandelbrotQuad(pxCoords, pyDouble, pyDouble, row + px);
                    const bool allInterior = row[px] < 0.0f && row[px + 1] < 0.0f &&
                        row[px + 2] < 0.0f && row[px + 3] < 0.0f;
                    px += 4;
                    if (allInterior && px + 8 <= pxEnd && quadSteps * ((pxEnd - px) / 4) >= maxIter * 2)
                    {
                        auto tryCertify = [&](int count) noexcept -> bool
                        {
                            const int firstP = px, lastP = px + count - 1;
                            const double dcxMid = (0.5 * (firstP + lastP) - refPosX) * stepX;
                            const double halfSpan = 0.5 * static_cast<double>(lastP - firstP) * stepX;
                            if (!detail::CertifyMandelbrotDeepInteriorStrip(
                                orbitPtr, refLen, maxIter, dcxMid, dcy, halfSpan)) return false;
                            std::fill(row + firstP, row + lastP + 1, -1.0f);
                            px += count;
                            return true;
                        };
                        if (tryCertify(std::min(16, pxEnd - px)) && px + 8 <= pxEnd)
                        {
                            tryCertify(pxEnd - px);
                        }
                    }
                }
                for (; px + 1 < pxEnd; px += 2)
                {
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed)) break;
                    evalDeepMandelbrotPair(static_cast<double>(px), static_cast<double>(px + 1), pyDouble, row + px);
                }
                if (px < pxEnd) row[px] = evalDeepPixel(px, py);
            }
        }
        else
#endif
        {
            detail::ExecuteRenderPass(
                width,
                height,
                config.renderStride,
                config.edgeRefine,
                smooth,
                type,
                outIterations,
                cancelToken,
                evalDeepPixel);
        }

#if defined(_M_ARM64)
        if (executionMode == ExecutionMode::Automatic && type == FractalType::Mandelbrot)
        {
            detail::RefineSubpixelsQuads(width, height, smooth, type, outIterations, sampleWeights,
                subpixelSamples, cancelToken,
                [&](double px0, double px1, double py0, double py1, float outSamples[4])
                {
                    const double pxCoords[4]{ px0, px1, px0, px1 };
                    evalDeepMandelbrotQuad(pxCoords, py0, py1, outSamples);
                });
        }
        else
#endif
        {
            detail::RefineSubpixels(width, height, smooth, type, outIterations, sampleWeights,
                subpixelSamples, cancelToken, evalDeepPixel);
        }

        const auto endTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }
}
