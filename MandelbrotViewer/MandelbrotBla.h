#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#if defined(_M_ARM64)
#include <arm64_neon.h>
#endif

namespace Mandelbrot::detail
{
    // Bivariate linear approximation of the perturbation recurrence: z -> A*z+B*c.
    // https://mathr.co.uk/web/deep-zoom.html
    // Conservative radii limit neglected nonlinear terms; exact replay below
    // protects sensitive trajectories when their later evolution amplifies error.
    class BlaTable
    {
    public:
        static constexpr int InitialSkip = 64;
        static constexpr int MaxOrdinaryTail = 64;

        // orbit[0..refLength] includes the last available reference state.
        template<class Point>
        void Build(const Point* orbit, int refLength, double cMax,
            const std::atomic<bool>* cancel = nullptr)
        {
            m_levels.clear();
            m_maxRadius = 0.0;
            m_refLength = 0;
            if (!orbit || refLength <= InitialSkip || !(cMax > 0.0) || !std::isfinite(cMax)) return;
            const auto canceled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
            if (canceled()) return;

            // Only build a full table when the whole viewport can skip a useful
            // initial prefix. This depends on geometry, never render timing.
            std::array<Block, InitialSkip> initial;
            for (int i = 0; i < InitialSkip; ++i) initial[i] = Single(orbit[i + 1].x, orbit[i + 1].y);
            for (int count = InitialSkip; count > 1; count /= 2)
                for (int i = 0; i < count / 2; ++i) initial[i] = Merge(initial[2 * i], initial[2 * i + 1], cMax);
            if (!(initial[0].radius > cMax)) return;

            // Publish only a complete table, including when a rebuild is canceled.
            // Levels 0 and 1 (skips < 4) are never queried in Apply; merge 4-step blocks directly.
            const size_t level2Count = static_cast<size_t>(refLength - 1) / 4;
            if (level2Count == 0) return;
            std::vector<std::vector<Block>> levels(3);
            double maxRadius = 0.0;
            levels[2].resize(level2Count);
            for (size_t i = 0; i < level2Count; ++i)
            {
                if ((i & 255) == 0 && canceled()) return;
                const int m = static_cast<int>(i * 4) + 1;
                const Block b0 = Merge(Single(orbit[m].x, orbit[m].y), Single(orbit[m + 1].x, orbit[m + 1].y), cMax);
                const Block b1 = Merge(Single(orbit[m + 2].x, orbit[m + 2].y), Single(orbit[m + 3].x, orbit[m + 3].y), cMax);
                levels[2][i] = Merge(b0, b1, cMax);
                maxRadius = std::max(maxRadius, levels[2][i].radius);
            }
            while (levels.back().size() >= 2)
            {
                if (canceled()) return;
                const auto& previous = levels.back();
                std::vector<Block> next(previous.size() / 2);
                for (size_t i = 0; i < next.size(); ++i)
                {
                    next[i] = Merge(previous[2 * i], previous[2 * i + 1], cMax);
                    maxRadius = std::max(maxRadius, next[i].radius);
                }
                levels.push_back(std::move(next));
            }
            if (canceled()) return;
            m_levels = std::move(levels);
            m_maxRadius = maxRadius;
            m_refLength = refLength;
        }

        bool Enabled() const noexcept { return m_levels.size() >= 3 && m_maxRadius > 0.0; }

        int Apply(int m, int remaining, double& x, double& y, double cx, double cy) const noexcept
        {
            // The critical initial step is never linearized. Fewer than four
            // iterations do not amortize lookup cost. Avoid squaring tiny radii.
            const double absX = std::abs(x), absY = std::abs(y);
            if (m < 1 || remaining < 4 || ((m - 1) & 3) || m > m_refLength - 4 ||
                absX >= m_maxRadius || absY >= m_maxRadius) return 0;
            const double maxAbs = std::max(absX, absY);
            const unsigned start = static_cast<unsigned>(m - 1);
            const int aligned = start ? static_cast<int>(std::countr_zero(start)) : 30;
            const int top = std::min(aligned, static_cast<int>(m_levels.size()) - 1);
            double zAbs = -1.0;
            for (int level = top; level >= 2; --level)
            {
                const int length = 1 << level;
                if (length > remaining || length > m_refLength - m) continue;
                const size_t index = start >> level;
                if (index >= m_levels[level].size()) continue;
                const Block& block = m_levels[level][index];
                if (!(maxAbs < block.radius)) continue;
                if (!(maxAbs * 1.4142135623730954 < block.radius))
                {
                    if (zAbs < 0.0) zAbs = std::hypot(x, y);
                    if (!(zAbs < block.radius)) continue;
                }
                const auto az = Multiply(block.a, {x, y}), bc = Multiply(block.b, {cx, cy});
                const double nextX = az.r + bc.r, nextY = az.i + bc.i;
                if (!std::isfinite(nextX) || !std::isfinite(nextY)) continue;
                x = nextX; y = nextY;
                return length;
            }
            return 0;
        }

