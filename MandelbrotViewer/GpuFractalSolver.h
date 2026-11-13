#pragma once
#include "DeepReferenceCache.h"
#include "FractalConfig.h"
#include "PixelSampling.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace Mandelbrot
{
    inline constexpr double GpuPerturbationThreshold = 16.0;

    // DirectCompute (D3D11 cs_5_0) fractal solver supporting shallow FP32 evaluation,
    // high-occupancy LDS perturbation, single-precision and FloatExp Bivariate Linear
    // Approximation (BLA) skipping, and batched 4x subpixel boundary refinement.
    class GpuFractalSolver
    {
    public:
        explicit GpuFractalSolver(bool allowSoftwareAdapter = false);
        ~GpuFractalSolver();

        GpuFractalSolver(const GpuFractalSolver&) = delete;
        GpuFractalSolver& operator=(const GpuFractalSolver&) = delete;

        bool IsAvailable() const noexcept;
        bool IsUsingSoftwareAdapter() const noexcept;

        // Pre-computes and uploads the full-frame reference orbit and GPU BLA table
        // so horizontal render bands share identical skip radii without re-uploading.
        void PrepareReference(
            const FractalConfig& config,
            DeepReferenceCache& referenceCache,
            const std::atomic<bool>* cancelToken = nullptr);

        // Returns true if the frame was rendered on the GPU (or canceled cleanly while doing so).
        // Returns false if the configuration should fall back to the CPU solver.
        bool TryCompute(
            const FractalConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            DeepReferenceCache& referenceCache,
            std::vector<float>& sampleWeights,
            std::vector<float>& outIterations,
            const std::atomic<bool>* cancelToken,
            std::vector<SubpixelSamples>* subpixelSamples,
            double& outElapsedMs);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
