#include "pch.h"
#include "ViewportNavigation.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot
{
    void ScreenToComplex(
        double px, double py,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outCx, double& outCy)
    {
        const double aspect = static_cast<double>(height) / static_cast<double>(width > 0 ? width : 1);
        const double spanX = 3.0 / (zoom > 1e-12 ? zoom : 1e-12);
        const double spanY = spanX * aspect;

        outCx = centerX - (spanX * 0.5) + (px + 0.5) / static_cast<double>(width) * spanX;
        // Invert Y so positive imaginary is upward
        outCy = centerY + (spanY * 0.5) - (py + 0.5) / static_cast<double>(height) * spanY;
    }

    void ComplexToScreen(
        double cx, double cy,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outPx, double& outPy)
    {
        const double aspect = static_cast<double>(height) / static_cast<double>(width > 0 ? width : 1);
        const double spanX = 3.0 / (zoom > 1e-12 ? zoom : 1e-12);
        const double spanY = spanX * aspect;

        outPx = ((cx - (centerX - (spanX * 0.5))) / spanX) * static_cast<double>(width) - 0.5;
        outPy = (((centerY + (spanY * 0.5)) - cy) / spanY) * static_cast<double>(height) - 0.5;
    }

    void ZoomViewport(
        FractalConfig& config,
        double factor,
        double pivotPx,
        double pivotPy)
    {
        if (config.width <= 0 || config.height <= 0 || factor <= 0.0) return;

        // Ensure hpCenterX/Y are synced if caller only set double centerX/Y
        if (std::abs(config.hpCenterX.ToDouble() - config.centerX) > 1e-14 ||
            std::abs(config.hpCenterY.ToDouble() - config.centerY) > 1e-14)
        {
            config.hpCenterX = BigFloat(config.centerX);
            config.hpCenterY = BigFloat(config.centerY);
        }

        const double oldZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        const double newZoom = std::clamp(oldZoom * factor, MinZoom, MaxZoom);
        const double effectiveFactor = newZoom / oldZoom;

        const double width = static_cast<double>(config.width);
        const double height = static_cast<double>(config.height);
        const double aspect = height / width;

        const double oldSpanX = 3.0 / oldZoom;
        const double oldSpanY = oldSpanX * aspect;

        // Offset of pivot relative to current center in complex plane (represented in double without adding centerX!)
        const double relX = ((pivotPx + 0.5) / width - 0.5) * oldSpanX;
        const double relY = (0.5 - (pivotPy + 0.5) / height) * oldSpanY;

        // To keep the pivot fixed on screen: newCenter = oldCenter + rel * (1 - 1 / effectiveFactor)
        const double scaleDiff = 1.0 - (1.0 / effectiveFactor);
        const double shiftX = relX * scaleDiff;
        const double shiftY = relY * scaleDiff;

        config.hpCenterX = config.hpCenterX + BigFloat(shiftX);
        config.hpCenterY = config.hpCenterY + BigFloat(shiftY);
        config.centerX = config.hpCenterX.ToDouble();
        config.centerY = config.hpCenterY.ToDouble();
        config.zoom = newZoom;
    }

    void PanViewport(
        FractalConfig& config,
        const BigFloat& startX,
        const BigFloat& startY,
        double deltaPx,
        double deltaPy)
    {
        if (config.width <= 0 || config.height <= 0) return;

        const double width = static_cast<double>(config.width);
        const double height = static_cast<double>(config.height);
        const double aspect = height / width;
        const double zoom = std::clamp(config.zoom, MinZoom, MaxZoom);

        const double spanX = 3.0 / zoom;
        const double spanY = spanX * aspect;

        const double shiftX = -(deltaPx / width) * spanX;
        const double shiftY = (deltaPy / height) * spanY;

        config.hpCenterX = startX + BigFloat(shiftX);
        config.hpCenterY = startY + BigFloat(shiftY);
        config.centerX = config.hpCenterX.ToDouble();
        config.centerY = config.hpCenterY.ToDouble();
    }

    bool StepZoomAnimation(
        FractalConfig& config,
        const BigFloat& targetX,
        const BigFloat& targetY,
        double zoomFactor,
        double panBlend)
    {
        if (zoomFactor <= 0.0 || std::isnan(zoomFactor) || std::isinf(zoomFactor) || zoomFactor == 1.0)
        {
            return false;
        }

        const double oldZoom = std::clamp(config.zoom, MinZoom, MaxZoom);
        if ((zoomFactor > 1.0 && oldZoom >= MaxZoom) || (zoomFactor < 1.0 && oldZoom <= MinZoom))
        {
            config.SetCenter(targetX, targetY);
            return false;
        }

        const double newZoom = std::clamp(oldZoom * zoomFactor, MinZoom, MaxZoom);
        const BigFloat dx = config.hpCenterX - targetX;
        const BigFloat dy = config.hpCenterY - targetY;

        if (dx.IsZero() && dy.IsZero())
        {
            config.SetCenter(targetX, targetY);
        }
        else
        {
            const double width = static_cast<double>(std::max(1, config.width));
            const double height = static_cast<double>(std::max(1, config.height));
            const double aspect = height / width;
            const double spanX = 3.0 / oldZoom;
            const double spanY = spanX * aspect;

            const double absDx = std::abs(dx.ToDouble());
            const double absDy = std::abs(dy.ToDouble());
            const double pxDistX = (absDx / spanX) * width;
            const double pxDistY = (absDy / spanY) * height;

            // If target is far outside the surrounding viewport or already within 0.25 pixels of center, lock directly
            if (absDx > spanX * 2.0 || absDy > spanY * 2.0 || (pxDistX < 0.25 && pxDistY < 0.25))
            {
                config.SetCenter(targetX, targetY);
            }
            else
            {
                const double stepRatio = (zoomFactor >= 1.0) ? (oldZoom / newZoom) : (newZoom / oldZoom);
                const double clampedBlend = std::clamp(panBlend, 0.05, 1.0);
                const double decay = std::pow(stepRatio, 3.0 * clampedBlend);
                const BigFloat bfDecay(decay);
                config.SetCenter(targetX + dx * bfDecay, targetY + dy * bfDecay);
            }
        }

        config.zoom = newZoom;
        return (newZoom > MinZoom && newZoom < MaxZoom);
    }

    int RecommendMaxIterations(
        double zoom,
        FractalType type,
        int baseIterations)
    {
        const double clampedZoom = std::max(1.0, zoom);
        const double log10Zoom = std::log10(clampedZoom);

        if (type == FractalType::Newton)
        {
            // Newton's method converges quadratically once separated, needing a gentler iteration growth
            const int scaled = static_cast<int>(std::round(100.0 + 45.0 * log10Zoom + 2.5 * log10Zoom * log10Zoom));
            return std::clamp(std::max(baseIterations, scaled), 50, 50000);
        }

        // Escape-time fractals require progressively higher iteration budgets as filaments thin out at depth
        const int scaled = static_cast<int>(std::round(300.0 + 100.0 * log10Zoom + 8.0 * log10Zoom * log10Zoom));
        return std::clamp(std::max(baseIterations, scaled), 50, 50000);
    }
}
