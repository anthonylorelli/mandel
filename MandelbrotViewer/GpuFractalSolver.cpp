#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "GpuFractalSolver.h"
#include "RenderPass.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <omp.h>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace Mandelbrot
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr double kGpuDeepExpThreshold = 1e32;
        constexpr double kGpuTricornMaxZoom = 1e20;
        constexpr size_t kMaxPrimaryChunkPixels = 16ULL * 1024 * 1024;
        constexpr uint64_t kMaxPrimaryIterBudget = 6'000'000'000ULL;
        constexpr size_t kMaxSubpixelBatchPixels = 2ULL * 1024 * 1024;
        constexpr size_t kD3D11SafeMaxBufferBytes = 128ULL * 1024 * 1024;
        constexpr int kZeroExpSentinel = -1073741824;

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
            float rx, ry;
            float rmx, rmy;
            int32_t rExp;
            int32_t pad0, pad1, pad2;
        };
        static_assert(sizeof(GpuOrbitPointExp) == 32);

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
            uint32_t pad1, pad2, pad3, pad4;
        };
        static_assert(sizeof(ShallowCb) == 64);

        struct alignas(16) PerturbLdsCb
        {
            uint32_t width, height, maxIter, refLen;
            uint32_t fractalType, smoothFlag, checkCardioid, needScaleCheck;
            uint32_t rowOffset, subpixelTotal;
            float refCx, refCy;
            uint32_t pad0, pad1, pad2, pad3;
        };
        static_assert(sizeof(PerturbLdsCb) == 64);

        struct alignas(16) PerturbLdsBlaCb
        {
            uint32_t width, height, maxIter, refLen;
            uint32_t smoothFlag, blaNumLevels, rowOffset, subpixelTotal;
            float blaMaxRadius, refCx, refCy, pad0;
            uint32_t checkCardioid, pad2, pad3, pad4;
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
            float refCy, pad0, pad1, pad2;
        };
        static_assert(sizeof(PerturbDeepExpCb) == 64);

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

        class GpuBlaBuilder
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

            void Clear() noexcept
            {
                blocksF32.clear();
                blocksExp.clear();
                levelMeta.fill(0);
                numLevels = 0;
                maxRadD = 0.0;
                maxRadMant = 0.0f;
                maxRadExp = kZeroExpSentinel;
                fitsFloat32 = true;
            }

            void Build(const ComplexOrbitPoint* orbit, int refLength, double cMax, const std::atomic<bool>* cancel)
            {
                Clear();
                if (!orbit || refLength <= 8 || !(cMax > 0.0) || !std::isfinite(cMax))
                {
                    return;
                }
                auto canceled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
                if (canceled()) return;

                constexpr double epsTol = 1e-8;
                auto single = [&](double x, double y) noexcept -> BlockD
                {
                    const double zAbs = std::hypot(x, y);
                    BlockD b;
                    b.ar = 2.0 * x;
                    b.ai = 2.0 * y;
                    b.br = 1.0;
                    b.bi = 0.0;
                    b.radius = std::max(0.0, std::min({ epsTol * 2.0 * zAbs, zAbs * 0.125, (4.0 - zAbs) * 0.125 }));
                    return b;
                };
                auto merge = [&](const BlockD& x, const BlockD& y) noexcept -> BlockD
                {
                    BlockD m{};
                    const double ax = std::hypot(x.ar, x.ai);
                    const double bx = std::hypot(x.br, x.bi);
                    if (!(x.radius > 0.0 && y.radius > 0.0 && ax > 0.0) || !std::isfinite(ax + bx))
                    {
                        return m;
                    }
                    m.ar = y.ar * x.ar - y.ai * x.ai;
                    m.ai = y.ar * x.ai + y.ai * x.ar;
                    m.br = (y.ar * x.br - y.ai * x.bi) + y.br;
                    m.bi = (y.ar * x.bi + y.ai * x.br) + y.bi;
                    const double allowance = y.radius - bx * cMax;
                    if (allowance > 0.0 &&
                        std::isfinite(m.ar) && std::isfinite(m.ai) &&
                        std::isfinite(m.br) && std::isfinite(m.bi))
                    {
                        m.radius = 0.5 * std::min(x.radius, allowance / ax);
                    }
                    return m;
                };

                BlockD init[8];
                for (int i = 0; i < 8; ++i)
                {
                    init[i] = single(orbit[i + 1].x, orbit[i + 1].y);
                }
                for (int cnt = 8; cnt > 1; cnt >>= 1)
                {
                    for (int i = 0; i < cnt / 2; ++i)
                    {
                        init[i] = merge(init[2 * i], init[2 * i + 1]);
                    }
                }
                if (!(init[0].radius > cMax))
                {
                    return;
                }

                const size_t l2Count = static_cast<size_t>(refLength - 1) / 4;
                if (l2Count == 0) return;

                std::vector<std::vector<BlockD>> levels(3);
                levels[2].resize(l2Count);
                double localMaxRad = 0.0;
                for (size_t i = 0; i < l2Count; ++i)
                {
                    if ((i & 255) == 0 && canceled()) return;
                    const int m = static_cast<int>(i * 4) + 1;
                    const BlockD b0 = merge(single(orbit[m].x, orbit[m].y), single(orbit[m + 1].x, orbit[m + 1].y));
                    const BlockD b1 = merge(single(orbit[m + 2].x, orbit[m + 2].y), single(orbit[m + 3].x, orbit[m + 3].y));
                    levels[2][i] = merge(b0, b1);
                    localMaxRad = std::max(localMaxRad, levels[2][i].radius);
                }

                while (levels.back().size() >= 2 && levels.size() < 31)
                {
                    if (canceled()) return;
                    const auto& prev = levels.back();
                    std::vector<BlockD> next(prev.size() / 2);
                    for (size_t i = 0; i < next.size(); ++i)
                    {
                        next[i] = merge(prev[2 * i], prev[2 * i + 1]);
                        localMaxRad = std::max(localMaxRad, next[i].radius);
                    }
                    levels.push_back(std::move(next));
                }
                if (canceled() || !(localMaxRad > 0.0)) return;

                maxRadD = localMaxRad;
                SplitFrexpPositive(maxRadD, maxRadMant, maxRadExp);
                numLevels = static_cast<uint32_t>(levels.size());

                size_t totalBlocks = 0;
                for (uint32_t lv = 2; lv < numLevels; ++lv)
                {
                    totalBlocks += levels[lv].size();
                }
                blocksF32.reserve(totalBlocks);
                blocksExp.reserve(totalBlocks);

                for (uint32_t lv = 2; lv < numLevels; ++lv)
                {
                    levelMeta[lv * 2 + 0] = static_cast<uint32_t>(blocksExp.size());
                    levelMeta[lv * 2 + 1] = static_cast<uint32_t>(levels[lv].size());
                    for (const auto& b : levels[lv])
                    {
                        const double maxCoeff = std::max({ std::abs(b.ar), std::abs(b.ai), std::abs(b.br), std::abs(b.bi) });
                        if (maxCoeff > 1e34 || (b.radius > 0.0 && b.radius < 1e-35))
                        {
                            fitsFloat32 = false;
                        }

                        GpuBlaBlockF32 bf{};
                        bf.ar = static_cast<float>(b.ar);
                        bf.ai = static_cast<float>(b.ai);
                        bf.br = static_cast<float>(b.br);
                        bf.bi = static_cast<float>(b.bi);
                        bf.radius = (maxCoeff <= 1e34 && b.radius >= 1e-35) ? static_cast<float>(b.radius) : 0.0f;
                        blocksF32.push_back(bf);

                        GpuBlaBlockExp be{};
                        SplitFrexp2(b.ar, b.ai, be.ar, be.ai, be.aExp);
                        SplitFrexp2(b.br, b.bi, be.br, be.bi, be.bExp);
                        SplitFrexpPositive(b.radius, be.radMant, be.radExp);
                        blocksExp.push_back(be);
                    }
                }
            }
        };

        // Shader 1: Direct FP32 evaluation for shallow zooms (zoom < 16.0), all 8 FractalTypes + Julia.
        static const char* kShallowShaderSrc = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint smoothFlag;
    uint fractalType;
    uint juliaFlag;
    uint rowOffset;
    uint subpixelTotal;
    float juliaCx;
    float juliaCy;
    float bailout2;
    float pad0;
    uint pad1, pad2, pad3, pad4;
};

Buffer<float> ColCoords : register(t0);
Buffer<float> RowCoords : register(t1);
Buffer<uint> SubpixelIndices : register(t4);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

float FinishSmoothEscape(uint fType, uint iter, float zr, float zi, float prevZr, float prevZi, float cx, float cy)
{
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    uint sIter = iter;
    float maxExtraMod2 = (fType == 5u) ? 1e11f : 1e18f;

    [loop]
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < maxExtraMod2; ++extra)
    {
        if (fType == 0u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 2u)
        {
            zi = -2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 3u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fType == 4u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fType == 5u)
        {
            float nextZr = zr * (zr2 - 3.0f * zi2) + cx;
            zi = zi * (3.0f * zr2 - zi2) + cy;
            zr = nextZr;
        }
        else
        {
            float nextZr = zr2 - zi2 + 0.5667f - 0.5f * prevZr;
            float nextZi = 2.0f * zr * zi - 0.5f * prevZi;
            prevZr = zr;
            prevZi = zi;
            zr = nextZr;
            zi = nextZi;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }

    float logZn = log(zr2 + zi2) * 0.5f;
    float nu = (fType == 5u)
        ? (float(sIter) + 1.0f - log(logZn) * 0.9102392266f)
        : (float(sIter) + 1.0f - log2(logZn));
    return max(0.0f, nu);
}

