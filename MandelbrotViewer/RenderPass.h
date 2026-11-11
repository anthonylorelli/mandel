#pragma once
#include "FractalCatalog.h"
#include "PixelSampling.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <omp.h>
#include <stdexcept>
#include <vector>

namespace Mandelbrot::detail
{
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
            // Partition rows into 64-pixel horizontal segments so all cores share heavy scanlines
            // and localized central zoom targets instead of stalling on a few long rows.
            constexpr int kSegmentWidth = 64;
            const int segmentsPerRow = std::max(1, (width + kSegmentWidth - 1) / kSegmentWidth);
            const int totalSegments = height * segmentsPerRow;
            #pragma omp parallel for schedule(dynamic, 1)
            for (int seg = 0; seg < totalSegments; ++seg)
            {
                if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                {
                    continue;
                }

                const int py = seg / segmentsPerRow;
                const int pxStart = (seg % segmentsPerRow) * kSegmentWidth;
                const int pxEnd = std::min(width, pxStart + kSegmentWidth);
                float* rowPtr = &outIterations[static_cast<size_t>(py) * width];
                for (int px = pxStart; px < pxEnd; ++px)
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

    template <typename EvalQuadFn>
    void RefineSubpixelsQuads(int width, int height, bool smooth, FractalType type,
        const std::vector<float>& centers, std::vector<float>& weights,
        std::vector<SubpixelSamples>* samples, const std::atomic<bool>* cancelToken,
        EvalQuadFn&& evalQuad)
    {
        if (!samples) return;
        samples->clear();
        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };
        if (canceled()) return;
        if (centers.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Supersampling image is too large.");

        weights.resize(centers.size());
        std::vector<size_t> rowOffsets(static_cast<size_t>(height) + 1, 0);
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < height; ++y)
        {
            if (canceled()) continue;
            size_t selected = 0;
            const int y0 = std::max(0, y - 1), y1 = std::min(height - 1, y + 1);
            for (int x = 0; x < width; ++x)
            {
                const size_t index = static_cast<size_t>(y) * width + x;
                const float center = centers[index];
                const int x0 = std::max(0, x - 1), x1 = std::min(width - 1, x + 1);
                if (center < 0.0f)
                {
                    bool boundary = false;
                    for (int ny = y0; ny <= y1 && !boundary; ++ny)
                    {
                        const float* nRow = centers.data() + static_cast<size_t>(ny) * width;
                        for (int nx = x0; nx <= x1; ++nx)
                            if (nRow[nx] >= 0.0f) { boundary = true; break; }
                    }
                    weights[index] = boundary ? 1.0f : 0.0f;
                    if (boundary) ++selected;
                    continue;
                }
                float variation = 0.0f;
                bool boundary = false;
                const float a = type == FractalType::Newton ? std::fmod(center, 1000000.0f) : center;
                for (int ny = y0; ny <= y1; ++ny)
                {
                    const float* nRow = centers.data() + static_cast<size_t>(ny) * width;
                    for (int nx = x0; nx <= x1; ++nx)
                    {
                        if (nx == x && ny == y) continue;
                        const float neighbor = nRow[nx];
                        if (neighbor < 0.0f) { boundary = true; continue; }
                        if (!SameBasin(center, neighbor, type) || (!smooth && center != neighbor))
                            boundary = true;
                        const float b = type == FractalType::Newton ? std::fmod(neighbor, 1000000.0f) : neighbor;
                        variation = std::max(variation, std::abs(a - b) / (10.0f + std::min(a, b)));
                    }
                }
                // A smooth transition avoids a second source of popping at the refinement
                // threshold. The mask depends on local detail, never on render timings.
                const float t = std::clamp((variation - 0.015f) / 0.065f, 0.0f, 1.0f);
                const float weight = boundary ? 1.0f : t * t * (3.0f - 2.0f * t);
                weights[index] = weight;
                if (weight > 0.0f) ++selected;
            }
            rowOffsets[static_cast<size_t>(y) + 1] = selected;
        }
        if (canceled()) return;
        for (int y = 0; y < height; ++y) rowOffsets[y + 1] += rowOffsets[y];
        if (rowOffsets.back() == 0) return;
        samples->resize(rowOffsets.back());

        #pragma omp parallel for schedule(static)
        for (int y = 0; y < height; ++y)
        {
            if (rowOffsets[y] == rowOffsets[y + 1]) continue;
            size_t output = rowOffsets[y];
            for (int x = 0; x < width; ++x)
            {
                const size_t index = static_cast<size_t>(y) * width + x;
                if (weights[index] <= 0.0f) continue;
                auto& sample = (*samples)[output++];
                sample.pixelIndex = static_cast<uint32_t>(index);
                sample.weight = weights[index];
            }
        }

        const intptr_t sampleCount = static_cast<intptr_t>(samples->size());
        #pragma omp parallel for schedule(dynamic, 32)
        for (intptr_t i = 0; i < sampleCount; ++i)
        {
            if (canceled()) continue;
            auto& sample = (*samples)[i];
            const int y = static_cast<int>(sample.pixelIndex / static_cast<uint32_t>(width));
            const int x = static_cast<int>(sample.pixelIndex % static_cast<uint32_t>(width));
            evalQuad(x - 0.25, x + 0.25, y - 0.25, y + 0.25, sample.iterations.data());
        }
        if (canceled()) samples->clear();
    }

    template <typename EvalPixelFn>
    void RefineSubpixels(int width, int height, bool smooth, FractalType type,
        const std::vector<float>& centers, std::vector<float>& weights,
        std::vector<SubpixelSamples>* samples, const std::atomic<bool>* cancelToken,
        EvalPixelFn&& evalPixel)
    {
        RefineSubpixelsQuads(width, height, smooth, type, centers, weights, samples, cancelToken,
            [&](double x0, double x1, double y0, double y1, float* out)
            {
                out[0] = evalPixel(x0, y0);
                out[1] = evalPixel(x1, y0);
                out[2] = evalPixel(x0, y1);
                out[3] = evalPixel(x1, y1);
            });
    }

    inline void DisableCurrentThreadPowerThrottling()
    {
        THREAD_POWER_THROTTLING_STATE throttling{};
        throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof(throttling));
    }

    inline void EnsureOpenMpAllCores()
    {
        thread_local bool initialized = false;
        if (initialized) return;
        initialized = true;

        const int numProcs = std::max(1, omp_get_num_procs());
        omp_set_dynamic(0);
        omp_set_num_threads(numProcs);
        #pragma omp parallel num_threads(numProcs)
        {
            DisableCurrentThreadPowerThrottling();
        }
    }
}
