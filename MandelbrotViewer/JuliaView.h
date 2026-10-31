#pragma once
#include "MandelbrotEngine.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace Mandelbrot
{
    // Canvas coordinates refer to pixel boundaries, keeping the selected point under the cursor.
    inline std::pair<BigFloat, BigFloat> ViewportPoint(const MandelbrotConfig& view, double x, double y)
    {
        const double step = 3.0 / view.zoom / std::max(1, view.width);
        return { view.hpCenterX + BigFloat((x - view.width * 0.5) * step),
            view.hpCenterY + BigFloat((view.height * 0.5 - y) * step) };
    }

    inline std::pair<double, double> ProjectPoint(const MandelbrotConfig& view, const BigFloat& x, const BigFloat& y)
    {
        const double pixelsPerUnit = view.zoom / 3.0 * view.width;
        return { view.width * 0.5 + (x - view.hpCenterX).ToDouble() * pixelsPerUnit,
            view.height * 0.5 - (y - view.hpCenterY).ToDouble() * pixelsPerUnit };
    }

    inline void ResetJuliaView(MandelbrotConfig& view)
    {
        view.fractalType = FractalType::Mandelbrot;
        view.juliaSet = true;
        view.SetCenter(0.0, 0.0);
        // |z| > sqrt(|c|)+1 is an escape radius. Fit its disk in either viewport aspect.
        const double radius = std::max(2.0, std::sqrt(std::hypot(view.juliaX.ToDouble(), view.juliaY.ToDouble())) + 1.0);
        const double aspect = std::min(1.0, static_cast<double>(std::max(1, view.height)) / std::max(1, view.width));
        view.zoom = std::clamp(3.0 * aspect / (2.2 * radius), MandelbrotEngine::MinZoom, MandelbrotEngine::MaxZoom);
        view.renderStride = 1;
        view.edgeRefine = true;
    }
}