        // Attempts to apply the same BLA block to two lanes sharing reference step m.
        // Returns > 0 if both lanes skip the same block, 0 if neither skips, or -1 if they diverge.
        int ApplyPair(int m, int remaining, double& x0, double& y0, double& x1, double& y1,
            double cx0, double cx1, double cy) const noexcept
        {
            if (m < 1 || remaining < 4 || ((m - 1) & 3) || m > m_refLength - 4) return 0;
            const double maxAbs0 = std::max(std::abs(x0), std::abs(y0));
            const double maxAbs1 = std::max(std::abs(x1), std::abs(y1));
            if (maxAbs0 >= m_maxRadius && maxAbs1 >= m_maxRadius) return 0;
            const double pairMinAbs = std::min(maxAbs0, maxAbs1);
            const unsigned start = static_cast<unsigned>(m - 1);
            const int aligned = start ? static_cast<int>(std::countr_zero(start)) : 30;
            const int top = std::min(aligned, static_cast<int>(m_levels.size()) - 1);
            double zAbs0 = -1.0, zAbs1 = -1.0;
            for (int level = top; level >= 2; --level)
            {
                const int length = 1 << level;
                if (length > remaining || length > m_refLength - m) continue;
                const size_t index = start >> level;
                if (index >= m_levels[level].size()) continue;
                const Block& block = m_levels[level][index];
                if (!(pairMinAbs < block.radius)) continue;
                bool ok0 = false;
                if (maxAbs0 < block.radius)
                {
                    if (maxAbs0 * 1.4142135623730954 < block.radius) ok0 = true;
                    else { if (zAbs0 < 0.0) zAbs0 = std::hypot(x0, y0); ok0 = zAbs0 < block.radius; }
                }
                bool ok1 = false;
                if (maxAbs1 < block.radius)
                {
                    if (maxAbs1 * 1.4142135623730954 < block.radius) ok1 = true;
                    else { if (zAbs1 < 0.0) zAbs1 = std::hypot(x1, y1); ok1 = zAbs1 < block.radius; }
                }
                if (!ok0 && !ok1) continue;
                if (ok0 != ok1) return -1;
                const auto az0 = Multiply(block.a, {x0, y0}), bc0 = Multiply(block.b, {cx0, cy});
                const double nx0 = az0.r + bc0.r, ny0 = az0.i + bc0.i;
                const auto az1 = Multiply(block.a, {x1, y1}), bc1 = Multiply(block.b, {cx1, cy});
                const double nx1 = az1.r + bc1.r, ny1 = az1.i + bc1.i;
                const bool fin0 = std::isfinite(nx0) && std::isfinite(ny0);
                const bool fin1 = std::isfinite(nx1) && std::isfinite(ny1);
                if (!fin0 && !fin1) continue;
                if (fin0 != fin1) return -1;
                x0 = nx0; y0 = ny0;
                x1 = nx1; y1 = ny1;
                return length;
            }
            return 0;
        }

    private:
        struct Complex { double r{}, i{}; };
        struct Block { Complex a, b; double radius{}; };
        std::vector<std::vector<Block>> m_levels;
        int m_refLength{};
        double m_maxRadius{};

