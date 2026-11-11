#pragma once
#include "DeepReferenceCache.h"
#include "FractalConfig.h"
#include "PixelSampling.h"
#include <atomic>
#include <vector>

namespace Mandelbrot
{
    // High-precision Perturbation Theory & Dynamic Rebasing solver for deep zooms across all fractal types.
    double ComputeDeepZoomFrame(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        ExecutionMode executionMode,
        DeepReferenceCache& referenceCache,
        std::vector<float>& sampleWeights,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken = nullptr,
        std::vector<SubpixelSamples>* subpixelSamples = nullptr);
}
