#include "MoviePlan.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot
{
    bool MoviePlan::PrepareAutoTargets(const std::atomic<bool>& cancel)
    {
        Validate();
        m_autoTargets.clear();
        if (!autoRetarget) return true;
        MandelbrotEngine engine;
        auto probe = start;
        // Analyze the same small viewport at every output resolution and frame rate.
        // Target decisions depend on footage time and magnification, not render speed.
        const double scale = std::min(192.0 / width, 108.0 / height);
        probe.width = std::max(8, static_cast<int>(std::lround(width * scale)));
        probe.height = std::max(8, static_cast<int>(std::lround(height * scale)));
        probe.renderStride = 1; probe.edgeRefine = false;
        std::vector<float> iterations;
        const double range = std::log(endZoom) - std::log(start.zoom);
        const int checks = static_cast<int>(std::ceil(std::max(seconds / .5, range / std::log(1.2))));
        AutoTarget state;
        state.x = start.hpCenterX; state.y = start.hpCenterY;
        state.targetX = state.x; state.targetY = state.y;
        uint64_t seed = targetSeed;
        for (int check = 0; check < checks; ++check)
        {
            if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
            const double t = static_cast<double>(check) / checks;
            if (check != 0) state = AdvanceAutoTarget(m_autoTargets.back(), t);
            // Retarget hysteresis follows elapsed footage or a doubling in zoom,
            // including very fast movies that cross several zoom decades per second.
            if (check != 0 && (t - m_autoTargets.back().progress) * seconds < .5 &&
                (t - m_autoTargets.back().progress) * range < std::log(2.0)) continue;
            probe.zoom = check == 0 ? start.zoom : std::exp(std::log(start.zoom) + range * t);
            if (autoScaleIterations)
            {
                probe.maxIterations = MandelbrotEngine::RecommendMaxIterations(
                    probe.zoom, probe.fractalType, baseIterations);
            }
            probe.SetCenter(state.x, state.y);
            engine.Compute(probe, iterations, &cancel);
            if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
            if (check == 0 || MandelbrotEngine::ShouldRetargetZoom(probe, iterations, state.targetX, state.targetY))
            {
                // A deterministic stream of nonzero seeds leaves subframe lookups
                // independent of randomness and of the order in which they are rendered.
                seed += 0x9e3779b97f4a7c15ull;
                uint64_t sample = seed;
                sample = (sample ^ (sample >> 30)) * 0xbf58476d1ce4e5b9ull;
                sample = (sample ^ (sample >> 27)) * 0x94d049bb133111ebull;
                sample ^= sample >> 31;
                BigFloat x = state.targetX, y = state.targetY;
                const bool found = MandelbrotEngine::PickInterestingTarget(probe, iterations, x, y, sample == 0 ? 1 : sample);
                // The live picker can fall back to a distant bookmark when everything
                // is uniform. A movie must not fly across millions of viewport widths;
                // keep following the current target until visible detail is available.
                const double span = 3.0 / probe.zoom;
                if (found && std::abs((x - state.x).ToDouble()) <= span * .5 &&
                    std::abs((y - state.y).ToDouble()) <= span * probe.height / probe.width * .5)
                {
                    state.targetX = x; state.targetY = y;
                    m_autoTargets.push_back(state);
                }
                else if (check == 0) m_autoTargets.push_back(state);
            }
        }
        if (cancel.load(std::memory_order_relaxed)) { m_autoTargets.clear(); return false; }
        return true;
    }
}
