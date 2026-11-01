#pragma once
#include "MandelbrotEngine.h"
#include "ColorTheme.h"
#include <array>
#include <cmath>
#include <stdexcept>

namespace Mandelbrot
{
    struct MovieResolution { int width, height; const wchar_t* label; };
    inline constexpr std::array<MovieResolution, 5> MovieResolutions{{
        {1280, 720, L"720p - 1280 x 720"}, {1920, 1080, L"1080p - 1920 x 1080"},
        {2560, 1440, L"1440p - 2560 x 1440"}, {3840, 2160, L"4K UHD - 3840 x 2160"},
        {7680, 4320, L"8K UHD - 7680 x 4320"} }};
    inline constexpr std::array<int, 6> MovieFrameRates{24, 25, 30, 48, 50, 60};

    struct MoviePlan
    {
        MandelbrotConfig start;
        BigFloat targetX, targetY;
        double endZoom{1000.0};
        int width{1920}, height{1080}, fps{30}, seconds{30};
        ColorTheme theme;
        float frequency{1.0f}, phase{};
        bool invert{}, motionSmoothing{true};
        uint32_t interior{0xFF000000};

        uint32_t FrameCount() const { return static_cast<uint32_t>(static_cast<uint64_t>(fps) * seconds); }
        void Validate() const
        {
            if (width < 1 || height < 1 || width > 7680 || height > 4320 ||
                std::find(MovieFrameRates.begin(), MovieFrameRates.end(), fps) == MovieFrameRates.end() ||
                seconds < 1 || seconds > 3600 || !std::isfinite(start.zoom) || !std::isfinite(endZoom) ||
                start.zoom < MandelbrotEngine::MinZoom || start.zoom > MandelbrotEngine::MaxZoom ||
                endZoom < MandelbrotEngine::MinZoom || endZoom > MandelbrotEngine::MaxZoom || start.maxIterations < 1 ||
                !std::isfinite(targetX.ToDouble()) || !std::isfinite(targetY.ToDouble()) ||
                !std::isfinite(start.hpCenterX.ToDouble()) || !std::isfinite(start.hpCenterY.ToDouble()) ||
                !std::isfinite(frequency) || !std::isfinite(phase))
                throw std::invalid_argument("Invalid movie resolution, frame rate, duration, or zoom.");
        }

        MandelbrotConfig CameraAt(double frameIndex) const
        {
            auto camera = start;
            camera.width = width; camera.height = height;
            camera.renderStride = 1;
            camera.edgeRefine = false;
            const double t = std::clamp(frameIndex / (FrameCount() - 1.0), 0.0, 1.0);
            const double range = std::log(endZoom) - std::log(start.zoom);
            camera.zoom = t == 0 ? start.zoom : t == 1 ? endZoom : std::exp(std::log(start.zoom) + range * t);
            if (t == 0) return camera;
            if (t == 1) { camera.SetCenter(targetX, targetY); return camera; }
            // Evaluate each camera from absolute footage time. Use the nearer endpoint as
            // the HP anchor, preserving sub-double displacements even across 300 zoom decades.
            if (std::abs(range) < 1e-8)
                camera.SetCenter(start.hpCenterX + (targetX - start.hpCenterX).Mul(BigFloat(t)),
                    start.hpCenterY + (targetY - start.hpCenterY).Mul(BigFloat(t)));
            else if (range > 0)
            {
                const BigFloat a(std::exp(-range * t) * -std::expm1(-range * (1.0 - t)) / -std::expm1(-range));
                camera.SetCenter(targetX + (start.hpCenterX - targetX).Mul(a), targetY + (start.hpCenterY - targetY).Mul(a));
            }
            else
            {
                const BigFloat b(std::exp(range * (1.0 - t)) * -std::expm1(range * t) / -std::expm1(range));
                camera.SetCenter(start.hpCenterX + (targetX - start.hpCenterX).Mul(b),
                    start.hpCenterY + (targetY - start.hpCenterY).Mul(b));
            }
            return camera;
        }
    };
}
