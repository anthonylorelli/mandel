#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <random>
#include <utility>

namespace Mandelbrot
{
    // Multiplier paths explore the main cardioid and period-two bulb, with brief
    // excursions beyond their edges. The regions meet at c=-.75 for continuous transfers.
    class JuliaTour
    {
    public:
        using Point = std::complex<double>;
        static constexpr double MaxParameterStep = 0.006;
        static constexpr double MaxMultiplierRadius = 1.16;

        Point Start(double x, double y, uint64_t seed)
        {
            m_random.seed(seed);
            const Point c(x, y);
            const double q = (x - 0.25) * (x - 0.25) + y * y;
            const bool bounded = std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 2.0 && std::abs(y) <= 2.0;
            m_bulb = false;
            m_returnFromExterior = false;
            if (bounded && q * (q + x - 0.25) <= 0.25 * y * y)
            {
                const Point root = std::sqrt(Point(1.0) - 4.0 * c);
                m_from = std::abs(Point(1.0) - root) <= std::abs(Point(1.0) + root)
                    ? Point(1.0) - root : Point(1.0) + root;
                m_from = InDisk(m_from, 1.0);
            }
            else if (bounded && std::abs(c + Point(1.0)) <= 0.25)
            {
                m_bulb = true;
                m_from = InDisk(4.0 * (c + Point(1.0)), 1.0);
            }
            else
            {
                m_from = Point(-1.0);
                if (bounded)
                {
                    // Preserve a stopped tour's exterior parameter when restarting nearby.
                    const Point root = std::sqrt(Point(1.0) - 4.0 * c);
                    const Point cardioid = std::abs(Point(1.0) - root) <= std::abs(Point(1.0) + root)
                        ? Point(1.0) - root : Point(1.0) + root;
                    const Point bulb = 4.0 * (c + Point(1.0));
                    m_bulb = std::abs(bulb) < std::abs(cardioid);
                    const Point selected = m_bulb ? bulb : cardioid;
                    if (std::abs(selected) <= MaxMultiplierRadius)
                    {
                        m_from = selected;
                        m_returnFromExterior = true;
                    }
                    else m_bulb = false;
                }
            }
            m_current = Parameter(m_from);
            m_exteriorCooldown = std::uniform_int_distribution<int>(3, 6)(m_random);
            BeginVisit();
            PlanSegment();
            return m_current;
        }

        Point Current() const { return m_current; }

        Point Advance(double seconds)
        {
            if (!(seconds > 0.0) || !std::isfinite(seconds)) return m_current;
            seconds = std::min(seconds, 0.1);
            const Point previous = m_current;
            while (seconds > 0.0)
            {
                const double remaining = (1.0 - m_progress) * m_duration;
                const double elapsed = std::min(seconds, remaining);
                double next = elapsed >= remaining ? 1.0 : m_progress + elapsed / m_duration;
                if (std::abs(Evaluate(next) - previous) > MaxParameterStep)
                {
                    double lo = m_progress, hi = next;
                    for (int i = 0; i < 40; ++i)
                    {
                        const double mid = (lo + hi) * 0.5;
                        if (std::abs(Evaluate(mid) - previous) <= MaxParameterStep) lo = mid;
                        else hi = mid;
                    }
                    m_progress = lo;
                    m_current = Evaluate(lo);
                    break;
                }
                m_progress = next;
                m_current = Evaluate(next);
                seconds -= elapsed;
                if (next < 1.0) break;
                m_from = m_target;
                if (m_crossing)
                {
                    m_bulb = !m_bulb;
                    m_from = Point(m_bulb ? 1.0 : -1.0);
                    BeginVisit();
                }
                PlanSegment();
            }
            return m_current;
        }

    private:
        static Point InDisk(Point p, double radius)
        {
            const double length = std::abs(p);
            return length > radius ? p * (radius / length) : p;
        }

        void BeginVisit()
        {
            m_wanders = std::uniform_int_distribution<int>(4, 7)(m_random);
            m_direction = std::uniform_int_distribution<int>(0, 1)(m_random) ? 1 : -1;
            m_detailStopsRemaining = std::uniform_int_distribution<int>(1, 2)(m_random);
        }

        double NextDetailAngle()
        {
            // Near roots of unity the attracting cycle is close to bifurcation,
            // producing lobes and fine branches. Favor lower-order rotations, with
            // small random offsets so successive visits need not look identical.
            static constexpr std::array<std::pair<int, int>, 18> rotations{
                std::pair{0, 1}, {1, 2}, {1, 3}, {2, 3}, {1, 4}, {3, 4},
                {1, 5}, {2, 5}, {3, 5}, {4, 5}, {1, 6}, {5, 6},
                {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7} };
            constexpr double turn = 2.0 * std::numbers::pi;
            std::array<double, rotations.size()> deltas{}, weights{};
            double total = 0.0;
            for (size_t i = 0; i < rotations.size(); ++i)
            {
                const auto [numerator, denominator] = rotations[i];
                const double angle = turn * numerator / denominator;
                deltas[i] = std::fmod(m_direction * (angle - m_fromAngle) + turn, turn);
                // Keep successive detail stops nearby, leaving broader moves to excursions.
                if (deltas[i] >= 0.35 && deltas[i] <= 1.65)
                    total += weights[i] = 8.0 - denominator;
            }
            double choice = std::uniform_real_distribution<double>(0.0, total)(m_random);
            double delta = 1.0;
            for (size_t i = 0; i < weights.size(); ++i)
            {
                if (weights[i] > 0.0 && choice < weights[i]) { delta = deltas[i]; break; }
                choice -= weights[i];
            }
            return m_fromAngle + m_direction * delta +
                std::uniform_real_distribution<double>(-0.035, 0.035)(m_random);
        }