        static Complex Multiply(Complex x, Complex y) noexcept
        { return {x.r * y.r - x.i * y.i, x.r * y.i + x.i * y.r}; }
        static double Abs(Complex z) noexcept { return std::hypot(z.r, z.i); }
        static bool Finite(Complex z) noexcept { return std::isfinite(z.r) && std::isfinite(z.i); }
        static Block Single(double x, double y) noexcept
        {
            const double zAbs = std::hypot(x, y);
            Block block;
            block.a = {2.0 * x, 2.0 * y}; block.b = {1.0, 0.0};
            // Nonlinearity, rebasing, and intermediate escape guards.
            block.radius = std::max(0.0, std::min({std::numeric_limits<double>::epsilon() * 2.0 * zAbs,
                zAbs * .125, (4.0 - zAbs) * .125}));
            return block;
        }
        static Block Merge(const Block& x, const Block& y, double cMax) noexcept
        {
            Block merged;
            const double ax = Abs(x.a), bx = Abs(x.b);
            if (!(x.radius > 0.0 && y.radius > 0.0 && ax > 0.0) || !std::isfinite(ax + bx)) return merged;
            merged.a = Multiply(y.a, x.a);
            const auto ybx = Multiply(y.a, x.b);
            merged.b = {ybx.r + y.b.r, ybx.i + y.b.i};
            const double allowance = y.radius - bx * cMax;
            if (allowance > 0.0 && Finite(merged.a) && Finite(merged.b))
                merged.radius = .5 * std::min(x.radius, allowance / ax);
            return merged;
        }
    };

    enum class ReplayReason { None, Rebase, LongTail };
    struct PerturbationResult
    {
        int iterations{};
        double x{}, y{};
        ReplayReason replay{ReplayReason::None};
    };

    // Separate instantiations keep the ordinary reference/replay loop free of
    // approximation checks. Arithmetic order matches the original deep solver.
    template<bool UseBla, class Point>
    inline PerturbationResult FinishMandelbrotPerturbationLane(const Point* orbit, int refLength,
        int maxIter, double dcx, double dcy, const BlaTable& bla,
        double dx, double dy, int m, int iter, bool usedBla, int ordinaryTail) noexcept
    {
        while (iter < maxIter)
        {
            double rx = orbit[m].x, ry = orbit[m].y;
            const double zx = rx + dx, zy = ry + dy;
            const double mod2 = zx * zx + zy * zy;
            if (mod2 > 16.0) return {iter, zx, zy};

            const double dz2 = dx * dx + dy * dy;
            if constexpr (UseBla)
                if (usedBla && m > 0 && mod2 < dz2) return {0, 0.0, 0.0, ReplayReason::Rebase};
            if (m == refLength || (m > 0 && mod2 < dz2))
            {
                dx = zx; dy = zy; m = 0;
                rx = 0.0; ry = 0.0;
            }
            if constexpr (UseBla)
            {
                const int skipped = bla.Apply(m, maxIter - iter, dx, dy, dcx, dcy);
                if (skipped)
                {
                    m += skipped; iter += skipped; usedBla = true; ordinaryTail = 0;
                    continue;
                }
                if (usedBla && ++ordinaryTail > BlaTable::MaxOrdinaryTail)
                    return {0, 0.0, 0.0, ReplayReason::LongTail};
            }
            const double sx = rx + zx, sy = ry + zy;
            const double nextDx = sx * dx - sy * dy + dcx;
            const double nextDy = sx * dy + sy * dx + dcy;
            dx = nextDx; dy = nextDy;
            ++m; ++iter;
        }
        return {iter};
    }

