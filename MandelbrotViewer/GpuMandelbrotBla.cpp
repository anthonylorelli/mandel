#include "pch.h"
#include "GpuMandelbrotBla.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace Mandelbrot::detail
{
    void GpuMandelbrotBlaBuilder::Clear() noexcept
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

    void GpuMandelbrotBlaBuilder::Build(
        const ComplexOrbitPoint* orbit,
        int refLength,
        double cMax,
        const std::atomic<bool>* cancel)
    {
        Clear();
        if (!orbit || refLength <= 8 || !(cMax > 0.0) || !std::isfinite(cMax))
        {
            return;
        }
        auto canceled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
        if (canceled()) return;

        constexpr double epsTol = 2.5e-8;
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
                m.radius = std::min(x.radius, allowance / ax);
            }
            return m;
        };

        const BlockD firstFour = merge(
            merge(single(orbit[1].x, orbit[1].y), single(orbit[2].x, orbit[2].y)),
            merge(single(orbit[3].x, orbit[3].y), single(orbit[4].x, orbit[4].y)));
        if (!(firstFour.radius > cMax * 0.125))
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

        SplitFrexpPositive(localMaxRad, maxRadMant, maxRadExp);
        numLevels = static_cast<uint32_t>(levels.size());

        size_t totalBlocks = 0;
        for (uint32_t lv = 2; lv < numLevels; ++lv)
        {
            totalBlocks += levels[lv].size();
        }
        blocksF32.reserve(totalBlocks);
        blocksExp.reserve(totalBlocks);

        double localMaxRadF32 = 0.0;
        for (uint32_t lv = 2; lv < numLevels; ++lv)
        {
            levelMeta[lv * 2 + 0] = static_cast<uint32_t>(blocksExp.size());
            levelMeta[lv * 2 + 1] = static_cast<uint32_t>(levels[lv].size());
            for (const auto& b : levels[lv])
            {
                const double maxCoeff = std::max({ std::abs(b.ar), std::abs(b.ai), std::abs(b.br), std::abs(b.bi) });
                const double aAmp = std::hypot(b.ar, b.ai);
                const bool blockFitsF32 = (maxCoeff <= 1e34 && aAmp >= 1e-6 && b.radius >= 1e-34);

                GpuBlaBlockF32 bf{};
                if (blockFitsF32)
                {
                    bf.ar = static_cast<float>(b.ar);
                    bf.ai = static_cast<float>(b.ai);
                    bf.br = static_cast<float>(b.br);
                    bf.bi = static_cast<float>(b.bi);
                    bf.radius = static_cast<float>(b.radius);
                    localMaxRadF32 = std::max(localMaxRadF32, b.radius);
                }
                blocksF32.push_back(bf);

                GpuBlaBlockExp be{};
                SplitFrexp2(b.ar, b.ai, be.ar, be.ai, be.aExp);
                SplitFrexp2(b.br, b.bi, be.br, be.bi, be.bExp);
                SplitFrexpPositive(b.radius, be.radMant, be.radExp);
                blocksExp.push_back(be);
            }
        }

        maxRadD = localMaxRadF32;
        fitsFloat32 = (localMaxRadF32 > 0.0);
    }
}