        Point Parameter(Point multiplier) const
        {
            return m_bulb ? Point(-1.0) + multiplier * 0.25
                : multiplier * 0.5 - multiplier * multiplier * 0.25;
        }

        bool PlanExteriorExcursion()
        {
            if (m_exteriorCooldown > 0 || m_fromRadius < 0.90) return false;
            for (int attempt = 0; attempt < 12; ++attempt)
            {
                const double radius = std::uniform_real_distribution<double>(1.04, MaxMultiplierRadius)(m_random);
                const double angle = m_fromAngle + std::uniform_real_distribution<double>(-0.75, 0.75)(m_random);
                const Point multiplier = std::polar(radius, angle);
                const Point c = Parameter(multiplier);
                Point z{};
                bool escapes = false;
                for (int iter = 0; iter < 512; ++iter)
                {
                    z = z * z + c;
                    if (std::norm(z) > 4.0) { escapes = true; break; }
                }
                // Beyond one component's multiplier disk can still lie in an adjoining
                // bulb. An escaping critical orbit confirms a truly exterior destination.
                if (!escapes) continue;
                m_targetRadius = radius;
                m_targetAngle = angle;
                m_target = multiplier;
                m_returnFromExterior = true;
                m_exteriorCooldown = std::uniform_int_distribution<int>(3, 6)(m_random);
                return true;
            }
            return false;
        }

        Point Evaluate(double progress) const
        {
            if (progress <= 0.0) return Parameter(m_from);
            if (progress >= 1.0) return Parameter(m_target);
            // Vary radius and angle together for edge stretches and inward/outward detours.
            // Interpolated radii stay in the bounded tour neighborhood. Quintic easing keeps
            // velocity and acceleration zero at joins, giving shapes time to settle into view.
            const double t = progress * progress * progress * (10.0 + progress * (-15.0 + 6.0 * progress));
            return Parameter(std::polar(std::lerp(m_fromRadius, m_targetRadius, t),
                std::lerp(m_fromAngle, m_targetAngle, t)));
        }

        void PlanSegment()
        {
            m_crossing = !m_returnFromExterior && m_wanders == 0;
            m_fromRadius = std::abs(m_from);
            m_fromAngle = std::arg(m_from);
            bool shortDetour = false;
            if (m_returnFromExterior)
            {
                m_targetRadius = std::uniform_real_distribution<double>(0.92, 0.98)(m_random);
                m_targetAngle = m_fromAngle + std::uniform_real_distribution<double>(-0.12, 0.12)(m_random);
                m_target = std::polar(m_targetRadius, m_targetAngle);
                m_returnFromExterior = false;
                shortDetour = true;
            }
            else if (m_crossing)
            {
                m_target = Point(m_bulb ? 1.0 : -1.0);
                m_targetRadius = 1.0;
                m_targetAngle = m_fromAngle + std::remainder(std::arg(m_target) - m_fromAngle,
                    2.0 * std::numbers::pi);
            }
            else if (PlanExteriorExcursion()) shortDetour = true;
            else
            {
                if (m_detailStopsRemaining > 0)
                {
                    m_targetRadius = 0.90 + 0.095 * std::sqrt(std::uniform_real_distribution<double>(0.0, 1.0)(m_random));
                    m_targetAngle = NextDetailAngle();
                    --m_detailStopsRemaining;
                }
                else
                {
                    // Limit consecutive edge stops so every component visit explores its
                    // interior too. Mix moderate excursions with occasional deeper dives.
                    m_targetRadius = std::bernoulli_distribution(0.35)(m_random)
                        ? std::uniform_real_distribution<double>(0.20, 0.55)(m_random)
                        : std::uniform_real_distribution<double>(0.55, 0.82)(m_random);
                    m_direction = std::uniform_int_distribution<int>(0, 1)(m_random) ? 1 : -1;
                    m_targetAngle = m_fromAngle + m_direction *
                        std::uniform_real_distribution<double>(0.55, 2.4)(m_random);
                    m_detailStopsRemaining = std::uniform_int_distribution<int>(1, 3)(m_random);
                }
                m_target = std::polar(m_targetRadius, m_targetAngle);
                --m_wanders;
                if (m_exteriorCooldown > 0) --m_exteriorCooldown;
            }
            m_duration = (shortDetour ? std::uniform_real_distribution<double>(3.0, 4.5)(m_random)
                : std::uniform_real_distribution<double>(5.0, 7.0)(m_random)) +
                1.5 * std::abs(m_targetAngle - m_fromAngle);
            m_progress = 0.0;
        }

        std::mt19937_64 m_random{ 0 };
        Point m_from{ -1.0 }, m_target{}, m_current{ -0.75 };
        double m_fromRadius{}, m_targetRadius{}, m_fromAngle{}, m_targetAngle{};
        double m_progress{}, m_duration{ 5.0 };
        int m_wanders{}, m_direction{ 1 }, m_detailStopsRemaining{};
        int m_exteriorCooldown{};
        bool m_bulb{}, m_crossing{}, m_returnFromExterior{};
    };
}
