#pragma once
#include "FractalConfig.h"
#include <cstdint>
#include <vector>

namespace Mandelbrot
{
    // Evaluates whether zooming further into (targetX, targetY) is approaching a uniform/solid-color region
    // (such as an all-interior bulb or a flat-gradient exterior area) and a new target spot should be chosen.
    bool ShouldRetargetZoom(
        const FractalConfig& config,
        const std::vector<float>& iterations,
        const BigFloat& targetX,
        const BigFloat& targetY);

    // Randomly picks an interesting fractal boundary/filament coordinate to zoom in on,
    // refined to sub-pixel boundary precision so it stays interesting for as long as possible.
    bool PickInterestingTarget(
        const FractalConfig& config,
        const std::vector<float>& iterations,
        BigFloat& outTargetX,
        BigFloat& outTargetY,
        uint64_t randomSeed = 0);
}