float EvalShallowSample(float xCoord, float yCoord)
{
    if (juliaFlag != 0u)
    {
        float zr = xCoord;
        float zi = yCoord;
        float jcx = juliaCx;
        float jcy = juliaCy;
        if (jcx == 0.0f && jcy == 0.0f && (zr * zr + zi * zi) <= 1.0f)
            return -1.0f;

        float chkZr = zr, chkZi = zi;
        uint chkPeriod = 8u, stepsSince = 0u;
        [loop]
        for (uint iter = 0u; iter < maxIter; ++iter)
        {
            float zr2 = zr * zr;
            float zi2 = zi * zi;
            if (zr2 + zi2 > bailout2)
            {
                if (smoothFlag == 0u) return float(iter);
                return FinishSmoothEscape(fractalType, iter, zr, zi, 0.0f, 0.0f, jcx, jcy);
            }
            float nextR = zr2 - zi2 + jcx;
            zi = (fractalType == 1u) ? (-2.0f * abs(zr * zi) + jcy) : (2.0f * zr * zi + jcy);
            zr = nextR;
            if (zr == chkZr && zi == chkZi) return -1.0f;
            if (++stepsSince == chkPeriod)
            {
                chkZr = zr; chkZi = zi; stepsSince = 0u;
                if (chkPeriod < 1024u) chkPeriod <<= 1u;
            }
        }
        return -1.0f;
    }

    float cx = xCoord;
    float cy = yCoord;

    if (fractalType == 0u)
    {
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f)
            return -1.0f;
    }
    else if (fractalType == 7u)
    {
        float zr = cx;
        float zi = cy;
        uint iter = 0u;
        int rootIndex = -1;
        float distSq = 0.0f;
        const float ROOT1_Y = 0.8660254038f;
        const float TOL_SQ = 1e-8f;

        [loop]
        while (iter < maxIter)
        {
            float zr2 = zr * zr;
            float zi2 = zi * zi;
            float r2 = zr2 + zi2;
            if (r2 < 1e-36f) break;

            float d0 = (zr - 1.0f) * (zr - 1.0f) + zi2;
            if (d0 < TOL_SQ) { rootIndex = 0; distSq = d0; break; }

            float d1 = (zr + 0.5f) * (zr + 0.5f) + (zi - ROOT1_Y) * (zi - ROOT1_Y);
            if (d1 < TOL_SQ) { rootIndex = 1; distSq = d1; break; }

            float d2 = (zr + 0.5f) * (zr + 0.5f) + (zi + ROOT1_Y) * (zi + ROOT1_Y);
            if (d2 < TOL_SQ) { rootIndex = 2; distSq = d2; break; }

            float invR2 = 1.0f / r2;
            float nextZr = (2.0f / 3.0f) * zr + (1.0f / 3.0f) * ((zr2 - zi2) * invR2) * invR2;
            float nextZi = (2.0f / 3.0f) * zi - (1.0f / 3.0f) * ((2.0f * zr * zi) * invR2) * invR2;
            zr = nextZr;
            zi = nextZi;
            ++iter;
        }
        if (rootIndex < 0 || iter >= maxIter) return -1.0f;
        float smoothVal = float(iter);
        if (smoothFlag != 0u)
        {
            float dist = max(sqrt(distSq), 1e-12f);
            float ratio = log(dist) / (-9.21034037f);
            if (ratio > 1.0f)
            {
                smoothVal = max(0.0f, float(iter) - log2(ratio));
            }
        }
        return float(rootIndex) * 1000000.0f + smoothVal;
    }

    if (fractalType == 0u)
    {
        float zr = 0.0f, zi = 0.0f, zr2 = 0.0f, zi2 = 0.0f;
        float chkZr = 0.0f, chkZi = 0.0f;
        uint chkPeriod = 8u, stepsSince = 0u, iter = 0u;

        [loop]
        while (iter < maxIter && (zr2 + zi2) <= 16.0f)
        {
            zi = 2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
            zr2 = zr * zr;
            zi2 = zi * zi;
            ++iter;
            if (zr == chkZr && zi == chkZi)
            {
                return -1.0f;
            }
            if (++stepsSince == chkPeriod)
            {
                chkZr = zr; chkZi = zi;
                stepsSince = 0u;
                if (chkPeriod < 1024u) chkPeriod <<= 1u;
            }
        }
        if (iter >= maxIter) return -1.0f;
        if (smoothFlag == 0u) return float(iter);
        return FinishSmoothEscape(0u, iter, zr, zi, 0.0f, 0.0f, cx, cy);
    }

    float zr = (fractalType == 6u) ? cy : 0.0f;
    float zi = (fractalType == 6u) ? cx : 0.0f;
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    float prevZr = 0.0f, prevZi = 0.0f;
    float chkZr = 0.0f, chkZi = 0.0f, chkPrevZr = 0.0f, chkPrevZi = 0.0f;
    uint chkPeriod = 8u, stepsSince = 0u, iter = 0u;

    [loop]
    while (iter < maxIter && (zr2 + zi2) <= 16.0f)
    {
        if (fractalType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fractalType == 2u)
        {
            zi = -2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fractalType == 3u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fractalType == 4u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fractalType == 5u)
        {
            float nextZr = zr * (zr2 - 3.0f * zi2) + cx;
            zi = zi * (3.0f * zr2 - zi2) + cy;
            zr = nextZr;
        }
        else
        {
            float nextZr = zr2 - zi2 + 0.5667f - 0.5f * prevZr;
            float nextZi = 2.0f * zr * zi - 0.5f * prevZi;
            prevZr = zr;
            prevZi = zi;
            zr = nextZr;
            zi = nextZi;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++iter;
        if (zr == chkZr && zi == chkZi && (fractalType != 6u || (prevZr == chkPrevZr && prevZi == chkPrevZi)))
        {
            iter = maxIter;
            break;
        }
        if (++stepsSince == chkPeriod)
        {
            chkZr = zr; chkZi = zi;
            chkPrevZr = prevZr; chkPrevZi = prevZi;
            stepsSince = 0u;
            if (chkPeriod < 1024u) chkPeriod <<= 1u;
        }
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);
    return FinishSmoothEscape(fractalType, iter, zr, zi, prevZr, prevZi, cx, cy);
}

[numthreads(16, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID)
{
    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    float xc = ColCoords[tid.x * 3u + 1u];
    float yc = RowCoords[py * 3u + 1u];
    OutIters[tid.y * width + tid.x] = EvalShallowSample(xc, yc);
}

[numthreads(256, 1, 1)]
void CSSubpixels(uint3 tid : SV_DispatchThreadID)
{
    uint sIdx = tid.x;
    if (sIdx >= subpixelTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalShallowSample(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky]);
}
)";

        // Shader 2: High-occupancy 512-thread LDS Perturbation (16.0 <= zoom < 1e32, without BLA).
        static const char* kPerturbLdsShaderSrc = R"(
#define LDS_ORBIT_SIZE 2048u

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint fractalType;
    uint smoothFlag;
    uint checkCardioid;
    uint needScaleCheck;
    uint rowOffset;
    uint subpixelTotal;
    float refCx;
    float refCy;
    uint pad0, pad1, pad2, pad3;
};

Buffer<float2> OrbitFast : register(t0);
Buffer<float> ColCoords : register(t1);
Buffer<float> RowCoords : register(t2);
Buffer<uint> SubpixelIndices : register(t4);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

groupshared float2 s_orbit[LDS_ORBIT_SIZE];

float DiffAbsF(float A, float a)
{
    float sum = A + a;
    if ((A >= 0.0f && sum >= 0.0f) || (A < 0.0f && sum < 0.0f))
        return (A >= 0.0f) ? a : -a;
    return abs(sum) - abs(A);
}

bool ShouldRebaseScaled(float rx, float ry, float dx, float dy)
{
    float maxD = max(abs(dx), abs(dy));
    if (!(maxD > 0.0f) || max(abs(rx), abs(ry)) >= 3.0f * maxD)
        return false;
    float s = (maxD < 1e-15f) ? 1.0842022e19f : 1.0f;
    float szx = rx * s + dx * s;
    float szy = ry * s + dy * s;
    float sdx = dx * s;
    float sdy = dy * s;
    return (szx * szx + szy * szy) < (sdx * sdx + sdy * sdy);
}

