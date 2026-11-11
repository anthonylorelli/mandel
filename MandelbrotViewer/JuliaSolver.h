#pragma once
#include "FractalConfig.h"
#include "PixelSampling.h"
#include <atomic>
#include <vector>

namespace Mandelbrot
{
    // Computes dynamical-plane Julia set iterations (quadratic Mandelbrot or Burning Ship formula).
    double ComputeJuliaSet(
        const FractalConfig& config,
        std::vector<float>& outIterations,
        std::vector<float>& sampleWeights,
        const std::atomic<bool>* cancelToken = nullptr,
        std::vector<SubpixelSamples>* subpixelSamples = nullptr);
}
