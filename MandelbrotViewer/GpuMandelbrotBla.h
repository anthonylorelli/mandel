#pragma once
#include "FractalConfig.h"
#include "GpuSolverTypes.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace Mandelbrot::detail
{
    // Builds single-precision and FloatExp Bivariate Linear Approximation (BLA)
    // skip tables on the CPU for Mandelbrot (z^2 + c) GPU perturbation shaders.
    class GpuMandelbrotBlaBuilder
    {
    public:
        struct BlockD
        {
            double ar{}, ai{}, br{}, bi{}, radius{};
        };

        std::vector<GpuBlaBlockF32> blocksF32;
        std::vector<GpuBlaBlockExp> blocksExp;
        std::array<uint32_t, 64> levelMeta{};
        uint32_t numLevels{ 0 };
        double maxRadD{ 0.0 };
        float maxRadMant{ 0.0f };
        int32_t maxRadExp{ kZeroExpSentinel };
        bool fitsFloat32{ true };

        void Clear() noexcept;
        void Build(const ComplexOrbitPoint* orbit, int refLength, double cMax, const std::atomic<bool>* cancel);
    };
}