    template<bool UseBla, class Point>
    inline PerturbationResult EvalMandelbrotPerturbation(const Point* orbit, int refLength,
        int maxIter, double dcx, double dcy, const BlaTable& bla) noexcept
    {
        return FinishMandelbrotPerturbationLane<UseBla>(orbit, refLength, maxIter, dcx, dcy, bla,
            0.0, 0.0, 0, 0, false, 0);
    }

#if defined(_M_ARM64)
    template<bool UseBla, class Point>
    inline void EvalMandelbrotPerturbationPair(const Point* orbit, int refLength,
        int maxIter, double dcx0, double dcx1, double dcy, const BlaTable& bla,
        PerturbationResult& out0, PerturbationResult& out1) noexcept
    {
        const double dcxArr[2]{ dcx0, dcx1 };
        const float64x2_t dcx = vld1q_f64(dcxArr);
        const float64x2_t dcyVec = vdupq_n_f64(dcy);
        const float64x2_t limit = vdupq_n_f64(16.0);
        float64x2_t dx = vdupq_n_f64(0.0), dy = vdupq_n_f64(0.0);
        int m = 0, iter = 0;
        bool usedBla = false;
        int ordinaryTail = 0;

        while (iter < maxIter)
        {
            float64x2_t rx = vdupq_n_f64(orbit[m].x);
            float64x2_t ry = vdupq_n_f64(orbit[m].y);
            const float64x2_t zx = vaddq_f64(rx, dx);
            const float64x2_t zy = vaddq_f64(ry, dy);
            const float64x2_t mod2 = vaddq_f64(vmulq_f64(zx, zx), vmulq_f64(zy, zy));
            const uint64x2_t escaped = vcgtq_f64(mod2, limit);
            if (vmaxvq_u32(vreinterpretq_u32_u64(escaped))) break;

            const float64x2_t dz2 = vaddq_f64(vmulq_f64(dx, dx), vmulq_f64(dy, dy));
            if (m > 0)
            {
                const uint64x2_t rebase = vcltq_f64(mod2, dz2);
                if (vmaxvq_u32(vreinterpretq_u32_u64(rebase)))
                {
                    if constexpr (UseBla)
                    {
                        if (usedBla) break;
                    }
                    if (!vminvq_u32(vreinterpretq_u32_u64(rebase))) break;
                    dx = zx; dy = zy; m = 0;
                    rx = vdupq_n_f64(0.0); ry = vdupq_n_f64(0.0);
                }
            }
            if (m == refLength)
            {
                dx = zx; dy = zy; m = 0;
                rx = vdupq_n_f64(0.0); ry = vdupq_n_f64(0.0);
            }
            if constexpr (UseBla)
            {
                if (m >= 1 && maxIter - iter >= 4 && ((m - 1) & 3) == 0 && m <= refLength - 4)
                {
                    double dxArr[2], dyArr[2];
                    vst1q_f64(dxArr, dx); vst1q_f64(dyArr, dy);
                    const int skipped = bla.ApplyPair(m, maxIter - iter,
                        dxArr[0], dyArr[0], dxArr[1], dyArr[1], dcx0, dcx1, dcy);
                    if (skipped < 0) break;
                    if (skipped > 0)
                    {
                        dx = vld1q_f64(dxArr); dy = vld1q_f64(dyArr);
                        m += skipped; iter += skipped; usedBla = true; ordinaryTail = 0;
                        continue;
                    }
                }
                if (usedBla && ordinaryTail + 1 > BlaTable::MaxOrdinaryTail)
                {
                    out0 = {0, 0.0, 0.0, ReplayReason::LongTail};
                    out1 = {0, 0.0, 0.0, ReplayReason::LongTail};
                    return;
                }
                if (usedBla) ++ordinaryTail;
            }
            const float64x2_t sx = vaddq_f64(rx, zx);
            const float64x2_t sy = vaddq_f64(ry, zy);
            const float64x2_t nextDx = vaddq_f64(vsubq_f64(vmulq_f64(sx, dx), vmulq_f64(sy, dy)), dcx);
            const float64x2_t nextDy = vaddq_f64(vaddq_f64(vmulq_f64(sx, dy), vmulq_f64(sy, dx)), dcyVec);
            dx = nextDx; dy = nextDy;
            ++m; ++iter;
        }

        if (iter >= maxIter)
        {
            out0 = {iter};
            out1 = {iter};
            return;
        }
        double dxArr[2], dyArr[2];
        vst1q_f64(dxArr, dx); vst1q_f64(dyArr, dy);
        out0 = FinishMandelbrotPerturbationLane<UseBla>(orbit, refLength, maxIter,
            dcx0, dcy, bla, dxArr[0], dyArr[0], m, iter, usedBla, ordinaryTail);
        out1 = FinishMandelbrotPerturbationLane<UseBla>(orbit, refLength, maxIter,
            dcx1, dcy, bla, dxArr[1], dyArr[1], m, iter, usedBla, ordinaryTail);
    }
#endif
}
