#include "pch.h"
#include "GpuMandelbrotBla.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace Mandelbrot::detail
{
    namespace
    {
        inline void NormPosD(double& m, int32_t& e) noexcept
        {
            if (!(m > 0.0) || !std::isfinite(m))
            {
                m = 0.0;
                e = kZeroExpSentinel;
                return;
            }
            int de = 0;
            m = std::frexp(m, &de);
            e += static_cast<int32_t>(de);
        }

        inline void NormComplexD(double& r, double& i, int32_t& e) noexcept
        {
            const double maxAbs = std::max(std::abs(r), std::abs(i));
            if (!(maxAbs > 0.0) || !std::isfinite(maxAbs))
            {
                r = 0.0;
                i = 0.0;
                e = kZeroExpSentinel;
                return;
            }
            int de = 0;
            std::frexp(maxAbs, &de);
            r = std::ldexp(r, -de);
            i = std::ldexp(i, -de);
            e += static_cast<int32_t>(de);
        }

        inline void AddComplexD(
            double ar, double ai, int32_t ae,
            double br, double bi, int32_t be,
            double& or_, double& oi, int32_t& oe) noexcept
        {
            if (ae <= -100000000) { or_ = br; oi = bi; oe = be; return; }
            if (be <= -100000000) { or_ = ar; oi = ai; oe = ae; return; }
            if (ae >= be)
            {
                const int32_t diff = ae - be;
                if (diff >= 60) { or_ = ar; oi = ai; oe = ae; return; }
                const double s = std::ldexp(1.0, -diff);
                or_ = ar + br * s;
                oi = ai + bi * s;
                oe = ae;
            }
            else
            {
                const int32_t diff = be - ae;
                if (diff >= 60) { or_ = br; oi = bi; oe = be; return; }
                const double s = std::ldexp(1.0, -diff);
                or_ = br + ar * s;
                oi = bi + ai * s;
                oe = be;
            }
            NormComplexD(or_, oi, oe);
        }

        inline bool PosGreaterD(double am, int32_t ae, double bm, int32_t be) noexcept
        {
            if (ae <= -100000000) return false;
            if (be <= -100000000) return true;
            if (ae != be) return ae > be;
            return am > bm;
        }

        inline bool SubPosIfGreaterD(
            double am, int32_t ae,
            double bm, int32_t be,
            double& om, int32_t& oe) noexcept
        {
            if (!PosGreaterD(am, ae, bm, be))
            {
                om = 0.0;
                oe = kZeroExpSentinel;
                return false;
            }
            if (be <= -100000000)
            {
                om = am;
                oe = ae;
                return true;
            }
            const int32_t diff = ae - be;
            if (diff >= 60)
            {
                om = am;
                oe = ae;
                return true;
            }
            om = am - std::ldexp(bm, -diff);
            oe = ae;
            NormPosD(om, oe);
            return oe > -100000000;
        }

        inline void PackComplexExpF(double r, double i, int32_t e, float& mrOut, float& miOut, int32_t& eOut) noexcept
        {
            if (e <= -100000000)
            {
                mrOut = 0.0f;
                miOut = 0.0f;
                eOut = kZeroExpSentinel;
                return;
            }
            float mr = static_cast<float>(r);
            float mi = static_cast<float>(i);
            if (std::max(std::abs(mr), std::abs(mi)) >= 1.0f)
            {
                mr *= 0.5f;
                mi *= 0.5f;
                ++e;
            }
            mrOut = mr;
            miOut = mi;
            eOut = e;
        }

        inline void PackPosExpF(double m, int32_t e, float& mOut, int32_t& eOut) noexcept
        {
            if (e <= -100000000 || !(m > 0.0))
            {
                mOut = 0.0f;
                eOut = kZeroExpSentinel;
                return;
            }
            float mf = static_cast<float>(m);
            if (mf >= 1.0f)
            {
                mf *= 0.5f;
                ++e;
            }
            mOut = mf;
            eOut = e;
        }
    }

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
        const std::atomic<bool>* cancel,
        bool forceDeepExp)
    {
        Clear();
        if (!orbit || refLength <= 8 || !(cMax > 0.0) || !std::isfinite(cMax))
        {
            return;
        }
        auto canceled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
        if (canceled()) return;

        double cMaxMant = cMax;
        int32_t cMaxExp = 0;
        NormPosD(cMaxMant, cMaxExp);
        if (cMaxExp <= -100000000) return;

        constexpr double epsTol = 2.5e-8;
        auto single = [&](double x, double y) noexcept -> BlockExpD
        {
            const double zAbs = std::hypot(x, y);
            BlockExpD b;
            b.ar = 2.0 * x;
            b.ai = 2.0 * y;
            b.aExp = 0;
            NormComplexD(b.ar, b.ai, b.aExp);
            b.br = 0.5;
            b.bi = 0.0;
            b.bExp = 1; // 0.5 * 2^1 = 1.0
            const double r = std::max(0.0, std::min({ epsTol * 2.0 * zAbs, zAbs * 0.125, (4.0 - zAbs) * 0.125 }));
            b.radMant = r;
            b.radExp = 0;
            NormPosD(b.radMant, b.radExp);
            return b;
        };

        auto merge = [&](const BlockExpD& x, const BlockExpD& y) noexcept -> BlockExpD
        {
            BlockExpD m{};
            if (x.radExp <= -100000000 || y.radExp <= -100000000 || x.aExp <= -100000000 || y.aExp <= -100000000)
            {
                return m;
            }

            double axMant = std::hypot(x.ar, x.ai);
            int32_t axExp = x.aExp;
            NormPosD(axMant, axExp);
            if (axExp <= -100000000) return m;

            double bxMant = (x.bExp > -100000000) ? std::hypot(x.br, x.bi) : 0.0;
            int32_t bxExp = (x.bExp > -100000000) ? x.bExp : kZeroExpSentinel;
            if (bxExp > -100000000) NormPosD(bxMant, bxExp);

            m.ar = y.ar * x.ar - y.ai * x.ai;
            m.ai = y.ar * x.ai + y.ai * x.ar;
            m.aExp = y.aExp + x.aExp;
            NormComplexD(m.ar, m.ai, m.aExp);

            double pr = 0.0, pi = 0.0;
            int32_t pExp = kZeroExpSentinel;
            if (x.bExp > -100000000)
            {
                pr = y.ar * x.br - y.ai * x.bi;
                pi = y.ar * x.bi + y.ai * x.br;
                pExp = y.aExp + x.bExp;
                NormComplexD(pr, pi, pExp);
            }
            AddComplexD(pr, pi, pExp, y.br, y.bi, y.bExp, m.br, m.bi, m.bExp);

            double bxCMant = 0.0;
            int32_t bxCExp = kZeroExpSentinel;
            if (bxExp > -100000000)
            {
                bxCMant = bxMant * cMaxMant;
                bxCExp = bxExp + cMaxExp;
                NormPosD(bxCMant, bxCExp);
            }

            double allowMant = 0.0;
            int32_t allowExp = kZeroExpSentinel;
            if (SubPosIfGreaterD(y.radMant, y.radExp, bxCMant, bxCExp, allowMant, allowExp))
            {
                double r2Mant = allowMant / axMant;
                int32_t r2Exp = allowExp - axExp;
                NormPosD(r2Mant, r2Exp);
                if (PosGreaterD(x.radMant, x.radExp, r2Mant, r2Exp))
                {
                    m.radMant = r2Mant;
                    m.radExp = r2Exp;
                }
                else
                {
                    m.radMant = x.radMant;
                    m.radExp = x.radExp;
                }
            }
            return m;
        };

        const BlockExpD firstFour = merge(
            merge(single(orbit[1].x, orbit[1].y), single(orbit[2].x, orbit[2].y)),
            merge(single(orbit[3].x, orbit[3].y), single(orbit[4].x, orbit[4].y)));
        if (!PosGreaterD(firstFour.radMant, firstFour.radExp, cMaxMant, cMaxExp - 3))
        {
            return;
        }

        const size_t l2Count = static_cast<size_t>(refLength - 1) / 4;
        if (l2Count == 0) return;

        std::vector<std::vector<BlockExpD>> levels(3);
        levels[2].resize(l2Count);
        double localMaxRadMant = 0.0;
        int32_t localMaxRadExp = kZeroExpSentinel;

        if (l2Count >= 512)
        {
            #pragma omp parallel
            {
                bool localCanceled = false;
                #pragma omp for schedule(static)
                for (int64_t ii = 0; ii < static_cast<int64_t>(l2Count); ++ii)
                {
                    if (localCanceled) continue;
                    if ((ii & 255) == 0 && canceled())
                    {
                        localCanceled = true;
                        continue;
                    }
                    const size_t i = static_cast<size_t>(ii);
                    const int m = static_cast<int>(i * 4) + 1;
                    const BlockExpD b0 = merge(single(orbit[m].x, orbit[m].y), single(orbit[m + 1].x, orbit[m + 1].y));
                    const BlockExpD b1 = merge(single(orbit[m + 2].x, orbit[m + 2].y), single(orbit[m + 3].x, orbit[m + 3].y));
                    levels[2][i] = merge(b0, b1);
                }
            }
            if (canceled()) return;
            for (size_t i = 0; i < l2Count; ++i)
            {
                if (PosGreaterD(levels[2][i].radMant, levels[2][i].radExp, localMaxRadMant, localMaxRadExp))
                {
                    localMaxRadMant = levels[2][i].radMant;
                    localMaxRadExp = levels[2][i].radExp;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < l2Count; ++i)
            {
                if ((i & 255) == 0 && canceled()) return;
                const int m = static_cast<int>(i * 4) + 1;
                const BlockExpD b0 = merge(single(orbit[m].x, orbit[m].y), single(orbit[m + 1].x, orbit[m + 1].y));
                const BlockExpD b1 = merge(single(orbit[m + 2].x, orbit[m + 2].y), single(orbit[m + 3].x, orbit[m + 3].y));
                levels[2][i] = merge(b0, b1);
                if (PosGreaterD(levels[2][i].radMant, levels[2][i].radExp, localMaxRadMant, localMaxRadExp))
                {
                    localMaxRadMant = levels[2][i].radMant;
                    localMaxRadExp = levels[2][i].radExp;
                }
            }
        }

        while (levels.back().size() >= 2 && levels.size() < 31)
        {
            if (canceled()) return;
            const auto& prev = levels.back();
            const size_t nextCount = prev.size() / 2;
            std::vector<BlockExpD> next(nextCount);
            if (nextCount >= 512)
            {
                #pragma omp parallel
                {
                    bool localCanceled = false;
                    #pragma omp for schedule(static)
                    for (int64_t ii = 0; ii < static_cast<int64_t>(nextCount); ++ii)
                    {
                        if (localCanceled) continue;
                        if ((ii & 255) == 0 && canceled())
                        {
                            localCanceled = true;
                            continue;
                        }
                        const size_t i = static_cast<size_t>(ii);
                        next[i] = merge(prev[2 * i], prev[2 * i + 1]);
                    }
                }
                if (canceled()) return;
                for (size_t i = 0; i < nextCount; ++i)
                {
                    if (PosGreaterD(next[i].radMant, next[i].radExp, localMaxRadMant, localMaxRadExp))
                    {
                        localMaxRadMant = next[i].radMant;
                        localMaxRadExp = next[i].radExp;
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < nextCount; ++i)
                {
                    next[i] = merge(prev[2 * i], prev[2 * i + 1]);
                    if (PosGreaterD(next[i].radMant, next[i].radExp, localMaxRadMant, localMaxRadExp))
                    {
                        localMaxRadMant = next[i].radMant;
                        localMaxRadExp = next[i].radExp;
                    }
                }
            }
            levels.push_back(std::move(next));
        }
        if (canceled() || localMaxRadExp <= -100000000) return;

        PackPosExpF(localMaxRadMant, localMaxRadExp, maxRadMant, maxRadExp);
        numLevels = static_cast<uint32_t>(levels.size());

        size_t totalBlocks = 0;
        for (uint32_t lv = 2; lv < numLevels; ++lv)
        {
            levelMeta[lv * 2 + 0] = static_cast<uint32_t>(totalBlocks);
            levelMeta[lv * 2 + 1] = static_cast<uint32_t>(levels[lv].size());
            totalBlocks += levels[lv].size();
        }

        bool buildExp = forceDeepExp;
        if (!forceDeepExp)
        {
            blocksF32.reserve(totalBlocks);
            double localMaxRadF32 = 0.0;
            for (uint32_t lv = 2; lv < numLevels; ++lv)
            {
                for (const auto& b : levels[lv])
                {
                    GpuBlaBlockF32 bf{};
                    if (b.radExp >= -113 && b.radExp <= 113 &&
                        b.aExp >= -21 && b.aExp <= 113 &&
                        b.bExp <= 113)
                    {
                        const double ar = std::ldexp(b.ar, b.aExp);
                        const double ai = std::ldexp(b.ai, b.aExp);
                        const double br = (b.bExp > -100000000) ? std::ldexp(b.br, b.bExp) : 0.0;
                        const double bi = (b.bExp > -100000000) ? std::ldexp(b.bi, b.bExp) : 0.0;
                        const double rad = std::ldexp(b.radMant, b.radExp);
                        const double maxCoeff = std::max({ std::abs(ar), std::abs(ai), std::abs(br), std::abs(bi) });
                        const double aAmp = std::hypot(ar, ai);
                        if (maxCoeff <= 1e34 && aAmp >= 1e-6 && rad >= 1e-34)
                        {
                            bf.ar = static_cast<float>(ar);
                            bf.ai = static_cast<float>(ai);
                            bf.br = static_cast<float>(br);
                            bf.bi = static_cast<float>(bi);
                            bf.radius = static_cast<float>(rad);
                            localMaxRadF32 = std::max(localMaxRadF32, rad);
                        }
                    }
                    blocksF32.push_back(bf);
                }
            }

            maxRadD = localMaxRadF32;
            fitsFloat32 = (localMaxRadF32 > 0.0);
            if (!fitsFloat32)
            {
                blocksF32.clear();
                buildExp = true;
            }
        }
        else
        {
            maxRadD = 0.0;
            fitsFloat32 = false;
        }

        if (buildExp)
        {
            blocksExp.reserve(totalBlocks);
            for (uint32_t lv = 2; lv < numLevels; ++lv)
            {
                for (const auto& b : levels[lv])
                {
                    GpuBlaBlockExp be{};
                    PackComplexExpF(b.ar, b.ai, b.aExp, be.ar, be.ai, be.aExp);
                    PackComplexExpF(b.br, b.bi, b.bExp, be.br, be.bi, be.bExp);
                    PackPosExpF(b.radMant, b.radExp, be.radMant, be.radExp);
                    blocksExp.push_back(be);
                }
            }
        }
    }
}
