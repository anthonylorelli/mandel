#pragma once
#include <array>
#include <cstdint>

namespace Mandelbrot
{
    // Sparse, exact samples at (-.25,-.25), (+.25,-.25), (-.25,+.25), (+.25,+.25)
    // relative to a pixel center. Keep iteration values separate until after coloring.
    struct SubpixelSamples
    {
        uint32_t pixelIndex{};
        float weight{};
        std::array<float, 4> iterations{};
    };
}
