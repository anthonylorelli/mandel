#include "pch.h"
#include "JuliaSolver.h"
#include "FractalFormulas.h"
#include "RenderPass.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Mandelbrot
{
    double ComputeJuliaSet(
        const FractalConfig& config,
        std::vector<float>& outIterations,
        std::vector<float>& sampleWeights,
        const std::atomic<bool>* cancelToken,
        std::vector<SubpixelSamples>* subpixelSamples)
    {
        const auto start = std::chrono::steady_clock::now();
        if (subpixelSamples) subpixelSamples->clear();
        if (config.width <= 0 || config.height <= 0) return 0.0;
        if (config.fractalType != FractalType::Mandelbrot && config.fractalType != FractalType::BurningShip)
            throw std::invalid_argument("Julia view requires the Mandelbrot or Burning Ship formula.");
        const bool burningShip = (config.fractalType == FractalType::BurningShip);
        const double cx = config.juliaX.ToDouble(), cy = config.juliaY.ToDouble();
        if (!std::isfinite(cx) || !std::isfinite(cy)) throw std::invalid_argument("Invalid Julia parameter.");
        const double zoom = std::clamp(config.zoom, 1e-12, MaxZoom);
        const double step = 3.0 / zoom / config.width;
        // R >= 4 and R^2 >= 4|c| guarantees escape, including parameters outside Mandelbrot/Burning Ship.
        const double bailout2 = std::max(16.0, 4.0 * std::hypot(cx, cy));
        if (!std::isfinite(bailout2)) throw std::invalid_argument("Julia parameter is too large.");
        const bool deep = zoom >= DeepZoomThreshold;
        const int limbs = std::clamp((static_cast<int>(std::ceil((2.0 * std::log2(zoom) + 96.0) / 32.0)) + 1) & ~1, 4, BigFloat::N);
        auto canceled = [&] { return cancelToken && cancelToken->load(std::memory_order_relaxed); };
        BigFloat centerX;
        BigFloat centerY;
        config.ResolveHpCenter(centerX, centerY);
        constexpr double invLn2 = 1.44269504088896340736;
        auto finish = [&](double zr, double zi, int iter) -> float
        {
            if (iter >= config.maxIterations) return -1.0f;
            if (!config.smooth) return static_cast<float>(iter);
            return detail::ComputeSmoothEscapeIteration(
                config.fractalType, iter, zr, zi, 0.0, 0.0, cx, cy, invLn2);
        };
        auto seed = [&](double px, double py)
        {
            return std::pair{ centerX + BigFloat((px + 0.5 - config.width * 0.5) * step),
                centerY + BigFloat((config.height * 0.5 - py - 0.5) * step) };
        };
        auto stepBigFloatJulia = [&](BigFloat& zr, BigFloat& zi, const BigFloat& r2, const BigFloat& i2)
        {
            const auto product = zr.Mul(zi, limbs);
            zr = r2 - i2 + config.juliaX;
            if (burningShip)
            {
                const auto twoAbs = (product + product).Abs();
                zi = config.juliaY - twoAbs;
            }
            else
            {
                zi = product + product + config.juliaY;
            }
        };
        auto exactPixel = [&](double px, double py) -> float
        {
            auto [zr, zi] = seed(px, py);
            const BigFloat limit(bailout2), one(1.0);
            for (int iter = 0; iter < config.maxIterations; ++iter)
            {
                if (canceled()) return -1.0f;
                const auto r2 = zr.Sqr(limbs), i2 = zi.Sqr(limbs);
                const auto mag = r2 + i2;
                if (iter == 0 && config.juliaX.IsZero() && config.juliaY.IsZero() && BigFloat::CompareMagnitude(mag, one) <= 0)
                    return -1.0f;
                if (BigFloat::CompareMagnitude(mag, limit) > 0) return finish(zr.ToDouble(), zi.ToDouble(), iter);
                stepBigFloatJulia(zr, zi, r2, i2);
            }
            return -1.0f;
        };

        // Julia perturbation varies z0 and keeps c fixed: delta' = 2Z*delta + delta^2.
        // Fall back to high precision when cancellation or an escaped reference makes the
        // relative orbit unreliable. This also preserves sub-double selected parameters.
        std::vector<ComplexOrbitPoint> orbit;
        std::vector<std::pair<BigFloat, BigFloat>> preciseOrbit;
        std::once_flag preciseOrbitOnce;
        if (deep)
        {
            orbit.reserve(static_cast<size_t>(config.maxIterations) + 1);
            auto zr = centerX, zi = centerY;
            for (int iter = 0; iter <= config.maxIterations; ++iter)
            {
                if (canceled()) return 0.0;
                orbit.push_back({ zr.ToDouble(), zi.ToDouble() });
                const auto r2 = zr.Sqr(limbs), i2 = zi.Sqr(limbs);
                if (r2.ToDouble() + i2.ToDouble() > bailout2) break;
                stepBigFloatJulia(zr, zi, r2, i2);
            }
        }
        auto ensurePreciseOrbit = [&]()
        {
            std::call_once(preciseOrbitOnce, [&]()
            {
                preciseOrbit.reserve(orbit.size());
                auto zr = centerX, zi = centerY;
                for (size_t iter = 0; iter < orbit.size(); ++iter)
                {
                    if (canceled()) break;
                    preciseOrbit.emplace_back(zr, zi);
                    if (iter + 1 >= orbit.size()) break;
                    const auto r2 = zr.Sqr(limbs), i2 = zi.Sqr(limbs);
                    stepBigFloatJulia(zr, zi, r2, i2);
                }
            });
        };
        auto bigDiffAbs = [](const BigFloat& r, const BigFloat& d) -> BigFloat
        {
            if (r.IsZero()) return d.Abs();
            const BigFloat sum = r + d;
            if (!sum.IsZero() && sum.sign == r.sign)
            {
                return r.sign ? -d : d;
            }
            return sum.Abs() - r.Abs();
        };
        // A critical point can square a visible displacement below double's exponent
        // range. Keep that displacement separate from Z so adding it to c cannot erase it.
        auto extendedPixel = [&](double px, double py) -> float
        {
            ensurePreciseOrbit();
            BigFloat dx((px + 0.5 - config.width * 0.5) * step);
            BigFloat dy((config.height * 0.5 - py - 0.5) * step);
            const BigFloat limit(bailout2);
            for (int iter = 0; iter < config.maxIterations; ++iter)
            {
                if (canceled()) return -1.0f;
                if (static_cast<size_t>(iter) >= preciseOrbit.size()) return exactPixel(px, py);
                const auto& [rx, ry] = preciseOrbit[iter];
                const auto zr = rx + dx, zi = ry + dy;
                if (BigFloat::CompareMagnitude(zr.Sqr(limbs) + zi.Sqr(limbs), limit) > 0)
                    return finish(zr.ToDouble(), zi.ToDouble(), iter);
                const auto nextDx = (rx + rx + dx).Mul(dx, limbs) - (ry + ry + dy).Mul(dy, limbs);
                if (burningShip)
                {
                    const auto daX = bigDiffAbs(rx, dx);
                    const auto daY = bigDiffAbs(ry, dy);
                    const auto product = rx.Abs().Mul(daY, limbs) + ry.Abs().Mul(daX, limbs) + daX.Mul(daY, limbs);
                    dy = -(product + product);
                }
                else
                {
                    const auto product = rx.Mul(dy, limbs) + ry.Mul(dx, limbs) + dx.Mul(dy, limbs);
                    dy = product + product;
                }
                dx = nextDx;
            }
            return -1.0f;
        };
        outIterations.resize(static_cast<size_t>(config.width) * config.height);
        auto evalPixel = [&](double px, double py) -> float
        {
            if (canceled()) return -1.0f;
            const double ox = (px + 0.5 - config.width * 0.5) * step;
            const double oy = (config.height * 0.5 - py - 0.5) * step;
            if (!deep)
            {
                double zr = config.centerX + ox, zi = config.centerY + oy;
                double checkZr = zr, checkZi = zi;
                int checkPeriod = 8, stepsSinceCheck = 0;
                for (int iter = 0; iter < config.maxIterations; ++iter)
                {
                    if ((iter & 63) == 0 && canceled()) return -1.0f;
                    if (zr * zr + zi * zi > bailout2) return finish(zr, zi, iter);
                    const double nextR = zr * zr - zi * zi + cx;
                    zi = burningShip ? (-2.0 * std::abs(zr * zi) + cy) : (2.0 * zr * zi + cy);
                    zr = nextR;
                    // Exact repetition proves this floating-point orbit cannot escape.
                    // No tolerance is used, so slowly escaping detail is retained.
                    if (zr == checkZr && zi == checkZi) return -1.0f;
                    if (++stepsSinceCheck == checkPeriod)
                    {
                        checkZr = zr; checkZi = zi;
                        stepsSinceCheck = 0;
                        if (checkPeriod < 1024) checkPeriod <<= 1;
                    }
                }
                return -1.0f;
            }
            if (config.juliaX.IsZero() && config.juliaY.IsZero())
            {
                const auto [zr, zi] = seed(px, py);
                if (BigFloat::CompareMagnitude(zr.Sqr(limbs) + zi.Sqr(limbs), BigFloat(1.0)) <= 0) return -1.0f;
            }
            double dx = ox, dy = oy;
            for (int iter = 0; iter < config.maxIterations; ++iter)
            {
                if (canceled()) return -1.0f;
                if (static_cast<size_t>(iter) >= orbit.size()) return exactPixel(px, py);
                const auto& ref = orbit[iter];
                const double zr = ref.x + dx, zi = ref.y + dy;
                const double mag = zr * zr + zi * zi;
                if (std::abs(mag - bailout2) < 1e-12 * bailout2 ||
                    (iter > 0 && mag < 1e-12 * (ref.x * ref.x + ref.y * ref.y + dx * dx + dy * dy)) ||
                    (burningShip && iter > 0 &&
                        (std::abs(zr) < 1e-6 * (std::abs(ref.x) + std::abs(dx)) ||
                         std::abs(zi) < 1e-6 * (std::abs(ref.y) + std::abs(dy)))))
                    return exactPixel(px, py);
                if (mag > bailout2) return finish(zr, zi, iter);
                const double nextDx = (2.0 * ref.x + dx) * dx - (2.0 * ref.y + dy) * dy;
                double nextDy;
                if (burningShip)
                {
                    const double daX = detail::DiffAbs(ref.x, dx);
                    const double daY = detail::DiffAbs(ref.y, dy);
                    nextDy = -2.0 * (std::abs(ref.x) * daY + std::abs(ref.y) * daX + daX * daY);
                }
                else
                {
                    nextDy = 2.0 * (ref.x * dy + ref.y * dx + dx * dy);
                }
                // Guard before double deltas underflow into subnormals (DBL_MIN ~ 2.2e-308).
                if (std::max(std::abs(nextDx), std::abs(nextDy)) < 1e-305 && (dx != 0.0 || dy != 0.0))
                    return extendedPixel(px, py);
                dy = nextDy;
                dx = nextDx;
            }
            return -1.0f;
        };
        detail::ExecuteRenderPass(config.width, config.height, config.renderStride, config.edgeRefine,
            config.smooth, config.fractalType, outIterations, cancelToken, evalPixel);
        detail::RefineSubpixels(config.width, config.height, config.smooth, config.fractalType,
            outIterations, sampleWeights, subpixelSamples, cancelToken, evalPixel);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
}
