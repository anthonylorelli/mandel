#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Mandelbrot::detail
{
    inline constexpr double kGpuDeepExpThreshold = 1e30;
    inline constexpr double kGpuTricornMaxZoom = 1e20;
    inline constexpr size_t kMaxPrimaryChunkPixels = 16ULL * 1024 * 1024;
    inline constexpr size_t kMaxSingleSyncInteractivePixels = 4ULL * 1024 * 1024;
    inline constexpr uint64_t kMaxPrimaryIterBudget = 6'000'000'000ULL;
    inline constexpr size_t kMaxSubpixelBatchPixels = 2ULL * 1024 * 1024;
    inline constexpr size_t kMaxMultiPassSubRays = 4ULL * 1024 * 1024;
    inline constexpr size_t kMaxFullFrameAccumPixels = 16ULL * 1024 * 1024;
    inline constexpr size_t kD3D11SafeMaxBufferBytes = 128ULL * 1024 * 1024;
    inline constexpr uint32_t kIndirectSubpixelSentinel = 0xFFFFFFFFu;
    inline constexpr uint32_t kMovieShutterTapCount = 7;
    inline constexpr size_t kPaletteLutSize = 4096;
    inline constexpr int kZeroExpSentinel = -1073741824;
    inline constexpr uint32_t kMultiPassMinMaxIter = 2048;

    enum class GpuShaderMode
    {
        Shallow,
        PerturbLds,
        PerturbLdsBla,
        PerturbDeepExp
    };

    struct GpuPixelSubInfo
    {
        uint32_t slot;
        float weight;
    };
    static_assert(sizeof(GpuPixelSubInfo) == 8);

    struct GpuPerturbState
    {
        float x, y;
        int32_t exp;
        uint32_t m;
        uint32_t iter;
    };
    static_assert(sizeof(GpuPerturbState) == 20);

    struct GpuBlaBlockF32
    {
        float ar, ai;
        float br, bi;
        float radius;
        float pad0, pad1, pad2;
    };
    static_assert(sizeof(GpuBlaBlockF32) == 32);

    struct GpuBlaBlockExp
    {
        float ar, ai;
        float br, bi;
        float radMant;
        int32_t aExp, bExp, radExp;
    };
    static_assert(sizeof(GpuBlaBlockExp) == 32);

    struct GpuOrbitPointExp
    {
        float rmx, rmy;
        int32_t rExp;
        int32_t pad0;
    };
    static_assert(sizeof(GpuOrbitPointExp) == 16);

    struct GpuCoordExp
    {
        float mant;
        int32_t exp;
    };
    static_assert(sizeof(GpuCoordExp) == 8);

    struct alignas(16) ShallowCb
    {
        uint32_t width, height, maxIter, smoothFlag;
        uint32_t fractalType, juliaFlag, rowOffset, subpixelTotal;
        float juliaCx, juliaCy, bailout2, pad0;
        uint32_t outRowOffset, pad2, pad3, pad4;
    };
    static_assert(sizeof(ShallowCb) == 64);

    struct alignas(16) PerturbLdsCb
    {
        uint32_t width, height, maxIter, refLen;
        uint32_t fractalType, smoothFlag, checkCardioid, needScaleCheck;
        uint32_t rowOffset, subpixelTotal;
        float refCx, refCy;
        uint32_t outRowOffset, pad1, pad2, pad3;
    };
    static_assert(sizeof(PerturbLdsCb) == 64);

    struct alignas(16) PerturbLdsBlaCb
    {
        uint32_t width, height, maxIter, refLen;
        uint32_t smoothFlag, blaNumLevels, rowOffset, subpixelTotal;
        float blaMaxRadius, refCx, refCy, pad0;
        uint32_t checkCardioid, outRowOffset, stageIterLimit, subRayOffset;
    };
    static_assert(sizeof(PerturbLdsBlaCb) == 64);

    struct alignas(16) PerturbDeepExpCb
    {
        uint32_t width, height, maxIter, refLen;
        uint32_t smoothFlag, useBla, blaNumLevels, rowOffset;
        uint32_t subpixelTotal;
        float blaMaxRadMant;
        int32_t blaMaxRadExp;
        float refCx;
        float refCy;
        uint32_t outRowOffset;
        uint32_t stageIterLimit;
        uint32_t subRayOffset;
    };
    static_assert(sizeof(PerturbDeepExpCb) == 64);

    struct alignas(16) PostProcessCb
    {
        uint32_t width, bandHeight, visibleRows, topHalo;
        uint32_t destRowOffset, fractalType, smoothFlag, invertFlag;
        float frequency, phaseOffset;
        uint32_t interiorColor, avgRgb;
        uint32_t subGroupThreads, subRayOffset, subRayLimit, pad2;
    };
    static_assert(sizeof(PostProcessCb) == 64);

    struct GpuShutterTap
    {
        float shiftX, shiftY, scale, pad;
    };
    static_assert(sizeof(GpuShutterTap) == 16);

    struct alignas(16) ShutterCb
    {
        uint32_t width, height, tapCount, pad0;
        GpuShutterTap taps[kMovieShutterTapCount];
    };
    static_assert(sizeof(ShutterCb) == 128);

    inline void SplitFrexpSigned(double v, int extraExp, float& mOut, int32_t& expOut) noexcept
    {
        if (v == 0.0 || !std::isfinite(v))
        {
            mOut = 0.0f;
            expOut = kZeroExpSentinel;
            return;
        }
        int e = 0;
        float m = static_cast<float>(std::frexp(v, &e));
        if (std::abs(m) >= 1.0f)
        {
            m *= 0.5f;
            ++e;
        }
        mOut = m;
        expOut = static_cast<int32_t>(e + extraExp);
    }

    inline void SplitFrexpPositive(double r, float& mOut, int32_t& expOut) noexcept
    {
        if (!(r > 0.0) || !std::isfinite(r))
        {
            mOut = 0.0f;
            expOut = kZeroExpSentinel;
            return;
        }
        int e = 0;
        float m = static_cast<float>(std::frexp(r, &e));
        if (m >= 1.0f)
        {
            m *= 0.5f;
            ++e;
        }
        mOut = m;
        expOut = static_cast<int32_t>(e);
    }

    inline void SplitFrexp2(double x, double y, float& mxOut, float& myOut, int32_t& expOut) noexcept
    {
        const double maxAbs = std::max(std::abs(x), std::abs(y));
        if (!(maxAbs > 0.0) || !std::isfinite(maxAbs))
        {
            mxOut = 0.0f;
            myOut = 0.0f;
            expOut = kZeroExpSentinel;
            return;
        }
        int e = 0;
        std::frexp(maxAbs, &e);
        float mx = static_cast<float>(std::ldexp(x, -e));
        float my = static_cast<float>(std::ldexp(y, -e));
        if (std::max(std::abs(mx), std::abs(my)) >= 1.0f)
        {
            mx *= 0.5f;
            my *= 0.5f;
            ++e;
        }
        mxOut = mx;
        myOut = my;
        expOut = static_cast<int32_t>(e);
    }
}
