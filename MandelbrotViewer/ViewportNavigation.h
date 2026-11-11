#pragma once
#include "FractalConfig.h"

namespace Mandelbrot
{
    void ScreenToComplex(
        double px, double py,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outCx, double& outCy);

    void ComplexToScreen(
        double cx, double cy,
        int width, int height,
        double centerX, double centerY, double zoom,
        double& outPx, double& outPy);

    // High-precision cursor-centered zoom without double cancellation
    void ZoomViewport(
        FractalConfig& config,
        double factor,
        double pivotPx,
        double pivotPy);

    // High-precision viewport pan without double cancellation
    void PanViewport(
        FractalConfig& config,
        const BigFloat& startX,
        const BigFloat& startY,
        double deltaPx,
        double deltaPy);

    // Smoothly steps an animated zoom (in or out) toward a target complex coordinate.
    // zoomFactor > 1.0 zooms in; zoomFactor < 1.0 zooms out.
    // panBlend in [0.0, 1.0] controls lateral pan acceleration (e.g., ramping 0->1 after retargeting for smooth ease-in).
    // Returns true if the zoom step was applied, or false if zoom reached [MinZoom, MaxZoom] bounds.
    bool StepZoomAnimation(
        FractalConfig& config,
        const BigFloat& targetX,
        const BigFloat& targetY,
        double zoomFactor,
        double panBlend = 1.0);

    // Recommends an iteration budget scaled to the current magnification depth
    int RecommendMaxIterations(
        double zoom,
        FractalType type,
        int baseIterations = 300);
}