float FinishPerturbSmooth(uint fType, uint iter, float zr, float zi, float cxApprox, float cyApprox)
{
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    uint sIter = iter;

    [loop]
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        if (fType == 0u)
        {
            zi = 2.0f * zr * zi + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else if (fType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else if (fType == 2u)
        {
            zi = -2.0f * zr * zi + cyApprox;
            zr = zr2 - zi2 + cxApprox;
        }
        else
        {
            zi = -2.0f * abs(zr * zi) + cyApprox;
            zr = abs(zr2 - zi2) + cxApprox;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }

    float logZn = log(zr2 + zi2) * 0.5f;
    return max(0.0f, float(sIter) + 1.0f - log2(logZn));
}

float EvalPerturbLds(float dcx, float dcy, uint sharedLim)
{
    if (checkCardioid != 0u)
    {
        float cx = refCx + dcx;
        float cy = refCy + dcy;
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 - 2e-6f || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f - 2e-6f)
            return -1.0f;
    }

    float dx = 0.0f, dy = 0.0f;
    uint m = 0u;
    uint iter = 0u;
    float escZx = 0.0f, escZy = 0.0f;
    uint ldsStop = min(refLen, LDS_ORBIT_SIZE);

    if (fractalType == 0u)
    {
        [loop]
        while (iter < maxIter)
        {
            if (needScaleCheck == 0u && m > 0u && m < ldsStop)
            {
                uint endIter = iter + min(maxIter - iter, ldsStop - m);
                [loop]
                while (iter < endIter)
                {
                    float2 r = s_orbit[m];
                    float zx = r.x + dx;
                    float zy = r.y + dy;
                    float mod2 = zx * zx + zy * zy;
                    float dz2 = dx * dx + dy * dy;
                    if (mod2 > 16.0f || mod2 < dz2) break;
                    float sx = r.x + zx;
                    float sy = r.y + zy;
                    float nextDx = sx * dx - sy * dy + dcx;
                    dy = sx * dy + sy * dx + dcy;
                    dx = nextDx;
                    ++m;
                    ++iter;
                }
                if (iter >= maxIter) break;
            }

            float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
            float rx = r.x, ry = r.y;
            float zx = rx + dx;
            float zy = ry + dy;
            float mod2 = zx * zx + zy * zy;
            if (mod2 > 16.0f) { escZx = zx; escZy = zy; break; }

            bool doRebase = (m == refLen);
            if (!doRebase && m > 0u)
            {
                doRebase = (needScaleCheck != 0u)
                    ? ShouldRebaseScaled(rx, ry, dx, dy)
                    : (mod2 < (dx * dx + dy * dy));
            }
            if (doRebase)
            {
                dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            }
            float sx = rx + zx;
            float sy = ry + zy;
            float nextDx = sx * dx - sy * dy + dcx;
            dy = sx * dy + sy * dx + dcy;
            dx = nextDx;
            ++m;
            ++iter;
        }
    }
    else
    {
        [loop]
        while (iter < maxIter)
        {
            if (needScaleCheck == 0u && m > 0u && m < ldsStop)
            {
                uint endIter = iter + min(maxIter - iter, ldsStop - m);
                if (fractalType == 1u)
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float daX = DiffAbsF(r.x, dx), daY = DiffAbsF(r.y, dy);
                        float nextDx = sx * dx - sy * dy + dcx;
                        dy = -2.0f * (abs(r.x) * daY + abs(r.y) * daX + daX * daY) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                else if (fractalType == 2u)
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float nextDx = sx * dx - sy * dy + dcx;
                        dy = -(sx * dy + sy * dx) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                else
                {
                    [loop]
                    while (iter < endIter)
                    {
                        float2 r = s_orbit[m];
                        float zx = r.x + dx, zy = r.y + dy;
                        float mod2 = zx * zx + zy * zy;
                        if (mod2 > 16.0f || mod2 < (dx * dx + dy * dy)) break;
                        float sx = r.x + zx, sy = r.y + zy;
                        float daX = DiffAbsF(r.x, dx), daY = DiffAbsF(r.y, dy);
                        float nextDx = DiffAbsF((r.x - r.y) * (r.x + r.y), sx * dx - sy * dy) + dcx;
                        dy = -2.0f * (abs(r.x) * daY + abs(r.y) * daX + daX * daY) + dcy;
                        dx = nextDx;
                        ++m; ++iter;
                    }
                }
                if (iter >= maxIter) break;
            }

            float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
            float rx = r.x, ry = r.y;
            float zx = rx + dx;
            float zy = ry + dy;
            float mod2 = zx * zx + zy * zy;
            if (mod2 > 16.0f) { escZx = zx; escZy = zy; break; }

            bool doRebase = (m == refLen);
            if (!doRebase && m > 0u)
            {
                doRebase = (needScaleCheck != 0u)
                    ? ShouldRebaseScaled(rx, ry, dx, dy)
                    : (mod2 < (dx * dx + dy * dy));
            }
            if (doRebase)
            {
                dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            }

            float sx = rx + zx;
            float sy = ry + zy;
            if (fractalType == 1u)
            {
                float daX = DiffAbsF(rx, dx);
                float daY = DiffAbsF(ry, dy);
                float nextDx = sx * dx - sy * dy + dcx;
                dy = -2.0f * (abs(rx) * daY + abs(ry) * daX + daX * daY) + dcy;
                dx = nextDx;
            }
            else if (fractalType == 2u)
            {
                float nextDx = sx * dx - sy * dy + dcx;
                dy = -(sx * dy + sy * dx) + dcy;
                dx = nextDx;
            }
            else
            {
                float daX = DiffAbsF(rx, dx);
                float daY = DiffAbsF(ry, dy);
                float nextDx = DiffAbsF((rx - ry) * (rx + ry), sx * dx - sy * dy) + dcx;
                dy = -2.0f * (abs(rx) * daY + abs(ry) * daX + daX * daY) + dcy;
                dx = nextDx;
            }
            ++m;
            ++iter;
        }
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);
    return FinishPerturbSmooth(fractalType, iter, escZx, escZy, refCx + dcx, refCy + dcy);
}

[numthreads(32, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[tid.y * width + tid.x] = EvalPerturbLds(ColCoords[tid.x * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim);
}

[numthreads(512, 1, 1)]
void CSSubpixels(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint sIdx = tid.x;
    if (sIdx >= subpixelTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalPerturbLds(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky], sharedLim);
}
)";

        // Shader 3: High-occupancy 512-thread LDS Perturbation + float32 BLA (Mandelbrot, zoom < 1e32).
        static const char* kPerturbLdsBlaShaderSrc = R"(
#define LDS_ORBIT_SIZE 2048u

cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint smoothFlag;
    uint blaNumLevels;
    uint rowOffset;
    uint subpixelTotal;
    float blaMaxRadius;
    float refCx;
    float refCy;
    float pad0;
    uint checkCardioid, pad2, pad3, pad4;
};

struct GpuBlaBlockF32
{
    float ar, ai;
    float br, bi;
    float radius;
    float pad0, pad1, pad2;
};

Buffer<float2> OrbitFast : register(t0);
Buffer<float> ColCoords : register(t1);
Buffer<float> RowCoords : register(t2);
StructuredBuffer<GpuBlaBlockF32> BlaBlocks : register(t3);
StructuredBuffer<uint2> BlaMeta : register(t4);
Buffer<uint> SubpixelIndices : register(t5);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

groupshared float2 s_orbit[LDS_ORBIT_SIZE];

float EvalPerturbLdsBla(float dcx, float dcy, uint sharedLim)
{
    if (checkCardioid != 0u)
    {
        float cx = refCx + dcx;
        float cy = refCy + dcy;
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 - 2e-6f || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f - 2e-6f)
            return -1.0f;
    }

    float dx = 0.0f, dy = 0.0f;
    uint m = 0u;
    uint iter = 0u;
    float escZx = 0.0f, escZy = 0.0f;
    uint ldsStop = min(refLen, LDS_ORBIT_SIZE);
    float blaBurstFloor = max(blaMaxRadius, 1e-15f);

    [loop]
    while (iter < maxIter)
    {
        float maxD = max(abs(dx), abs(dy));
        if (maxD >= blaBurstFloor && m > 0u && m < ldsStop)
        {
            uint endIter = iter + min(maxIter - iter, ldsStop - m);
            [loop]
            while (iter < endIter)
            {
                float2 r = s_orbit[m];
                float zx = r.x + dx;
                float zy = r.y + dy;
                float mod2 = zx * zx + zy * zy;
                float dz2 = dx * dx + dy * dy;
                if (mod2 > 16.0f || mod2 < dz2) break;
                float sx = r.x + zx;
                float sy = r.y + zy;
                float nextDx = sx * dx - sy * dy + dcx;
                dy = sx * dy + sy * dx + dcy;
                dx = nextDx;
                ++m;
                ++iter;
            }
            if (iter >= maxIter) break;
            maxD = max(abs(dx), abs(dy));
        }

        float2 r = (m < sharedLim) ? s_orbit[m] : OrbitFast[m];
        float rx = r.x, ry = r.y;
        float zx = rx + dx;
        float zy = ry + dy;
        float mod2 = zx * zx + zy * zy;
        if (mod2 > 16.0f) { escZx = zx; escZy = zy; break; }

        bool doRebase = (m == refLen);
        if (!doRebase && m > 0u && max(abs(rx), abs(ry)) < 3.0f * maxD && maxD > 0.0f)
        {
            float s = (maxD < 1e-15f) ? 1.0842022e19f : 1.0f;
            float szx = rx * s + dx * s;
            float szy = ry * s + dy * s;
            float sdx = dx * s;
            float sdy = dy * s;
            if (szx * szx + szy * szy < sdx * sdx + sdy * sdy)
                doRebase = true;
        }
        if (doRebase)
        {
            dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
            maxD = max(abs(dx), abs(dy));
        }

        if (m >= 1u && (maxIter - iter) >= 4u && ((m - 1u) & 3u) == 0u && m + 4u <= refLen && maxD < blaMaxRadius)
        {
            uint start = m - 1u;
            uint aligned = (start != 0u) ? firstbitlow(start) : 30u;
            uint top = min(aligned, blaNumLevels - 1u);
            uint skipped = 0u;
            for (int lv = int(top); lv >= 2; --lv)
            {
                uint len = 1u << uint(lv);
                if (len > (maxIter - iter) || m + len > refLen) continue;
                uint2 meta = BlaMeta[lv];
                uint idx = start >> uint(lv);
                if (idx >= meta.y) continue;
                GpuBlaBlockF32 blk = BlaBlocks[meta.x + idx];
                if (!(maxD * 1.41421356f < blk.radius)) continue;
                float nx = (blk.ar * dx - blk.ai * dy) + (blk.br * dcx - blk.bi * dcy);
                float ny = (blk.ar * dy + blk.ai * dx) + (blk.br * dcy + blk.bi * dcx);
                dx = nx; dy = ny;
                skipped = len;
                break;
            }
            if (skipped != 0u)
            {
                m += skipped;
                iter += skipped;
                continue;
            }
        }

        float sx = rx + zx;
        float sy = ry + zy;
        float nextDx = sx * dx - sy * dy + dcx;
        dy = sx * dy + sy * dx + dcy;
        dx = nextDx;
        ++m;
        ++iter;
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);

    float cxApprox = refCx + dcx;
    float cyApprox = refCy + dcy;
    float zr = escZx, zi = escZy;
    float zr2 = zr * zr, zi2 = zi * zi;
    uint sIter = iter;
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        zi = 2.0f * zr * zi + cyApprox;
        zr = zr2 - zi2 + cxApprox;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }
    return max(0.0f, float(sIter) + 1.0f - log2(log(zr2 + zi2) * 0.5f));
}

[numthreads(32, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[tid.y * width + tid.x] = EvalPerturbLdsBla(ColCoords[tid.x * 3u + 1u], RowCoords[py * 3u + 1u], sharedLim);
}

[numthreads(512, 1, 1)]
void CSSubpixels(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint sharedLim = min(refLen + 1u, LDS_ORBIT_SIZE);
    for (uint i = gi; i < sharedLim; i += 512u)
        s_orbit[i] = OrbitFast[i];
    GroupMemoryBarrierWithGroupSync();

    uint sIdx = tid.x;
    if (sIdx >= subpixelTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalPerturbLdsBla(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky], sharedLim);
}
)";

        // Shader 4: Ultra-Deep FloatExp Perturbation + FloatExp BLA (zoom >= 1e32 .. 1e300).
        static const char* kPerturbDeepExpShaderSrc = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint smoothFlag;
    uint useBla;
    uint blaNumLevels;
    uint rowOffset;
    uint subpixelTotal;
    float blaMaxRadMant;
    int blaMaxRadExp;
    float refCx;
    float refCy;
    float pad0, pad1, pad2;
};

struct GpuOrbitPointExp
{
    float rx, ry;
    float rmx, rmy;
    int rExp;
    int pad0, pad1, pad2;
};

struct GpuCoordExp
{
    float mant;
    int exp;
};

struct GpuBlaBlockExp
{
    float ar, ai;
    float br, bi;
    float radMant;
    int aExp, bExp, radExp;
};

Buffer<float2> OrbitFast : register(t0);
StructuredBuffer<GpuOrbitPointExp> OrbitFull : register(t1);
StructuredBuffer<GpuCoordExp> ColCoordsExp : register(t2);
StructuredBuffer<GpuCoordExp> RowCoordsExp : register(t3);
StructuredBuffer<GpuBlaBlockExp> BlaBlocks : register(t4);
StructuredBuffer<uint2> BlaMeta : register(t5);
Buffer<uint> SubpixelIndices : register(t6);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

void Renorm2(inout float mx, inout float my, inout int e)
{
    float a = max(abs(mx), abs(my));
    if (a == 0.0f) { mx = 0.0f; my = 0.0f; e = -1073741824; return; }
    if (a < 1.1754944e-38f)
    {
        mx *= 16777216.0f;
        my *= 16777216.0f;
        e -= 24;
        a = max(abs(mx), abs(my));
    }
    uint bits = asuint(a);
    int biasedExp = int((bits >> 23) & 255u);
    int rawExp = biasedExp - 126;
    int scaleBiased = clamp(253 - biasedExp, 1, 254);
    float scale = asfloat(uint(scaleBiased) << 23);
    mx *= scale;
    my *= scale;
    e += rawExp;
}

