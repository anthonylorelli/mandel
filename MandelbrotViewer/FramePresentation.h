#pragma once
#include "MandelbrotEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Mandelbrot
{
    struct FrameTransform
    {
        double scale{ 1.0 };
        double x{};
        double y{};
    };

    constexpr double MaxPresentationScale = 1.12;

    inline MandelbrotConfig AnimationRenderConfig(const MandelbrotConfig& viewport)
    {
        auto frame = viewport;
        // Keep the coarse grid stable throughout motion. Camera speed absorbs rendering
        // pressure instead of changing which fine features are reconstructed each frame.
        frame.renderStride = 2;
        frame.edgeRefine = true;
        // Preserve complex-plane pixel spacing while adding an 8% guard band on each edge.
        frame.width += 2 * static_cast<int>(std::ceil(viewport.width * 0.08));
        frame.height += 2 * static_cast<int>(std::ceil(viewport.height * 0.08));
        frame.zoom *= static_cast<double>(viewport.width) / frame.width;
        return frame;
    }

    inline MandelbrotConfig FrameViewport(const MandelbrotConfig& frame, int width, int height)
    {
        auto view = frame;
        view.width = width;
        view.height = height;
        view.zoom *= static_cast<double>(frame.width) / width;
        return view;
    }

    // Pixel-boundary coordinates: a sample at index (x,y) is located at (x+0.5,y+0.5).
    inline FrameTransform FrameToViewport(const MandelbrotConfig& frame, const MandelbrotConfig& view)
    {
        const double pixelsPerUnit = (view.zoom / 3.0) * view.width;
        const double scale = (view.zoom / frame.zoom) * (static_cast<double>(view.width) / frame.width);
        return { scale,
            view.width * 0.5 - frame.width * 0.5 * scale +
                (frame.hpCenterX - view.hpCenterX).ToDouble() * pixelsPerUnit,
            view.height * 0.5 - frame.height * 0.5 * scale +
                (view.hpCenterY - frame.hpCenterY).ToDouble() * pixelsPerUnit };
    }

    inline bool FrameCoversViewport(const MandelbrotConfig& frame, const MandelbrotConfig& view)
    {
        if (frame.fractalType != view.fractalType || frame.juliaSet != view.juliaSet ||
            (frame.juliaSet && (frame.juliaX != view.juliaX || frame.juliaY != view.juliaY)) ||
            frame.width <= 0 || frame.height <= 0 ||
            view.width <= 0 || view.height <= 0 || !(frame.zoom > 0.0) || !(view.zoom > 0.0)) return false;
        const auto t = FrameToViewport(frame, view);
        constexpr double epsilon = 1e-6;
        return std::isfinite(t.scale) && std::isfinite(t.x) && std::isfinite(t.y) &&
            t.scale > 0.0 && t.scale <= MaxPresentationScale + epsilon &&
            t.x <= epsilon && t.y <= epsilon &&
            t.x + t.scale * frame.width >= view.width - epsilon &&
            t.y + t.scale * frame.height >= view.height - epsilon;
    }

    // Interpolate the affine screen transform, keeping complex centers in high precision.
    // Centers must be weighted by zoom to make translation linear along with scale.
    inline MandelbrotConfig InterpolateCamera(const MandelbrotConfig& from, const MandelbrotConfig& to, double progress)
    {
        if (progress <= 0.0) return from;
        if (progress >= 1.0) return to;
        auto view = to;
        const double ratio = to.zoom / from.zoom;
        const double scale = 1.0 + progress * (ratio - 1.0);
        view.zoom = std::clamp(from.zoom * scale, MandelbrotEngine::MinZoom, MandelbrotEngine::MaxZoom);
        const BigFloat weight(progress * ratio / scale);
        view.SetCenter(from.hpCenterX + (to.hpCenterX - from.hpCenterX) * weight,
            from.hpCenterY + (to.hpCenterY - from.hpCenterY) * weight);
        return view;
    }

    inline MandelbrotConfig LimitCameraToFrame(
        const MandelbrotConfig& frame, const MandelbrotConfig& from, const MandelbrotConfig& requested)
    {
        if (requested.fractalType != from.fractalType || requested.width != from.width || requested.height != from.height)
            return from;
        if (!FrameCoversViewport(frame, from)) return FrameViewport(frame, requested.width, requested.height);
        const auto a = FrameToViewport(frame, from);
        const auto b = FrameToViewport(frame, requested);
        if (!std::isfinite(b.scale) || !std::isfinite(b.x) || !std::isfinite(b.y)) return from;
        double progress = 1.0;
        auto constrain = [&](double start, double end)
        {
            if (end < 0.0) progress = std::min(progress, std::max(0.0, start) / (start - end));
        };
        constrain(MaxPresentationScale - a.scale, MaxPresentationScale - b.scale);
        constrain(-a.x, -b.x);
        constrain(-a.y, -b.y);
        constrain(a.x + a.scale * frame.width - from.width, b.x + b.scale * frame.width - requested.width);
        constrain(a.y + a.scale * frame.height - from.height, b.y + b.scale * frame.height - requested.height);
        return InterpolateCamera(from, requested, std::clamp(progress, 0.0, 1.0));
    }

    inline double PresentationZoomSpeed(double requested, double renderMs)
    {
        // Keep roughly 6% of scale change per 1.5 render times, leaving room for jitter.
        return std::min(requested, std::log2(1.06) / std::max(0.016, renderMs * 0.0015));
    }

    // Export the viewport, rather than the larger guard-band bitmap, using the same affine
    // mapping as presentation. Bilinear sampling matches the temporarily transformed image.
    inline std::vector<uint8_t> SnapshotViewport(
        const uint8_t* source, const MandelbrotConfig& frame, const MandelbrotConfig& view)
    {
        const auto t = FrameToViewport(frame, view);
        std::vector<uint8_t> pixels(static_cast<size_t>(view.width) * view.height * 4);
        for (int y = 0; y < view.height; ++y)
        {
            const double sy = std::clamp(((y + 0.5) - t.y) / t.scale - 0.5, 0.0, frame.height - 1.0);
            const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, frame.height - 1);
            const double fy = sy - y0;
            for (int x = 0; x < view.width; ++x)
            {
                const double sx = std::clamp(((x + 0.5) - t.x) / t.scale - 0.5, 0.0, frame.width - 1.0);
                const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, frame.width - 1);
                const double fx = sx - x0;
                for (int c = 0; c < 4; ++c)
                {
                    auto sample = [&](int px, int py) { return source[(static_cast<size_t>(py) * frame.width + px) * 4 + c]; };
                    const double top = sample(x0, y0) * (1.0 - fx) + sample(x1, y0) * fx;
                    const double bottom = sample(x0, y1) * (1.0 - fx) + sample(x1, y1) * fx;
                    pixels[(static_cast<size_t>(y) * view.width + x) * 4 + c] =
                        static_cast<uint8_t>(std::clamp(std::round(top * (1.0 - fy) + bottom * fy), 0.0, 255.0));
                }
            }
        }
        return pixels;
    }
}
