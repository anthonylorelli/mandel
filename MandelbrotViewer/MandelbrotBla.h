#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

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
            std::vector<std::vector<Block>> levels;
            double maxRadius = 0.0;
            levels.emplace_back(static_cast<size_t>(refLength - 1));
            for (int m = 1; m < refLength; ++m)
            {
                if ((m & 1023) == 0 && canceled()) return;
                levels[0][m - 1] = Single(orbit[m].x, orbit[m].y);
            }
            while (levels.back().size() >= 2)
            {
                if (canceled()) return;
                const auto& previous = levels.back();
                std::vector<Block> next(previous.size() / 2);
                for (size_t i = 0; i < next.size(); ++i)
                {
                    next[i] = Merge(previous[2 * i], previous[2 * i + 1], cMax);
                    if (levels.size() >= 2) maxRadius = std::max(maxRadius, next[i].radius);
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
            if (m < 1 || remaining < 4 || ((m - 1) & 3) || m > m_refLength - 4 ||
                std::abs(x) >= m_maxRadius || std::abs(y) >= m_maxRadius) return 0;
            const unsigned start = static_cast<unsigned>(m - 1);
            const int aligned = start ? static_cast<int>(std::countr_zero(start)) : 30;
            const int top = std::min(aligned, static_cast<int>(m_levels.size()) - 1);
            const double zAbs = std::hypot(x, y);
            for (int level = top; level >= 2; --level)
            {
                const int length = 1 << level;
                if (length > remaining || length > m_refLength - m) continue;
                const size_t index = start >> level;
                if (index >= m_levels[level].size()) continue;
                const Block& block = m_levels[level][index];
                if (!(zAbs < block.radius)) continue;
                const auto az = Multiply(block.a, {x, y}), bc = Multiply(block.b, {cx, cy});
                const double nextX = az.r + bc.r, nextY = az.i + bc.i;
                if (!std::isfinite(nextX) || !std::isfinite(nextY)) continue;
                x = nextX; y = nextY;
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
    inline PerturbationResult EvalMandelbrotPerturbation(const Point* orbit, int refLength,
        int maxIter, double dcx, double dcy, const BlaTable& bla) noexcept
    {
        double dx = 0.0, dy = 0.0;
        int m = 0, iter = 0;
        bool usedBla = false;
        int ordinaryTail = 0;
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
}
