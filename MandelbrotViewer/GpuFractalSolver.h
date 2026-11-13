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

    struct GpuMovieColorParams
    {
        const uint32_t* lutData{ nullptr }; // ColorTheme::LUT_SIZE (4096) BGRA entries
        uint32_t avgRgb{ 0 };
        float frequency{ 1.0f };
        float phaseOffset{ 0.0f };
        bool invert{ false };
        uint32_t interiorColor{ 0xFF000000 };
    };

    // DirectCompute (D3D11 cs_5_0) fractal solver supporting shallow FP32 evaluation,
    // high-occupancy LDS perturbation, single-precision and FloatExp Bivariate Linear
    // Approximation (BLA) skipping, batched 4x subpixel boundary refinement, and fused
    // GPU supersampled palette coloring + linear-light temporal accumulation.
    class GpuFractalSolver
    {
    public:
        explicit GpuFractalSolver(bool allowSoftwareAdapter = false);
        ~GpuFractalSolver();

        GpuFractalSolver(const GpuFractalSolver&) = delete;
        GpuFractalSolver& operator=(const GpuFractalSolver&) = delete;

        bool IsAvailable() const noexcept;
        bool IsUsingSoftwareAdapter() const noexcept;
        bool CanSupportConfig(const FractalConfig& config) const noexcept;

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

        // Fused GPU-resident movie frame pipeline: keeps iteration, boundary detection,
        // DispatchIndirect 4x subpixel evaluation, palette coloring, and 2x linear-light
        // temporal shutter blending entirely on the GPU with a single final BGRA readback.
        bool BeginMovieFrame(int width, int height);

        bool RenderMovieBand(
            const FractalConfig& bandConfig,
            const BigFloat& centerX,
            const BigFloat& centerY,
            DeepReferenceCache& referenceCache,
            int topHalo,
            int visibleRows,
            int destRowOffset,
            int temporalStep,
            const GpuMovieColorParams& colorParams,
            uint32_t* ioBandPixels,
            const std::atomic<bool>* cancelToken);

        bool EndMovieFrame(int width, int height, uint32_t* outFramePixels);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
