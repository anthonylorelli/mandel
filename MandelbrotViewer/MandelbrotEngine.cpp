#include "pch.h"
#include "MandelbrotEngine.h"
#include "DeepZoomSolver.h"
#include "FractalFormulas.h"
#include "JuliaSolver.h"
#include "MandelbrotSimd.h"
#include "RenderPass.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Mandelbrot
{
    double MandelbrotEngine::Compute(
        const FractalConfig& config,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples)
    {
        if (subpixelSamples) subpixelSamples->clear();
        if (config.width <= 0 || config.height <= 0) return 0.0;
        detail::EnsureOpenMpAllCores();
        if (config.juliaSet)
        {
            return ComputeJuliaSet(config, outIterations, m_sampleWeights, cancelToken, subpixelSamples);
        }

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
            return ComputeDeepZoom(config, hpX, hpY, outIterations, cancelToken, subpixelSamples);
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

        auto evalStandardPixel = [&](double px, double py) -> float
        {
            if (cancelToken && cancelToken->load(std::memory_order_relaxed)) return -1.0f;
            const double cy = config.centerY + (halfH - (static_cast<double>(py) + 0.5)) * dy;
            const double cx = config.centerX + ((static_cast<double>(px) + 0.5) - halfW) * dx;
            return detail::EvalStandardPoint(type, cx, cy, maxIter, smooth);
        };

#if defined(_M_ARM64)
        if (m_executionMode == ExecutionMode::Automatic &&
            type == FractalType::Mandelbrot && config.renderStride <= 1)
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
                const double cy = config.centerY + (halfH - (static_cast<double>(py) + 0.5)) * dy;
                int px = pxStart;
                while (px + 3 < pxEnd)
                {
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed)) break;
                    const double cxCoords[4]{
                        config.centerX + ((static_cast<double>(px) + 0.5) - halfW) * dx,
                        config.centerX + ((static_cast<double>(px + 1) + 0.5) - halfW) * dx,
                        config.centerX + ((static_cast<double>(px + 2) + 0.5) - halfW) * dx,
                        config.centerX + ((static_cast<double>(px + 3) + 0.5) - halfW) * dx
                    };
                    const int quadSteps = detail::EvalMandelbrotQuad(cxCoords, cy, cy, maxIter, smooth, row + px);
                    const bool allInterior = row[px] < 0.0f && row[px + 1] < 0.0f &&
                        row[px + 2] < 0.0f && row[px + 3] < 0.0f;
                    px += 4;
                    if (allInterior && px + 8 <= pxEnd && quadSteps * ((pxEnd - px) / 4) >= maxIter * 2)
                    {
                        auto tryCertify = [&](int count) noexcept -> bool
                        {
                            const int firstP = px, lastP = px + count - 1;
                            const double midCx = config.centerX + ((0.5 * (firstP + lastP) + 0.5) - halfW) * dx;
                            const double halfSpan = 0.5 * static_cast<double>(lastP - firstP) * dx;
                            if (!detail::CertifyMandelbrotStandardInteriorStrip(midCx, cy, halfSpan, maxIter)) return false;
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
                    const double x0 = config.centerX + ((static_cast<double>(px) + 0.5) - halfW) * dx;
                    const double x1 = config.centerX + ((static_cast<double>(px + 1) + 0.5) - halfW) * dx;
                    detail::EvalMandelbrotPair(x0, x1, cy, maxIter, smooth, row + px);
                }
                if (px < pxEnd) row[px] = evalStandardPixel(px, py);
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
                evalStandardPixel);
        }

#if defined(_M_ARM64)
        if (m_executionMode == ExecutionMode::Automatic && type == FractalType::Mandelbrot)
        {
            detail::RefineSubpixelsQuads(width, height, smooth, type, outIterations, m_sampleWeights,
                subpixelSamples, cancelToken,
                [&](double px0, double px1, double py0, double py1, float outSamples[4])
                {
                    const double cy0 = config.centerY + (halfH - (py0 + 0.5)) * dy;
                    const double cy1 = config.centerY + (halfH - (py1 + 0.5)) * dy;
                    const double x0 = config.centerX + ((px0 + 0.5) - halfW) * dx;
                    const double x1 = config.centerX + ((px1 + 0.5) - halfW) * dx;
                    const double cxCoords[4]{ x0, x1, x0, x1 };
                    detail::EvalMandelbrotQuad(cxCoords, cy0, cy1, maxIter, smooth, outSamples);
                });
        }
        else
#endif
        {
            detail::RefineSubpixels(width, height, smooth, type, outIterations, m_sampleWeights,
                subpixelSamples, cancelToken, evalStandardPixel);
        }

        const auto endTime = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
        return elapsed.count();
    }

    double MandelbrotEngine::ComputeDeepZoom(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples)
    {
        detail::EnsureOpenMpAllCores();
        return ComputeDeepZoomFrame(
            config, centerX, centerY, m_executionMode, m_deepCache,
            m_sampleWeights, outIterations, cancelToken, subpixelSamples);
    }

    void MandelbrotEngine::PrepareDeepReference(
        const FractalConfig& config,
        const std::atomic<bool>* cancelToken)
    {
        m_deepCache.Prepare(config, m_executionMode, cancelToken);
    }

    void MandelbrotEngine::ZoomViewport(
        FractalConfig& config,
        double factor,
        double pivotPx,
        double pivotPy)
    {
        Mandelbrot::ZoomViewport(config, factor, pivotPx, pivotPy);
    }

    void MandelbrotEngine::PanViewport(
        FractalConfig& config,
        const BigFloat& startX,
        const BigFloat& startY,
        double deltaPx,
        double deltaPy)
    {
        Mandelbrot::PanViewport(config, startX, startY, deltaPx, deltaPy);
    }

    bool MandelbrotEngine::StepZoomAnimation(
        FractalConfig& config,
        const BigFloat& targetX,
        const BigFloat& targetY,
        double zoomFactor,
        double panBlend)
    {
        return Mandelbrot::StepZoomAnimation(config, targetX, targetY, zoomFactor, panBlend);
    }

    bool MandelbrotEngine::ShouldRetargetZoom(
        const FractalConfig& config,
        const std::vector<float>& iterations,
        const BigFloat& targetX,
        const BigFloat& targetY)
    {
        return Mandelbrot::ShouldRetargetZoom(config, iterations, targetX, targetY);
    }

    bool MandelbrotEngine::PickInterestingTarget(
        const FractalConfig& config,
        const std::vector<float>& iterations,
        BigFloat& outTargetX,
        BigFloat& outTargetY,
        uint64_t randomSeed)
    {
        return Mandelbrot::PickInterestingTarget(config, iterations, outTargetX, outTargetY, randomSeed);
    }

    int MandelbrotEngine::RecommendMaxIterations(
        double zoom,
        FractalType type,
        int baseIterations)
    {
        return Mandelbrot::RecommendMaxIterations(zoom, type, baseIterations);
    }

    void MandelbrotEngine::ScreenToComplex(
        double px, double py,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outCx, double& outCy)
    {
        Mandelbrot::ScreenToComplex(px, py, width, height, centerX, centerY, zoom, outCx, outCy);
    }

    void MandelbrotEngine::ComplexToScreen(
        double cx, double cy,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outPx, double& outPy)
    {
        Mandelbrot::ComplexToScreen(cx, cy, width, height, centerX, centerY, zoom, outPx, outPy);
    }
}
