#pragma once
#include "FractalConfig.h"
#include "MandelbrotBla.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace Mandelbrot
{
    struct DeepReferenceState
    {
        double refPosX{ 0.0 };
        double refPosY{ 0.0 };
        int refLen{ 0 };
        double refCxDouble{ 0.0 };
        double refCyDouble{ 0.0 };
        const ComplexOrbitPoint* orbit{ nullptr };
        const detail::BlaTable* mandelbrotBla{ nullptr };
    };

    // High-precision reference orbit generator, cross-frame/band cache, 8x8 candidate
    // search, and Mandelbrot BLA table cache for deep-zoom perturbation rendering.
    class DeepReferenceCache
    {
    public:
        void Prepare(
            const FractalConfig& config,
            ExecutionMode executionMode,
            const std::atomic<bool>* cancelToken = nullptr);

        bool Ensure(
            const FractalConfig& config,
            const BigFloat& centerX,
            const BigFloat& centerY,
            ExecutionMode executionMode,
            DeepReferenceState& outState,
            const std::atomic<bool>* cancelToken = nullptr);

    private:
        bool m_hasCachedOrbit{ false };
        FractalType m_cachedType{ FractalType::Mandelbrot };
        BigFloat m_cachedRefX{};
        BigFloat m_cachedRefY{};
        BigFloat m_cachedEndZx{};
        BigFloat m_cachedEndZy{};
        int m_cachedActiveLimbs{ 0 };
        int m_cachedRefLen{ 0 };
        int m_cachedMaxIter{ 0 };
        double m_cachedSearchZoom{ 0.0 };
        bool m_cachedEscaped{ false };
        std::vector<ComplexOrbitPoint> m_cachedOrbit;
        uint64_t m_cachedOrbitVersion{ 0 };
        detail::BlaTable m_cachedMandelbrotBla{};
        uint64_t m_cachedBlaVersion{ 0 };
        int m_cachedBlaRefLen{ 0 };
        double m_cachedBlaCMax{ 0.0 };
    };
}