void FloatExpAdd(float ax, float ay, int ae, float bx, float by, int be, out float ox, out float oy, out int oe)
{
    if (ae <= -100000000) { ox = bx; oy = by; oe = be; return; }
    if (be <= -100000000) { ox = ax; oy = ay; oe = ae; return; }
    if (ae >= be)
    {
        int diff = ae - be;
        if (diff >= 30) { ox = ax; oy = ay; oe = ae; return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = ax + bx * s; oy = ay + by * s; oe = ae;
    }
    else
    {
        int diff = be - ae;
        if (diff >= 30) { ox = bx; oy = by; oe = be; return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = bx + ax * s; oy = by + ay * s; oe = be;
    }
    Renorm2(ox, oy, oe);
}

bool FloatExpLess(float dmx, float dmy, int de, float radMant, int radExp)
{
    if (radExp <= -100000000) return false;
    if (de <= -100000000) return true;
    int diff = de - radExp;
    if (diff < -1) return true;
    if (diff > 1) return false;
    float s = asfloat(uint(127 + diff) << 23);
    return sqrt(dmx * dmx + dmy * dmy) * s < radMant;
}

float EvalDeepExp(GpuCoordExp cx, GpuCoordExp cy)
{
    float dcMx = 0.0f, dcMy = 0.0f;
    int dcExp = -1073741824;
    if (cx.exp > -100000000 || cy.exp > -100000000)
    {
        dcExp = max(cx.exp, cy.exp);
        dcMx = (cx.exp > -100000000 && (dcExp - cx.exp) < 30)
            ? (cx.mant * asfloat(uint(127 - (dcExp - cx.exp)) << 23))
            : 0.0f;
        dcMy = (cy.exp > -100000000 && (dcExp - cy.exp) < 30)
            ? (cy.mant * asfloat(uint(127 - (dcExp - cy.exp)) << 23))
            : 0.0f;
        Renorm2(dcMx, dcMy, dcExp);
    }

    float dcxF = (dcExp > -126) ? (dcMx * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;
    float dcyF = (dcExp > -126) ? (dcMy * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;

    uint m = 0u;
    uint iter = 0u;
    float escZx = 0.0f, escZy = 0.0f;
    float dmx = 0.0f, dmy = 0.0f;
    int dExp = -1073741824;

    [loop]
    while (iter < maxIter)
    {
        if (dExp >= -45 && dExp <= 10 && m > 0u)
        {
            float initScale = asfloat(uint(127 + dExp) << 23);
            float dx = dmx * initScale;
            float dy = dmy * initScale;
            bool escaped = false;
            [loop]
            while (iter < maxIter)
            {
                float2 r = OrbitFast[m];
                float rx = r.x, ry = r.y;
                float zx = rx + dx;
                float zy = ry + dy;
                float mod2 = zx * zx + zy * zy;
                if (mod2 > 16.0f) { escZx = zx; escZy = zy; escaped = true; break; }
                float dz2 = dx * dx + dy * dy;
                if (m == refLen || (m > 0u && mod2 < dz2))
                {
                    dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
                    if (max(abs(dx), abs(dy)) < 1e-12f)
                    {
                        dmx = dx * 1.0842022e19f; dmy = dy * 1.0842022e19f; dExp = -63;
                        Renorm2(dmx, dmy, dExp);
                        break;
                    }
                }
                float sx = rx + zx;
                float sy = ry + zy;
                float nextDx = sx * dx - sy * dy + dcxF;
                dy = sx * dy + sy * dx + dcyF;
                dx = nextDx;
                ++m;
                ++iter;
                if (max(abs(dx), abs(dy)) < 2.842171e-14f)
                {
                    dmx = dx * 1.0842022e19f; dmy = dy * 1.0842022e19f; dExp = -63;
                    Renorm2(dmx, dmy, dExp);
                    break;
                }
            }
            if (escaped || iter >= maxIter) break;
            continue;
        }

        GpuOrbitPointExp op = OrbitFull[m];
        if (op.rExp >= 2)
        {
            float dScale = (dExp >= -125 && dExp <= 10) ? asfloat(uint(127 + dExp) << 23) : 0.0f;
            float zx = op.rx + dmx * dScale;
            float zy = op.ry + dmy * dScale;
            if (zx * zx + zy * zy > 16.0f) { escZx = zx; escZy = zy; break; }
        }

        bool doRebase = (m == refLen);
        float zmx = 0.0f, zmy = 0.0f;
        int zExp = -1073741824;
        if (doRebase || (m > 0u && op.rExp <= dExp + 2))
        {
            FloatExpAdd(op.rmx, op.rmy, op.rExp, dmx, dmy, dExp, zmx, zmy, zExp);
            if (!doRebase && m > 0u)
            {
                int cExp = max(zExp, dExp);
                if (cExp > -100000000)
                {
                    float zs = (zExp > -100000000 && (cExp - zExp) < 30) ? asfloat(uint(127 - (cExp - zExp)) << 23) : 0.0f;
                    float ds = (dExp > -100000000 && (cExp - dExp) < 30) ? asfloat(uint(127 - (cExp - dExp)) << 23) : 0.0f;
                    float zxS = zmx * zs, zyS = zmy * zs;
                    float dxS = dmx * ds, dyS = dmy * ds;
                    if (zxS * zxS + zyS * zyS < dxS * dxS + dyS * dyS) doRebase = true;
                }
            }
        }
        if (doRebase)
        {
            dmx = zmx; dmy = zmy; dExp = zExp; m = 0u;
            op.rx = 0.0f; op.ry = 0.0f; op.rmx = 0.0f; op.rmy = 0.0f; op.rExp = -1073741824;
        }

        if (useBla != 0u && m >= 1u && (maxIter - iter) >= 4u && ((m - 1u) & 3u) == 0u && m + 4u <= refLen &&
            FloatExpLess(dmx, dmy, dExp, blaMaxRadMant, blaMaxRadExp))
        {
            uint start = m - 1u;
            uint aligned = (start != 0u) ? firstbitlow(start) : 30u;
            uint top = min(aligned, blaNumLevels - 1u);
            uint skipped = 0u;
            for (int lv = int(top); lv >= 2; --lv)
            {
                uint len = 1u << uint(lv);
                if (len > (maxIter - iter) || m + len > refLen) continue;
                uint2 meta = BlaMeta[lv];
                uint idx = start >> uint(lv);
                if (idx >= meta.y) continue;
                GpuBlaBlockExp blk = BlaBlocks[meta.x + idx];
                if (!FloatExpLess(dmx, dmy, dExp, blk.radMant, blk.radExp)) continue;

                float azx = blk.ar * dmx - blk.ai * dmy;
                float azy = blk.ar * dmy + blk.ai * dmx;
                int azExp = (dExp > -100000000) ? (blk.aExp + dExp) : -1073741824;
                Renorm2(azx, azy, azExp);

                float bcx = blk.br * dcMx - blk.bi * dcMy;
                float bcy = blk.br * dcMy + blk.bi * dcMx;
                int bcExp = (dcExp > -100000000) ? (blk.bExp + dcExp) : -1073741824;
                Renorm2(bcx, bcy, bcExp);

                FloatExpAdd(azx, azy, azExp, bcx, bcy, bcExp, dmx, dmy, dExp);
                skipped = len;
                break;
            }
            if (skipped != 0u) { m += skipped; iter += skipped; continue; }
        }

        float smx = dmx, smy = dmy;
        int sExp = dExp;
        if (op.rExp > -100000000)
        {
            FloatExpAdd(op.rmx, op.rmy, op.rExp + 1, dmx, dmy, dExp, smx, smy, sExp);
        }
        float prodX = smx * dmx - smy * dmy;
        float prodY = smx * dmy + smy * dmx;
        int prodExp = (sExp > -100000000 && dExp > -100000000) ? (sExp + dExp) : -1073741824;
        Renorm2(prodX, prodY, prodExp);
        FloatExpAdd(prodX, prodY, prodExp, dcMx, dcMy, dcExp, dmx, dmy, dExp);
        ++m;
        ++iter;
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);

    float zr = escZx, zi = escZy;
    float zr2 = zr * zr, zi2 = zi * zi;
    uint sIter = iter;
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        zi = 2.0f * zr * zi + refCy;
        zr = zr2 - zi2 + refCx;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }
    return max(0.0f, float(sIter) + 1.0f - log2(log(zr2 + zi2) * 0.5f));
}

[numthreads(16, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID)
{
    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[tid.y * width + tid.x] = EvalDeepExp(ColCoordsExp[tid.x * 3u + 1u], RowCoordsExp[py * 3u + 1u]);
}

[numthreads(256, 1, 1)]
void CSSubpixels(uint3 tid : SV_DispatchThreadID)
{
    uint sIdx = tid.x;
    if (sIdx >= subpixelTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalDeepExp(ColCoordsExp[px * 3u + kx], RowCoordsExp[py * 3u + ky]);
}
)";

        struct CompiledShaderCache
        {
            ComPtr<ID3DBlob> shallowPrim, shallowSub;
            ComPtr<ID3DBlob> ldsPrim, ldsSub;
            ComPtr<ID3DBlob> ldsBlaPrim, ldsBlaSub;
            ComPtr<ID3DBlob> deepExpPrim, deepExpSub;
            bool ok{ false };
        };

        const CompiledShaderCache& GetCompiledShaderCache()
        {
            static CompiledShaderCache cache;
            static std::once_flag flag;
            std::call_once(flag, []()
            {
                auto compileOne = [](const char* src, const char* entry, ComPtr<ID3DBlob>& outBlob) -> bool
                {
                    ComPtr<ID3DBlob> err;
                    const HRESULT hr = D3DCompile(
                        src,
                        std::strlen(src),
                        entry,
                        nullptr,
                        nullptr,
                        entry,
                        "cs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3,
                        0,
                        &outBlob,
                        &err);
                    return SUCCEEDED(hr) && outBlob != nullptr;
                };

                cache.ok =
                    compileOne(kShallowShaderSrc, "CSPrimary", cache.shallowPrim) &&
                    compileOne(kShallowShaderSrc, "CSSubpixels", cache.shallowSub) &&
                    compileOne(kPerturbLdsShaderSrc, "CSPrimary", cache.ldsPrim) &&
                    compileOne(kPerturbLdsShaderSrc, "CSSubpixels", cache.ldsSub) &&
                    compileOne(kPerturbLdsBlaShaderSrc, "CSPrimary", cache.ldsBlaPrim) &&
                    compileOne(kPerturbLdsBlaShaderSrc, "CSSubpixels", cache.ldsBlaSub) &&
                    compileOne(kPerturbDeepExpShaderSrc, "CSPrimary", cache.deepExpPrim) &&
                    compileOne(kPerturbDeepExpShaderSrc, "CSSubpixels", cache.deepExpSub);
            });
            return cache;
        }

        size_t GrowCapacityBytes(size_t currentBytes, size_t neededBytes) noexcept
        {
            if (neededBytes <= currentBytes) return currentBytes;
            if (neededBytes >= 64ULL * 1024 * 1024) return neededBytes;
            const size_t grown = std::max(neededBytes, currentBytes + (currentBytes >> 1) + 4096);
            return std::min(grown, kD3D11SafeMaxBufferBytes);
        }
    }

    struct GpuFractalSolver::Impl
    {
        std::mutex mutex;
        bool available{ false };
        bool usingSoftwareAdapter{ false };

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        ComPtr<ID3D11ComputeShader> csShallowPrim, csShallowSub;
        ComPtr<ID3D11ComputeShader> csLdsPrim, csLdsSub;
        ComPtr<ID3D11ComputeShader> csLdsBlaPrim, csLdsBlaSub;
        ComPtr<ID3D11ComputeShader> csDeepExpPrim, csDeepExpSub;

        ComPtr<ID3D11Buffer> constantBuffer;

        ComPtr<ID3D11Buffer> outItersBuf, outItersStaging;
        ComPtr<ID3D11UnorderedAccessView> outItersUav;
        size_t outItersCapElems{ 0 };

        ComPtr<ID3D11Buffer> subIdxBuf;
        ComPtr<ID3D11ShaderResourceView> subIdxSrv;
        size_t subIdxCapElems{ 0 };

        ComPtr<ID3D11Buffer> subOutBuf, subOutStaging;
        ComPtr<ID3D11UnorderedAccessView> subOutUav;
        size_t subOutCapFloats{ 0 };

        ComPtr<ID3D11Buffer> colCoordsBuf, rowCoordsBuf;
        ComPtr<ID3D11ShaderResourceView> colCoordsSrv, rowCoordsSrv;
        size_t colCoordsCapFloats{ 0 }, rowCoordsCapFloats{ 0 };

        ComPtr<ID3D11Buffer> colCoordsExpBuf, rowCoordsExpBuf;
        ComPtr<ID3D11ShaderResourceView> colCoordsExpSrv, rowCoordsExpSrv;
        size_t colCoordsExpCapElems{ 0 }, rowCoordsExpCapElems{ 0 };

        ComPtr<ID3D11Buffer> orbitFastBuf;
        ComPtr<ID3D11ShaderResourceView> orbitFastSrv;
        size_t orbitFastCapElems{ 0 };

        ComPtr<ID3D11Buffer> orbitFullBuf;
        ComPtr<ID3D11ShaderResourceView> orbitFullSrv;
        size_t orbitFullCapElems{ 0 };

        ComPtr<ID3D11Buffer> blaF32Buf;
        ComPtr<ID3D11ShaderResourceView> blaF32Srv;
        size_t blaF32CapElems{ 0 };

        ComPtr<ID3D11Buffer> blaExpBuf;
        ComPtr<ID3D11ShaderResourceView> blaExpSrv;
        size_t blaExpCapElems{ 0 };

        ComPtr<ID3D11Buffer> blaMetaBuf;
        ComPtr<ID3D11ShaderResourceView> blaMetaSrv;

        // Cached orbit & BLA state uploaded to GPU
        uint64_t uploadedOrbitVersion{ 0 };
        int uploadedRefLen{ -1 };
        FractalType uploadedType{ FractalType::Mandelbrot };
        bool uploadedHasDeepExpOrbit{ false };

        uint64_t uploadedBlaOrbitVersion{ 0 };
        int uploadedBlaRefLen{ -1 };
        double uploadedBlaCMax{ 0.0 };
        bool uploadedHasBlaF32{ false };
        bool uploadedHasBlaExp{ false };
        GpuBlaBuilder blaBuilder;

        // Reusable host staging vectors to avoid heap churn and WC memory stalls
        std::vector<float> hostColCoords, hostRowCoords;
        std::vector<GpuCoordExp> hostColCoordsExp, hostRowCoordsExp;
        std::vector<float> hostOrbitFast;
        std::vector<GpuOrbitPointExp> hostOrbitExp;
        std::vector<uint32_t> hostSlotOfPixel;
        std::vector<uint32_t> hostTiledPixels;
        std::vector<uint32_t> hostTiledSlots;
        std::vector<float> hostSubFloats;

        explicit Impl(bool allowSoftwareAdapter)
        {
            const auto& shaders = GetCompiledShaderCache();
            if (!shaders.ok) return;

            const D3D_FEATURE_LEVEL featureLevels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            D3D_FEATURE_LEVEL flOut{};

            ComPtr<IDXGIFactory1> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory)
            {
                for (UINT idx = 0;; ++idx)
                {
                    ComPtr<IDXGIAdapter1> adapter;
                    if (factory->EnumAdapters1(idx, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                    DXGI_ADAPTER_DESC1 desc{};
                    if (FAILED(adapter->GetDesc1(&desc))) continue;
                    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

                    if (SUCCEEDED(D3D11CreateDevice(
                        adapter.Get(),
                        D3D_DRIVER_TYPE_UNKNOWN,
                        nullptr,
                        D3D11_CREATE_DEVICE_SINGLETHREADED,
                        featureLevels,
                        static_cast<UINT>(std::size(featureLevels)),
                        D3D11_SDK_VERSION,
                        &device,
                        &flOut,
                        &context)))
                    {
                        usingSoftwareAdapter = false;
                        break;
                    }
                }
            }

            if (!device && allowSoftwareAdapter)
            {
                if (SUCCEEDED(D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    D3D11_CREATE_DEVICE_SINGLETHREADED,
                    featureLevels,
                    static_cast<UINT>(std::size(featureLevels)),
                    D3D11_SDK_VERSION,
                    &device,
                    &flOut,
                    &context)))
                {
                    usingSoftwareAdapter = true;
                }
            }

            if (!device || !context) return;

            auto makeShader = [&](const ComPtr<ID3DBlob>& blob, ComPtr<ID3D11ComputeShader>& outCs) -> bool
            {
                return SUCCEEDED(device->CreateComputeShader(
                    blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &outCs)) && outCs != nullptr;
            };

            if (!makeShader(shaders.shallowPrim, csShallowPrim) ||
                !makeShader(shaders.shallowSub, csShallowSub) ||
                !makeShader(shaders.ldsPrim, csLdsPrim) ||
                !makeShader(shaders.ldsSub, csLdsSub) ||
                !makeShader(shaders.ldsBlaPrim, csLdsBlaPrim) ||
                !makeShader(shaders.ldsBlaSub, csLdsBlaSub) ||
                !makeShader(shaders.deepExpPrim, csDeepExpPrim) ||
                !makeShader(shaders.deepExpSub, csDeepExpSub))
            {
                return;
            }

            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = 64;
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constantBuffer)))
            {
                return;
            }

            D3D11_BUFFER_DESC metaDesc{};
            metaDesc.ByteWidth = 32 * 8;
            metaDesc.Usage = D3D11_USAGE_DYNAMIC;
            metaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            metaDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            metaDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            metaDesc.StructureByteStride = 8;
            if (FAILED(device->CreateBuffer(&metaDesc, nullptr, &blaMetaBuf)))
            {
                return;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC metaSrvDesc{};
            metaSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            metaSrvDesc.Buffer.NumElements = 32;
            if (FAILED(device->CreateShaderResourceView(blaMetaBuf.Get(), &metaSrvDesc, &blaMetaSrv)))
            {
                return;
            }

            available = true;
        }

        bool CanSupportConfig(const FractalConfig& config) const noexcept
        {
            if (!available || config.width <= 0 || config.height <= 0 || config.maxIterations < 0)
            {
                return false;
            }
            if (config.renderStride > 1 && config.width >= 8 && config.height >= 8)
            {
                return false;
            }

            if (config.juliaSet)
            {
                if (config.zoom >= GpuPerturbationThreshold) return false;
                if (config.fractalType != FractalType::Mandelbrot && config.fractalType != FractalType::BurningShip)
                    return false;
                const double jx = config.juliaX.ToDouble();
                const double jy = config.juliaY.ToDouble();
                if (!std::isfinite(jx) || !std::isfinite(jy)) return false;
                const double bailout2 = std::max(16.0, 4.0 * std::hypot(jx, jy));
                return std::isfinite(bailout2) && bailout2 <= 1e18;
            }

            if (config.zoom < GpuPerturbationThreshold)
            {
                return std::isfinite(config.centerX) && std::isfinite(config.centerY);
            }

            switch (config.fractalType)
            {
            case FractalType::Mandelbrot:
                return config.zoom <= MaxZoom;
            case FractalType::Tricorn:
                return config.zoom < kGpuTricornMaxZoom;
            case FractalType::BurningShip:
            case FractalType::Buffalo:
                return config.zoom < DeepZoomThreshold;
            case FractalType::Celtic:
            case FractalType::CubicMultibrot:
            case FractalType::Phoenix:
            case FractalType::Newton:
            default:
                return false;
            }
        }

        bool EnsureTypedSrvBuffer(
            size_t neededElems,
            UINT elemBytes,
            DXGI_FORMAT format,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems)
        {
            if (neededElems <= capElems && buf && srv) return true;
            const size_t currentBytes = capElems * elemBytes;
            const size_t neededBytes = std::max<size_t>(1, neededElems) * elemBytes;
            if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
            const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
            const size_t allocElems = allocBytes / elemBytes;

            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(allocElems * elemBytes);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            ComPtr<ID3D11Buffer> newBuf;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

            ComPtr<ID3D11ShaderResourceView> newSrv;
            if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

            buf = std::move(newBuf);
            srv = std::move(newSrv);
            capElems = allocElems;
            return true;
        }

        bool EnsureStructuredSrvBuffer(
            size_t neededElems,
            UINT strideBytes,
            ComPtr<ID3D11Buffer>& buf,
            ComPtr<ID3D11ShaderResourceView>& srv,
            size_t& capElems)
        {
            if (neededElems <= capElems && buf && srv) return true;
            const size_t currentBytes = capElems * strideBytes;
            const size_t neededBytes = std::max<size_t>(1, neededElems) * strideBytes;
            if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
            const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
            const size_t allocElems = allocBytes / strideBytes;

            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(allocElems * strideBytes);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = strideBytes;

            ComPtr<ID3D11Buffer> newBuf;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &newBuf))) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.NumElements = static_cast<UINT>(allocElems);

            ComPtr<ID3D11ShaderResourceView> newSrv;
            if (FAILED(device->CreateShaderResourceView(newBuf.Get(), &srvDesc, &newSrv))) return false;

            buf = std::move(newBuf);
            srv = std::move(newSrv);
            capElems = allocElems;
            return true;
        }

        bool EnsureUavAndStagingFloats(
            size_t neededFloats,
            ComPtr<ID3D11Buffer>& uavBuf,
            ComPtr<ID3D11UnorderedAccessView>& uav,
            ComPtr<ID3D11Buffer>& stagingBuf,
            size_t& capFloats)
        {
            if (neededFloats <= capFloats && uavBuf && uav && stagingBuf) return true;
            const size_t currentBytes = capFloats * sizeof(float);
            const size_t neededBytes = std::max<size_t>(1, neededFloats) * sizeof(float);
            if (neededBytes > kD3D11SafeMaxBufferBytes) return false;
            const size_t allocBytes = GrowCapacityBytes(currentBytes, neededBytes);
            const size_t allocFloats = allocBytes / sizeof(float);

            D3D11_BUFFER_DESC uavBufDesc{};
            uavBufDesc.ByteWidth = static_cast<UINT>(allocFloats * sizeof(float));
            uavBufDesc.Usage = D3D11_USAGE_DEFAULT;
            uavBufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            uavBufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            uavBufDesc.StructureByteStride = sizeof(float);

            ComPtr<ID3D11Buffer> newUavBuf;
            if (FAILED(device->CreateBuffer(&uavBufDesc, nullptr, &newUavBuf))) return false;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = static_cast<UINT>(allocFloats);

            ComPtr<ID3D11UnorderedAccessView> newUav;
            if (FAILED(device->CreateUnorderedAccessView(newUavBuf.Get(), &uavDesc, &newUav))) return false;

            D3D11_BUFFER_DESC stageDesc{};
            stageDesc.ByteWidth = static_cast<UINT>(allocFloats * sizeof(float));
            stageDesc.Usage = D3D11_USAGE_STAGING;
            stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            ComPtr<ID3D11Buffer> newStaging;
            if (FAILED(device->CreateBuffer(&stageDesc, nullptr, &newStaging))) return false;

            uavBuf = std::move(newUavBuf);
            uav = std::move(newUav);
            stagingBuf = std::move(newStaging);
            capFloats = allocFloats;
            return true;
        }

        bool UploadBufferDiscard(ID3D11Buffer* buf, const void* src, size_t bytes)
        {
            if (bytes == 0) return true;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
            std::memcpy(mapped.pData, src, bytes);
            context->Unmap(buf, 0);
            return true;
        }

        bool SyncReferenceAndBla(
            const FractalConfig& config,
            const DeepReferenceState& refState,
            double cMax,
            const std::atomic<bool>* cancelToken,
            bool& outRunDeepExp)
        {
            const size_t orbitCount = static_cast<size_t>(refState.refLen) + 1;
            const bool zoomDeepExp = (config.zoom >= kGpuDeepExpThreshold);

            if (refState.orbitVersion != uploadedOrbitVersion ||
                refState.refLen > uploadedRefLen ||
                config.fractalType != uploadedType)
            {
                if (!EnsureTypedSrvBuffer(
                    orbitCount, 8, DXGI_FORMAT_R32G32_FLOAT, orbitFastBuf, orbitFastSrv, orbitFastCapElems))
                {
                    return false;
                }
                hostOrbitFast.resize(orbitCount * 2);
                for (size_t m = 0; m < orbitCount; ++m)
                {
                    hostOrbitFast[m * 2 + 0] = static_cast<float>(refState.orbit[m].x);
                    hostOrbitFast[m * 2 + 1] = static_cast<float>(refState.orbit[m].y);
                }
                if (!UploadBufferDiscard(orbitFastBuf.Get(), hostOrbitFast.data(), orbitCount * 8))
                {
                    return false;
                }
                uploadedOrbitVersion = refState.orbitVersion;
                uploadedRefLen = refState.refLen;
                uploadedType = config.fractalType;
                uploadedHasDeepExpOrbit = false;
            }

            if (config.fractalType == FractalType::Mandelbrot)
            {
                if (refState.orbitVersion != uploadedBlaOrbitVersion ||
                    refState.refLen != uploadedBlaRefLen ||
                    cMax > uploadedBlaCMax * (1.0 + 1e-12) ||
                    cMax < uploadedBlaCMax * 0.25)
                {
                    blaBuilder.Build(refState.orbit, refState.refLen, cMax, cancelToken);
                    if (cancelToken && cancelToken->load(std::memory_order_relaxed))
                    {
                        return false;
                    }
                    uploadedBlaOrbitVersion = refState.orbitVersion;
                    uploadedBlaRefLen = refState.refLen;
                    uploadedBlaCMax = cMax;
                    uploadedHasBlaF32 = false;
                    uploadedHasBlaExp = false;

                    if (blaBuilder.numLevels >= 3)
                    {
                        if (!UploadBufferDiscard(blaMetaBuf.Get(), blaBuilder.levelMeta.data(), 32 * 8))
                        {
                            return false;
                        }
                    }
                }
            }
            else
            {
                blaBuilder.Clear();
                uploadedBlaOrbitVersion = 0;
                uploadedBlaRefLen = -1;
                uploadedBlaCMax = 0.0;
                uploadedHasBlaF32 = false;
                uploadedHasBlaExp = false;
            }

            const bool hasBla = (config.fractalType == FractalType::Mandelbrot && blaBuilder.numLevels >= 3);
            outRunDeepExp = zoomDeepExp || (hasBla && !blaBuilder.fitsFloat32);

            if (outRunDeepExp && !uploadedHasDeepExpOrbit)
            {
                if (!EnsureStructuredSrvBuffer(
                    orbitCount, sizeof(GpuOrbitPointExp), orbitFullBuf, orbitFullSrv, orbitFullCapElems))
                {
                    return false;
                }
                hostOrbitExp.resize(orbitCount);
                for (size_t m = 0; m < orbitCount; ++m)
                {
                    const double rx = refState.orbit[m].x;
                    const double ry = refState.orbit[m].y;
                    hostOrbitExp[m].rx = static_cast<float>(rx);
                    hostOrbitExp[m].ry = static_cast<float>(ry);
                    SplitFrexp2(rx, ry, hostOrbitExp[m].rmx, hostOrbitExp[m].rmy, hostOrbitExp[m].rExp);
                }
                if (!UploadBufferDiscard(orbitFullBuf.Get(), hostOrbitExp.data(), orbitCount * sizeof(GpuOrbitPointExp)))
                {
                    return false;
                }
                uploadedHasDeepExpOrbit = true;
            }

            if (hasBla)
            {
                if (outRunDeepExp)
                {
                    if (!uploadedHasBlaExp)
                    {
                        if (!EnsureStructuredSrvBuffer(
                            blaBuilder.blocksExp.size(), sizeof(GpuBlaBlockExp), blaExpBuf, blaExpSrv, blaExpCapElems))
                        {
                            return false;
                        }
                        if (!UploadBufferDiscard(
                            blaExpBuf.Get(),
                            blaBuilder.blocksExp.data(),
                            blaBuilder.blocksExp.size() * sizeof(GpuBlaBlockExp)))
                        {
                            return false;
                        }
                        uploadedHasBlaExp = true;
                    }
                }
                else
                {
                    if (!uploadedHasBlaF32)
                    {
                        if (!EnsureStructuredSrvBuffer(
                            blaBuilder.blocksF32.size(), sizeof(GpuBlaBlockF32), blaF32Buf, blaF32Srv, blaF32CapElems))
                        {
                            return false;
                        }
                        if (!UploadBufferDiscard(
                            blaF32Buf.Get(),
                            blaBuilder.blocksF32.data(),
                            blaBuilder.blocksF32.size() * sizeof(GpuBlaBlockF32)))
                        {
                            return false;
                        }
                        uploadedHasBlaF32 = true;
                    }
                }
            }

            return true;
        }
    };

    GpuFractalSolver::GpuFractalSolver(bool allowSoftwareAdapter)
        : m_impl(std::make_unique<Impl>(allowSoftwareAdapter))
    {
    }

    GpuFractalSolver::~GpuFractalSolver() = default;

    bool GpuFractalSolver::IsAvailable() const noexcept
    {
        return m_impl && m_impl->available;
    }

    bool GpuFractalSolver::IsUsingSoftwareAdapter() const noexcept
    {
        return m_impl && m_impl->usingSoftwareAdapter;
    }

    void GpuFractalSolver::PrepareReference(
        const FractalConfig& config,
        DeepReferenceCache& referenceCache,
        const std::atomic<bool>* cancelToken)
    {
        if (!m_impl || !m_impl->CanSupportConfig(config) ||
            config.juliaSet || config.zoom < GpuPerturbationThreshold)
        {
            return;
        }

        std::lock_guard lock(m_impl->mutex);

        BigFloat hpX = config.hpCenterX;
        BigFloat hpY = config.hpCenterY;
        if (std::abs(hpX.ToDouble() - config.centerX) > 1e-14 ||
            std::abs(hpY.ToDouble() - config.centerY) > 1e-14)
        {
            hpX = BigFloat(config.centerX);
            hpY = BigFloat(config.centerY);
        }

        DeepReferenceState refState{};
        if (!referenceCache.Ensure(config, hpX, hpY, ExecutionMode::Gpu, refState, cancelToken))
        {
            return;
        }

        const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double step = (3.0 / clampedZoom) / static_cast<double>(config.width);
        const double maxCx = std::max(std::abs(-0.25 - refState.refPosX), std::abs(config.width - 0.75 - refState.refPosX)) * step;
        const double maxCy = std::max(std::abs(refState.refPosY + 0.25), std::abs(refState.refPosY - (config.height - 0.75))) * step;
        const double cMax = std::hypot(maxCx, maxCy);

        bool unusedRunDeepExp = false;
        m_impl->SyncReferenceAndBla(config, refState, cMax, cancelToken, unusedRunDeepExp);
    }

    bool GpuFractalSolver::TryCompute(
        const FractalConfig& config,
        const BigFloat& centerX,
        const BigFloat& centerY,
        DeepReferenceCache& referenceCache,
        std::vector<float>& sampleWeights,
        std::vector<float>& outIterations,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples,
        double& outElapsedMs)
    {
        outElapsedMs = 0.0;
        if (!m_impl || !m_impl->CanSupportConfig(config))
        {
            return false;
        }

        std::lock_guard lock(m_impl->mutex);
        const auto startTime = std::chrono::high_resolution_clock::now();
        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };

        if (subpixelSamples) subpixelSamples->clear();
        const int width = config.width;
        const int height = config.height;
        const int maxIter = config.maxIterations;
        const size_t totalPixels = static_cast<size_t>(width) * static_cast<size_t>(height);

        if (subpixelSamples && totalPixels > std::numeric_limits<uint32_t>::max())
        {
            throw std::length_error("Supersampling image is too large.");
        }

        if (outIterations.size() != totalPixels)
        {
            outIterations.resize(totalPixels);
        }

        if (maxIter == 0)
        {
            std::fill(outIterations.begin(), outIterations.end(), -1.0f);
            outElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - startTime).count();
            return true;
        }

        if (canceled())
        {
            return true;
        }

        enum class ShaderMode
        {
            Shallow,
            PerturbLds,
            PerturbLdsBla,
            PerturbDeepExp
        };

        ShaderMode mode = ShaderMode::Shallow;
        DeepReferenceState refState{};
        double juliaCx = 0.0, juliaCy = 0.0, juliaBailout2 = 16.0;

        if (config.zoom < GpuPerturbationThreshold)
        {
            mode = ShaderMode::Shallow;
            const double effectiveZoom = config.juliaSet
                ? std::clamp(config.zoom, MinZoom, MaxZoom)
                : (config.zoom > 1e-12 ? config.zoom : 1e-12);
            const double step = (3.0 / effectiveZoom) / static_cast<double>(width);
            const double halfW = static_cast<double>(width) * 0.5;
            const double halfH = static_cast<double>(height) * 0.5;
            const double cx0 = config.juliaSet ? centerX.ToDouble() : config.centerX;
            const double cy0 = config.juliaSet ? centerY.ToDouble() : config.centerY;

            if (config.juliaSet)
            {
                juliaCx = config.juliaX.ToDouble();
                juliaCy = config.juliaY.ToDouble();
                juliaBailout2 = std::max(16.0, 4.0 * std::hypot(juliaCx, juliaCy));
            }

            m_impl->hostColCoords.resize(static_cast<size_t>(width) * 3);
            for (int x = 0; x < width; ++x)
            {
                m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(cx0 + ((x + 0.25) - halfW) * step);
                m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>(cx0 + ((x + 0.50) - halfW) * step);
                m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(cx0 + ((x + 0.75) - halfW) * step);
            }
            m_impl->hostRowCoords.resize(static_cast<size_t>(height) * 3);
            for (int y = 0; y < height; ++y)
            {
                m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>(cy0 + (halfH - (y + 0.25)) * step);
                m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>(cy0 + (halfH - (y + 0.50)) * step);
                m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>(cy0 + (halfH - (y + 0.75)) * step);
            }

            if (!m_impl->EnsureTypedSrvBuffer(
                    m_impl->hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    m_impl->colCoordsBuf, m_impl->colCoordsSrv, m_impl->colCoordsCapFloats) ||
                !m_impl->EnsureTypedSrvBuffer(
                    m_impl->hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                    m_impl->rowCoordsBuf, m_impl->rowCoordsSrv, m_impl->rowCoordsCapFloats) ||
                !m_impl->UploadBufferDiscard(
                    m_impl->colCoordsBuf.Get(), m_impl->hostColCoords.data(), m_impl->hostColCoords.size() * sizeof(float)) ||
                !m_impl->UploadBufferDiscard(
                    m_impl->rowCoordsBuf.Get(), m_impl->hostRowCoords.data(), m_impl->hostRowCoords.size() * sizeof(float)))
            {
                return false;
            }
        }
        else
        {
            if (!referenceCache.Ensure(config, centerX, centerY, ExecutionMode::Gpu, refState, cancelToken))
            {
                return canceled();
            }

            const double clampedZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
            const double step = (3.0 / clampedZoom) / static_cast<double>(width);
            const double maxCx = std::max(std::abs(-0.25 - refState.refPosX), std::abs(width - 0.75 - refState.refPosX)) * step;
            const double maxCy = std::max(std::abs(refState.refPosY + 0.25), std::abs(refState.refPosY - (height - 0.75))) * step;
            const double cMax = std::hypot(maxCx, maxCy);

            bool runDeepExp = false;
            if (!m_impl->SyncReferenceAndBla(config, refState, cMax, cancelToken, runDeepExp))
            {
                return canceled();
            }

            if (runDeepExp)
            {
                mode = ShaderMode::PerturbDeepExp;
                int stepExp = 0;
                const double stepMantD = std::frexp(step, &stepExp);

                m_impl->hostColCoordsExp.resize(static_cast<size_t>(width) * 3);
                for (int x = 0; x < width; ++x)
                {
                    SplitFrexpSigned(((x - 0.25) - refState.refPosX) * stepMantD, stepExp,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 0].mant,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 0].exp);
                    SplitFrexpSigned((static_cast<double>(x) - refState.refPosX) * stepMantD, stepExp,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 1].mant,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 1].exp);
                    SplitFrexpSigned(((x + 0.25) - refState.refPosX) * stepMantD, stepExp,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 2].mant,
                        m_impl->hostColCoordsExp[static_cast<size_t>(x) * 3 + 2].exp);
                }

                m_impl->hostRowCoordsExp.resize(static_cast<size_t>(height) * 3);
                for (int y = 0; y < height; ++y)
                {
                    SplitFrexpSigned((refState.refPosY - (y - 0.25)) * stepMantD, stepExp,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 0].mant,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 0].exp);
                    SplitFrexpSigned((refState.refPosY - static_cast<double>(y)) * stepMantD, stepExp,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 1].mant,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 1].exp);
                    SplitFrexpSigned((refState.refPosY - (y + 0.25)) * stepMantD, stepExp,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 2].mant,
                        m_impl->hostRowCoordsExp[static_cast<size_t>(y) * 3 + 2].exp);
                }

                if (!m_impl->EnsureStructuredSrvBuffer(
                        m_impl->hostColCoordsExp.size(), sizeof(GpuCoordExp),
                        m_impl->colCoordsExpBuf, m_impl->colCoordsExpSrv, m_impl->colCoordsExpCapElems) ||
                    !m_impl->EnsureStructuredSrvBuffer(
                        m_impl->hostRowCoordsExp.size(), sizeof(GpuCoordExp),
                        m_impl->rowCoordsExpBuf, m_impl->rowCoordsExpSrv, m_impl->rowCoordsExpCapElems) ||
                    !m_impl->UploadBufferDiscard(
                        m_impl->colCoordsExpBuf.Get(), m_impl->hostColCoordsExp.data(),
                        m_impl->hostColCoordsExp.size() * sizeof(GpuCoordExp)) ||
                    !m_impl->UploadBufferDiscard(
                        m_impl->rowCoordsExpBuf.Get(), m_impl->hostRowCoordsExp.data(),
                        m_impl->hostRowCoordsExp.size() * sizeof(GpuCoordExp)))
                {
                    return false;
                }
            }
            else
            {
                const bool hasBla = (config.fractalType == FractalType::Mandelbrot && m_impl->blaBuilder.numLevels >= 3);
                mode = hasBla ? ShaderMode::PerturbLdsBla : ShaderMode::PerturbLds;

                m_impl->hostColCoords.resize(static_cast<size_t>(width) * 3);
                for (int x = 0; x < width; ++x)
                {
                    m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 0] = static_cast<float>(((x - 0.25) - refState.refPosX) * step);
                    m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 1] = static_cast<float>((static_cast<double>(x) - refState.refPosX) * step);
                    m_impl->hostColCoords[static_cast<size_t>(x) * 3 + 2] = static_cast<float>(((x + 0.25) - refState.refPosX) * step);
                }
                m_impl->hostRowCoords.resize(static_cast<size_t>(height) * 3);
                for (int y = 0; y < height; ++y)
                {
                    m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 0] = static_cast<float>((refState.refPosY - (y - 0.25)) * step);
                    m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 1] = static_cast<float>((refState.refPosY - static_cast<double>(y)) * step);
                    m_impl->hostRowCoords[static_cast<size_t>(y) * 3 + 2] = static_cast<float>((refState.refPosY - (y + 0.25)) * step);
                }

                if (!m_impl->EnsureTypedSrvBuffer(
                        m_impl->hostColCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                        m_impl->colCoordsBuf, m_impl->colCoordsSrv, m_impl->colCoordsCapFloats) ||
                    !m_impl->EnsureTypedSrvBuffer(
                        m_impl->hostRowCoords.size(), 4, DXGI_FORMAT_R32_FLOAT,
                        m_impl->rowCoordsBuf, m_impl->rowCoordsSrv, m_impl->rowCoordsCapFloats) ||
                    !m_impl->UploadBufferDiscard(
                        m_impl->colCoordsBuf.Get(), m_impl->hostColCoords.data(), m_impl->hostColCoords.size() * sizeof(float)) ||
                    !m_impl->UploadBufferDiscard(
                        m_impl->rowCoordsBuf.Get(), m_impl->hostRowCoords.data(), m_impl->hostRowCoords.size() * sizeof(float)))
                {
                    return false;
                }
            }
        }

        const size_t maxRowsByMem = std::max<size_t>(1, kMaxPrimaryChunkPixels / static_cast<size_t>(width));
        const size_t maxRowsByTdr = std::max<size_t>(
            1,
            static_cast<size_t>(kMaxPrimaryIterBudget /
                (static_cast<uint64_t>(width) * static_cast<uint64_t>(std::max(1, maxIter)))));
        const int maxChunkRows = static_cast<int>(std::min<size_t>({ static_cast<size_t>(height), maxRowsByMem, maxRowsByTdr }));
        const size_t primaryChunkCap = static_cast<size_t>(width) * static_cast<size_t>(maxChunkRows);

        if (!m_impl->EnsureUavAndStagingFloats(
            primaryChunkCap, m_impl->outItersBuf, m_impl->outItersUav, m_impl->outItersStaging, m_impl->outItersCapElems))
        {
            return false;
        }

        auto updateConstantBuffer = [&](uint32_t rowOffset, uint32_t subTotal) -> bool
        {
            switch (mode)
            {
            case ShaderMode::Shallow:
            {
                ShallowCb cb{};
                cb.width = static_cast<uint32_t>(width);
                cb.height = static_cast<uint32_t>(height);
                cb.maxIter = static_cast<uint32_t>(maxIter);
                cb.smoothFlag = config.smooth ? 1u : 0u;
                cb.fractalType = static_cast<uint32_t>(config.fractalType);
                cb.juliaFlag = config.juliaSet ? 1u : 0u;
                cb.rowOffset = rowOffset;
                cb.subpixelTotal = subTotal;
                cb.juliaCx = static_cast<float>(juliaCx);
                cb.juliaCy = static_cast<float>(juliaCy);
                cb.bailout2 = static_cast<float>(juliaBailout2);
                return m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &cb, sizeof(cb));
            }
            case ShaderMode::PerturbLds:
            {
                PerturbLdsCb cb{};
                cb.width = static_cast<uint32_t>(width);
                cb.height = static_cast<uint32_t>(height);
                cb.maxIter = static_cast<uint32_t>(maxIter);
                cb.refLen = static_cast<uint32_t>(refState.refLen);
                cb.fractalType = static_cast<uint32_t>(config.fractalType);
                cb.smoothFlag = config.smooth ? 1u : 0u;
                cb.checkCardioid = (config.fractalType == FractalType::Mandelbrot && config.zoom < 1e6) ? 1u : 0u;
                cb.needScaleCheck = (config.zoom >= 1e15) ? 1u : 0u;
                cb.rowOffset = rowOffset;
                cb.subpixelTotal = subTotal;
                cb.refCx = static_cast<float>(refState.refCxDouble);
                cb.refCy = static_cast<float>(refState.refCyDouble);
                return m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &cb, sizeof(cb));
            }
            case ShaderMode::PerturbLdsBla:
            {
                PerturbLdsBlaCb cb{};
                cb.width = static_cast<uint32_t>(width);
                cb.height = static_cast<uint32_t>(height);
                cb.maxIter = static_cast<uint32_t>(maxIter);
                cb.refLen = static_cast<uint32_t>(refState.refLen);
                cb.smoothFlag = config.smooth ? 1u : 0u;
                cb.blaNumLevels = m_impl->blaBuilder.numLevels;
                cb.rowOffset = rowOffset;
                cb.subpixelTotal = subTotal;
                cb.blaMaxRadius = static_cast<float>(m_impl->blaBuilder.maxRadD);
                cb.refCx = static_cast<float>(refState.refCxDouble);
                cb.refCy = static_cast<float>(refState.refCyDouble);
                cb.checkCardioid = (config.zoom < 1e6) ? 1u : 0u;
                return m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &cb, sizeof(cb));
            }
            case ShaderMode::PerturbDeepExp:
            default:
            {
                const bool useBla = (config.fractalType == FractalType::Mandelbrot && m_impl->blaBuilder.numLevels >= 3);
                PerturbDeepExpCb cb{};
                cb.width = static_cast<uint32_t>(width);
                cb.height = static_cast<uint32_t>(height);
                cb.maxIter = static_cast<uint32_t>(maxIter);
                cb.refLen = static_cast<uint32_t>(refState.refLen);
                cb.smoothFlag = config.smooth ? 1u : 0u;
                cb.useBla = useBla ? 1u : 0u;
                cb.blaNumLevels = m_impl->blaBuilder.numLevels;
                cb.rowOffset = rowOffset;
                cb.subpixelTotal = subTotal;
                cb.blaMaxRadMant = m_impl->blaBuilder.maxRadMant;
                cb.blaMaxRadExp = m_impl->blaBuilder.maxRadExp;
                cb.refCx = static_cast<float>(refState.refCxDouble);
                cb.refCy = static_cast<float>(refState.refCyDouble);
                return m_impl->UploadBufferDiscard(m_impl->constantBuffer.Get(), &cb, sizeof(cb));
            }
            }
        };

        auto bindModeResources = [&]()
        {
            ID3D11Buffer* cbs[]{ m_impl->constantBuffer.Get() };
            m_impl->context->CSSetConstantBuffers(0, 1, cbs);

            ID3D11ShaderResourceView* srvs[7]{};
            switch (mode)
            {
            case ShaderMode::Shallow:
                srvs[0] = m_impl->colCoordsSrv.Get();
                srvs[1] = m_impl->rowCoordsSrv.Get();
                srvs[4] = m_impl->subIdxSrv.Get();
                break;
            case ShaderMode::PerturbLds:
                srvs[0] = m_impl->orbitFastSrv.Get();
                srvs[1] = m_impl->colCoordsSrv.Get();
                srvs[2] = m_impl->rowCoordsSrv.Get();
                srvs[4] = m_impl->subIdxSrv.Get();
                break;
            case ShaderMode::PerturbLdsBla:
                srvs[0] = m_impl->orbitFastSrv.Get();
                srvs[1] = m_impl->colCoordsSrv.Get();
                srvs[2] = m_impl->rowCoordsSrv.Get();
                srvs[3] = m_impl->blaF32Srv.Get();
                srvs[4] = m_impl->blaMetaSrv.Get();
                srvs[5] = m_impl->subIdxSrv.Get();
                break;
            case ShaderMode::PerturbDeepExp:
                srvs[0] = m_impl->orbitFastSrv.Get();
                srvs[1] = m_impl->orbitFullSrv.Get();
                srvs[2] = m_impl->colCoordsExpSrv.Get();
                srvs[3] = m_impl->rowCoordsExpSrv.Get();
                srvs[4] = m_impl->blaExpSrv.Get();
                srvs[5] = m_impl->blaMetaSrv.Get();
                srvs[6] = m_impl->subIdxSrv.Get();
                break;
            }
            m_impl->context->CSSetShaderResources(0, 7, srvs);

            ID3D11UnorderedAccessView* uavs[2]{ m_impl->outItersUav.Get(), m_impl->subOutUav.Get() };
            m_impl->context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        };

        // Pass 1: Primary center pixel evaluation (chunked by rows for TDR safety and bounded VRAM)
        bindModeResources();
        switch (mode)
        {
        case ShaderMode::Shallow:
            m_impl->context->CSSetShader(m_impl->csShallowPrim.Get(), nullptr, 0);
            break;
        case ShaderMode::PerturbLds:
            m_impl->context->CSSetShader(m_impl->csLdsPrim.Get(), nullptr, 0);
            break;
        case ShaderMode::PerturbLdsBla:
            m_impl->context->CSSetShader(m_impl->csLdsBlaPrim.Get(), nullptr, 0);
            break;
        case ShaderMode::PerturbDeepExp:
            m_impl->context->CSSetShader(m_impl->csDeepExpPrim.Get(), nullptr, 0);
            break;
        }

        const UINT groupX = (mode == ShaderMode::PerturbLds || mode == ShaderMode::PerturbLdsBla)
            ? static_cast<UINT>((width + 31) / 32)
            : static_cast<UINT>((width + 15) / 16);

        for (int rowStart = 0; rowStart < height; rowStart += maxChunkRows)
        {
            if (canceled()) return true;
            const int chunkRows = std::min(maxChunkRows, height - rowStart);
            const size_t chunkFloats = static_cast<size_t>(width) * static_cast<size_t>(chunkRows);

            if (!updateConstantBuffer(static_cast<uint32_t>(rowStart), 0u))
            {
                return false;
            }

            const UINT groupY = static_cast<UINT>((chunkRows + 15) / 16);
            m_impl->context->Dispatch(groupX, groupY, 1);

            D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(chunkFloats * sizeof(float)), 1, 1 };
            m_impl->context->CopySubresourceRegion(
                m_impl->outItersStaging.Get(), 0, 0, 0, 0, m_impl->outItersBuf.Get(), 0, &box);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_impl->context->Map(m_impl->outItersStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                return false;
            }
            std::memcpy(
                outIterations.data() + static_cast<size_t>(rowStart) * static_cast<size_t>(width),
                mapped.pData,
                chunkFloats * sizeof(float));
            m_impl->context->Unmap(m_impl->outItersStaging.Get(), 0);
        }

        // Pass 2: Optional 4x adaptive boundary subpixel refinement
        if (subpixelSamples && !canceled())
        {
            const bool smooth = config.smooth;
            const FractalType type = config.fractalType;
            sampleWeights.resize(totalPixels);
            std::vector<size_t> rowOffsets(static_cast<size_t>(height) + 1, 0);

            #pragma omp parallel for schedule(static)
            for (int y = 0; y < height; ++y)
            {
                if (canceled()) continue;
                size_t selected = 0;
                const int y0 = std::max(0, y - 1), y1 = std::min(height - 1, y + 1);
                for (int x = 0; x < width; ++x)
                {
                    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                    const float center = outIterations[index];
                    const int x0 = std::max(0, x - 1), x1 = std::min(width - 1, x + 1);
                    if (center < 0.0f)
                    {
                        bool boundary = false;
                        for (int ny = y0; ny <= y1 && !boundary; ++ny)
                        {
                            const float* nRow = outIterations.data() + static_cast<size_t>(ny) * static_cast<size_t>(width);
                            for (int nx = x0; nx <= x1; ++nx)
                            {
                                if (nRow[nx] >= 0.0f) { boundary = true; break; }
                            }
                        }
                        sampleWeights[index] = boundary ? 1.0f : 0.0f;
                        if (boundary) ++selected;
                        continue;
                    }
                    float variation = 0.0f;
                    bool boundary = false;
                    const float a = (type == FractalType::Newton) ? std::fmod(center, 1000000.0f) : center;
                    for (int ny = y0; ny <= y1; ++ny)
                    {
                        const float* nRow = outIterations.data() + static_cast<size_t>(ny) * static_cast<size_t>(width);
                        for (int nx = x0; nx <= x1; ++nx)
                        {
                            if (nx == x && ny == y) continue;
                            const float neighbor = nRow[nx];
                            if (neighbor < 0.0f) { boundary = true; continue; }
                            if (!detail::SameBasin(center, neighbor, type) || (!smooth && center != neighbor))
                            {
                                boundary = true;
                            }
                            const float b = (type == FractalType::Newton) ? std::fmod(neighbor, 1000000.0f) : neighbor;
                            variation = std::max(variation, std::abs(a - b) / (10.0f + std::min(a, b)));
                        }
                    }
                    const float t = std::clamp((variation - 0.015f) / 0.065f, 0.0f, 1.0f);
                    const float weight = boundary ? 1.0f : t * t * (3.0f - 2.0f * t);
                    sampleWeights[index] = weight;
                    if (weight > 0.0f) ++selected;
                }
                rowOffsets[static_cast<size_t>(y) + 1] = selected;
            }

            if (canceled())
            {
                subpixelSamples->clear();
                return true;
            }

            for (int y = 0; y < height; ++y)
            {
                rowOffsets[static_cast<size_t>(y) + 1] += rowOffsets[static_cast<size_t>(y)];
            }
            const size_t subCount = rowOffsets.back();

            if (subCount > 0)
            {
                subpixelSamples->resize(subCount);
                m_impl->hostSlotOfPixel.resize(totalPixels);

                #pragma omp parallel for schedule(static)
                for (int y = 0; y < height; ++y)
                {
                    if (rowOffsets[static_cast<size_t>(y)] == rowOffsets[static_cast<size_t>(y) + 1]) continue;
                    size_t output = rowOffsets[static_cast<size_t>(y)];
                    for (int x = 0; x < width; ++x)
                    {
                        const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                        if (sampleWeights[index] <= 0.0f) continue;
                        m_impl->hostSlotOfPixel[index] = static_cast<uint32_t>(output);
                        auto& sample = (*subpixelSamples)[output++];
                        sample.pixelIndex = static_cast<uint32_t>(index);
                        sample.weight = sampleWeights[index];
                    }
                }

                // Group active pixels into 16x16 spatial tiles for wave coherence while preserving sorted output slots
                const int tilesX = (width + 15) / 16;
                const int tilesY = (height + 15) / 16;
                const int numTiles = tilesX * tilesY;
                std::vector<size_t> tileOffsets(static_cast<size_t>(numTiles) + 1, 0);

                #pragma omp parallel for schedule(static)
                for (int ty = 0; ty < tilesY; ++ty)
                {
                    const int y0 = ty * 16, y1 = std::min(height, y0 + 16);
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        const int x0 = tx * 16, x1 = std::min(width, x0 + 16);
                        size_t cnt = 0;
                        for (int y = y0; y < y1; ++y)
                        {
                            const float* wRow = sampleWeights.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
                            for (int x = x0; x < x1; ++x)
                            {
                                if (wRow[x] > 0.0f) ++cnt;
                            }
                        }
                        tileOffsets[static_cast<size_t>(ty * tilesX + tx) + 1] = cnt;
                    }
                }
                for (int t = 0; t < numTiles; ++t)
                {
                    tileOffsets[static_cast<size_t>(t) + 1] += tileOffsets[static_cast<size_t>(t)];
                }

                m_impl->hostTiledPixels.resize(subCount);
                m_impl->hostTiledSlots.resize(subCount);

                #pragma omp parallel for schedule(static)
                for (int ty = 0; ty < tilesY; ++ty)
                {
                    const int y0 = ty * 16, y1 = std::min(height, y0 + 16);
                    for (int tx = 0; tx < tilesX; ++tx)
                    {
                        const size_t t = static_cast<size_t>(ty * tilesX + tx);
                        size_t wp = tileOffsets[t];
                        if (wp == tileOffsets[t + 1]) continue;
                        const int x0 = tx * 16, x1 = std::min(width, x0 + 16);
                        for (int y = y0; y < y1; ++y)
                        {
                            for (int x = x0; x < x1; ++x)
                            {
                                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                                if (sampleWeights[idx] <= 0.0f) continue;
                                m_impl->hostTiledPixels[wp] = static_cast<uint32_t>(idx);
                                m_impl->hostTiledSlots[wp] = m_impl->hostSlotOfPixel[idx];
                                ++wp;
                            }
                        }
                    }
                }

                const size_t maxBatchPix = std::min(subCount, kMaxSubpixelBatchPixels);
                if (!m_impl->EnsureTypedSrvBuffer(
                        maxBatchPix, 4, DXGI_FORMAT_R32_UINT,
                        m_impl->subIdxBuf, m_impl->subIdxSrv, m_impl->subIdxCapElems) ||
                    !m_impl->EnsureUavAndStagingFloats(
                        maxBatchPix * 4,
                        m_impl->subOutBuf, m_impl->subOutUav, m_impl->subOutStaging, m_impl->subOutCapFloats))
                {
                    subpixelSamples->clear();
                    return false;
                }

                bindModeResources();
                switch (mode)
                {
                case ShaderMode::Shallow:
                    m_impl->context->CSSetShader(m_impl->csShallowSub.Get(), nullptr, 0);
                    break;
                case ShaderMode::PerturbLds:
                    m_impl->context->CSSetShader(m_impl->csLdsSub.Get(), nullptr, 0);
                    break;
                case ShaderMode::PerturbLdsBla:
                    m_impl->context->CSSetShader(m_impl->csLdsBlaSub.Get(), nullptr, 0);
                    break;
                case ShaderMode::PerturbDeepExp:
                    m_impl->context->CSSetShader(m_impl->csDeepExpSub.Get(), nullptr, 0);
                    break;
                }

                const uint32_t subGroupThreads =
                    (mode == ShaderMode::PerturbLds || mode == ShaderMode::PerturbLdsBla) ? 512u : 256u;

                for (size_t batchStart = 0; batchStart < subCount; batchStart += kMaxSubpixelBatchPixels)
                {
                    if (canceled())
                    {
                        subpixelSamples->clear();
                        return true;
                    }
                    const size_t batchPix = std::min(kMaxSubpixelBatchPixels, subCount - batchStart);
                    const uint32_t subTotal = static_cast<uint32_t>(batchPix * 4);

                    if (!m_impl->UploadBufferDiscard(
                            m_impl->subIdxBuf.Get(),
                            m_impl->hostTiledPixels.data() + batchStart,
                            batchPix * sizeof(uint32_t)) ||
                        !updateConstantBuffer(0u, subTotal))
                    {
                        subpixelSamples->clear();
                        return false;
                    }

                    const UINT groups = static_cast<UINT>((subTotal + subGroupThreads - 1u) / subGroupThreads);
                    m_impl->context->Dispatch(groups, 1, 1);

                    D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(subTotal * sizeof(float)), 1, 1 };
                    m_impl->context->CopySubresourceRegion(
                        m_impl->subOutStaging.Get(), 0, 0, 0, 0, m_impl->subOutBuf.Get(), 0, &box);

                    m_impl->hostSubFloats.resize(subTotal);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (FAILED(m_impl->context->Map(m_impl->subOutStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                    {
                        subpixelSamples->clear();
                        return false;
                    }
                    std::memcpy(m_impl->hostSubFloats.data(), mapped.pData, subTotal * sizeof(float));
                    m_impl->context->Unmap(m_impl->subOutStaging.Get(), 0);

                    #pragma omp parallel for schedule(static)
                    for (intptr_t i = 0; i < static_cast<intptr_t>(batchPix); ++i)
                    {
                        const uint32_t slot = m_impl->hostTiledSlots[batchStart + static_cast<size_t>(i)];
                        auto& dst = (*subpixelSamples)[slot];
                        dst.iterations[0] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 0];
                        dst.iterations[1] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 1];
                        dst.iterations[2] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 2];
                        dst.iterations[3] = m_impl->hostSubFloats[static_cast<size_t>(i) * 4 + 3];
                    }
                }

                if (canceled())
                {
                    subpixelSamples->clear();
                    return true;
                }
            }
        }

        // Unbind resources cleanly
        ID3D11UnorderedAccessView* nullUavs[2]{ nullptr, nullptr };
        m_impl->context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        ID3D11ShaderResourceView* nullSrvs[7]{};
        m_impl->context->CSSetShaderResources(0, 7, nullSrvs);

        outElapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - startTime).count();
        return true;
    }
}
