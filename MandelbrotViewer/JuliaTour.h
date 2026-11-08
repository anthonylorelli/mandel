#pragma once
#include "MandelbrotEngine.h"
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
    // Multiplier paths favor the exterior of the main cardioid and period-two bulb,
    // with occasional inward visits. The regions meet at c=-.75 for continuous transfers.
    class JuliaTour
    {
    public:
        using Point = std::complex<double>;
        static constexpr double MaxParameterStep = 0.006;
        static constexpr double MaxMultiplierRadius = 1.16;

        Point Start(double x, double y, uint64_t seed, FractalType fractalType = FractalType::Mandelbrot)
        {
            m_random.seed(seed);
            m_fractalType = fractalType;
            if (m_fractalType == FractalType::BurningShip)
            {
                const bool bounded = std::isfinite(x) && std::isfinite(y) &&
                    x >= -2.0 && x <= 1.0 && y >= -0.8 && y <= 1.5;
                m_fromPoint = bounded ? Point(x, y) : Point(-0.45, 0.5);
                m_current = m_fromPoint;
                m_inFlames = true;
                m_flameStopsRemaining = std::uniform_int_distribution<int>(4, 6)(m_random);
                m_flameSide = (m_fromPoint.real() < -0.5) ? -1 : 1;
                PlanBurningShipSegment();
                return m_current;
            }
            const Point c(x, y);
            const double q = (x - 0.25) * (x - 0.25) + y * y;
            const bool bounded = std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 2.0 && std::abs(y) <= 2.0;
            m_bulb = false;
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
                        m_from = selected;
                    else m_bulb = false;
                }
            }
            m_current = Parameter(m_from);
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
                if (m_fractalType == FractalType::BurningShip)
                {
                    m_fromPoint = m_targetPoint;
                    PlanBurningShipSegment();
                    continue;
                }
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
        int BurningShipEscapeIterations(Point c, int maxIter = 512) const
        {
            double zr = 0.0, zi = 0.0;
            for (int iter = 0; iter < maxIter; ++iter)
            {
                const double nextR = zr * zr - zi * zi + c.real();
                zi = -2.0 * std::abs(zr * zi) + c.imag();
                zr = nextR;
                if (zr * zr + zi * zi > 4.0) return iter;
            }
            return -1;
        }

        Point RefineBurningShipRay(Point insideAnchor, Point outsideAnchor, bool wantInterior)
        {
            constexpr int kSteps = 18;
            Point lo = insideAnchor, hi = outsideAnchor;
            bool foundBoundary = false;
            int bestEscapingIter = -1;
            Point bestEscapingPt = 0.5 * (insideAnchor + outsideAnchor);

            // Walk from outer sky inward so we catch the outer/middle flame curtains first.
            int prevIter = BurningShipEscapeIterations(outsideAnchor);
            Point prevPt = outsideAnchor;
            for (int i = kSteps - 1; i >= 0; --i)
            {
                const double s = static_cast<double>(i) / kSteps;
                const Point pt = insideAnchor + s * (outsideAnchor - insideAnchor);
                const int iter = BurningShipEscapeIterations(pt);
                if (iter >= 14 && iter > bestEscapingIter)
                {
                    bestEscapingIter = iter;
                    bestEscapingPt = pt;
                }
                const bool prevHighOrIn = (prevIter < 0 || prevIter >= 36);
                const bool curHighOrIn = (iter < 0 || iter >= 36);
                if (curHighOrIn && !prevHighOrIn)
                {
                    lo = pt;
                    hi = prevPt;
                    foundBoundary = true;
                    // 70% chance to stop at the first outer/middle flame curtain; 30% to continue deeper.
                    if (std::bernoulli_distribution(0.70)(m_random)) break;
                }
                prevIter = iter;
                prevPt = pt;
            }

            if (!foundBoundary)
            {
                return (bestEscapingIter >= 14 && !wantInterior) ? bestEscapingPt : insideAnchor + 0.35 * (outsideAnchor - insideAnchor);
            }

            Point bestExterior = hi;
            int bestExtIter = BurningShipEscapeIterations(hi);
            for (int step = 0; step < 12; ++step)
            {
                const Point mid = 0.5 * (lo + hi);
                const int iter = BurningShipEscapeIterations(mid);
                if (iter < 0)
                {
                    lo = mid;
                }
                else
                {
                    if (iter >= 16)
                    {
                        bestExterior = mid;
                        bestExtIter = iter;
                    }
                    if (iter >= 64) hi = mid;
                    else lo = mid;
                }
            }
            if (wantInterior) return lo;
            return (bestExtIter >= 14) ? bestExterior : hi;
        }

        void PlanBurningShipSegment()
        {
            if (m_flameStopsRemaining <= 0)
            {
                m_inFlames = !m_inFlames;
                m_flameStopsRemaining = m_inFlames
                    ? std::uniform_int_distribution<int>(4, 6)(m_random)
                    : std::uniform_int_distribution<int>(2, 3)(m_random);
            }
            --m_flameStopsRemaining;

            const bool wantInterior = std::bernoulli_distribution(0.18)(m_random);
            if (m_inFlames)
            {
                // Explore the towering flames, sails, and central mast above the ship (y in [0.58, 1.08]).
                // Along x = -0.50, z_2 = (-0.25 - y^2, 0) stays on the bounded real axis for all y <= 1.05,
                // providing an exact interior mast spine from which rays sweep outward through the flames.
                if (std::bernoulli_distribution(0.25)(m_random))
                {
                    // High central spire & rigging near (-0.50, 0.88..1.08)
                    const double yIn = std::uniform_real_distribution<double>(0.84, 1.00)(m_random);
                    const double xOut = -0.50 + std::uniform_real_distribution<double>(-0.22, 0.22)(m_random);
                    const double yOut = std::uniform_real_distribution<double>(1.08, 1.18)(m_random);
                    m_targetPoint = RefineBurningShipRay(Point(-0.50, yIn), Point(xOut, yOut), wantInterior);
                }
                else
                {
                    if (std::bernoulli_distribution(0.65)(m_random)) m_flameSide = -m_flameSide;
                    const double y = std::uniform_real_distribution<double>(0.60, 1.02)(m_random);
                    const double dy = std::uniform_real_distribution<double>(-0.05, 0.08)(m_random);
                    const double reach = (m_flameSide < 0) ? 0.58 : 0.48;
                    const Point insideAnchor(-0.50, y);
                    const Point outsideAnchor(-0.50 + m_flameSide * reach, std::clamp(y + dy, 0.58, 1.12));
                    m_targetPoint = RefineBurningShipRay(insideAnchor, outsideAnchor, wantInterior);
                }
            }
            else
            {
                // Visit the upper hull shoreline, western deck/superstructure, or lower keel ribs.
                const double x0 = std::uniform_real_distribution<double>(-1.18, 0.05)(m_random);
                const bool upperDeck = std::bernoulli_distribution(0.70)(m_random);
                const double yOut = upperDeck
                    ? std::uniform_real_distribution<double>(0.36, 0.56)(m_random)
                    : std::uniform_real_distribution<double>(-0.26, -0.10)(m_random);
                const double dx = std::uniform_real_distribution<double>(-0.12, 0.12)(m_random);
                m_targetPoint = RefineBurningShipRay(Point(x0, 0.0), Point(x0 + dx, yOut), wantInterior);
            }

            const Point delta = m_targetPoint - m_fromPoint;
            const Point perp(-delta.imag(), delta.real());
            const double arcSign = (m_fromPoint.real() + m_targetPoint.real() < -1.0) ? -1.0 : 1.0;
            m_controlPoint = 0.5 * (m_fromPoint + m_targetPoint) + 0.16 * arcSign * perp;
            if (m_inFlames)
            {
                m_controlPoint = Point(
                    std::clamp(m_controlPoint.real(), -1.05, 0.05),
                    std::max(0.58, m_controlPoint.imag()));
            }
            m_duration = std::uniform_real_distribution<double>(4.5, 6.5)(m_random) + 4.0 * std::abs(delta);
            m_progress = 0.0;
        }
        static Point InDisk(Point p, double radius)
        {
            const double length = std::abs(p);
            return length > radius ? p * (radius / length) : p;
        }

        void BeginVisit()
        {
            m_wanders = std::uniform_int_distribution<int>(8, 11)(m_random);
            m_direction = std::uniform_int_distribution<int>(0, 1)(m_random) ? 1 : -1;
            m_exteriorStopsRemaining = std::uniform_int_distribution<int>(3, 5)(m_random);
        }

        double NextDetailAngle()
        {
            // Aim near low-order bifurcations for lobes and fine branches, with
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
                // Keep successive detail stops nearby, leaving broader moves to inward visits.
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

        bool PlanExteriorStop()
        {
            for (int attempt = 0; attempt < 24; ++attempt)
            {
                // Keep most stops close to detailed edges, varying the outward depth
                // so an exterior run also reveals broader disconnected forms.
                const double depth = std::uniform_real_distribution<double>(0.0, 1.0)(m_random);
                const double radius = 1.02 + (MaxMultiplierRadius - 1.02) * depth * depth;
                const double angle = NextDetailAngle();
                const Point multiplier = std::polar(radius, angle);
                const Point c = Parameter(multiplier);
                double zr = 0.0, zi = 0.0;
                bool escapes = false;
                for (int iter = 0; iter < 512; ++iter)
                {
                    const double nextR = zr * zr - zi * zi + c.real();
                    zi = (m_fractalType == FractalType::BurningShip)
                        ? (-2.0 * std::abs(zr * zi) + c.imag())
                        : (2.0 * zr * zi + c.imag());
                    zr = nextR;
                    if (zr * zr + zi * zi > 4.0) { escapes = true; break; }
                }
                // Beyond one component's multiplier disk can still lie in an adjoining
                // bulb. An escaping critical orbit confirms a truly exterior destination.
                if (!escapes) continue;
                m_targetRadius = radius;
                m_targetAngle = angle;
                m_target = multiplier;
                return true;
            }
            return false;
        }

        Point Evaluate(double progress) const
        {
            if (m_fractalType == FractalType::BurningShip)
            {
                if (progress <= 0.0) return m_fromPoint;
                if (progress >= 1.0) return m_targetPoint;
                const double t = progress * progress * progress * (10.0 + progress * (-15.0 + 6.0 * progress));
                const double u = 1.0 - t;
                return u * u * m_fromPoint + 2.0 * u * t * m_controlPoint + t * t * m_targetPoint;
            }
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
            m_crossing = m_wanders == 0;
            m_fromRadius = std::abs(m_from);
            m_fromAngle = std::arg(m_from);
            bool exteriorStop = false, interiorStop = false;
            if (m_crossing)
            {
                m_target = Point(m_bulb ? 1.0 : -1.0);
                m_targetRadius = 1.0;
                m_targetAngle = m_fromAngle + std::remainder(std::arg(m_target) - m_fromAngle,
                    2.0 * std::numbers::pi);
            }
            else if (m_exteriorStopsRemaining > 0 && PlanExteriorStop())
            {
                --m_exteriorStopsRemaining;
                --m_wanders;
                exteriorStop = true;
            }
            else
            {
                if (m_exteriorStopsRemaining > 0 || !std::bernoulli_distribution(0.35)(m_random))
                {
                    m_targetRadius = std::uniform_real_distribution<double>(0.95, 0.995)(m_random);
                    m_targetAngle = NextDetailAngle();
                }
                else
                {
                    // Brief, infrequent inward visits add contrast without making
                    // the tour repeatedly dwell on large filled Julia interiors.
                    interiorStop = true;
                    m_targetRadius = std::bernoulli_distribution(0.20)(m_random)
                        ? std::uniform_real_distribution<double>(0.25, 0.55)(m_random)
                        : std::uniform_real_distribution<double>(0.55, 0.82)(m_random);
                    m_direction = std::uniform_int_distribution<int>(0, 1)(m_random) ? 1 : -1;
                    m_targetAngle = m_fromAngle + m_direction *
                        std::uniform_real_distribution<double>(0.55, 2.4)(m_random);
                }
                m_target = std::polar(m_targetRadius, m_targetAngle);
                m_exteriorStopsRemaining = std::uniform_int_distribution<int>(3, 5)(m_random);
                --m_wanders;
            }
            m_duration = (exteriorStop ? std::uniform_real_distribution<double>(6.0, 8.0)(m_random)
                : interiorStop ? std::uniform_real_distribution<double>(3.0, 4.0)(m_random)
                : std::uniform_real_distribution<double>(4.0, 6.0)(m_random)) +
                1.5 * std::abs(m_targetAngle - m_fromAngle);
            m_progress = 0.0;
        }

        std::mt19937_64 m_random{ 0 };
        Point m_from{ -1.0 }, m_target{}, m_current{ -0.75 };
        double m_fromRadius{}, m_targetRadius{}, m_fromAngle{}, m_targetAngle{};
        double m_progress{}, m_duration{ 5.0 };
        int m_wanders{}, m_direction{ 1 }, m_exteriorStopsRemaining{};
        bool m_bulb{}, m_crossing{};
        FractalType m_fractalType{ FractalType::Mandelbrot };
        Point m_fromPoint{ -0.45, 0.5 }, m_controlPoint{}, m_targetPoint{};
        int m_flameStopsRemaining{}, m_flameSide{ 1 };
        bool m_inFlames{ true };
    };
}
