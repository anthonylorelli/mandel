#pragma once
#include "GpuFractalSolver.h"
#include "MoviePlan.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace Mandelbrot
{
    // Exact affine mapping from a camera viewport into a wider/higher-resolution keyframe.
    struct KeyframeCameraTransform
    {
        double offsetX{}; // Camera center offset in keyframe pixels (+right)
        double offsetY{}; // Camera center offset in keyframe pixels (+down)
        double rho{};     // Keyframe pixels per camera pixel: (kf.zoom / cam.zoom) * (kf.width / cam.width)
    };

    // Interior rectangle of an outer keyframe that is covered at higher resolution by a deeper keyframe
    // and can therefore be skipped during rendering and filled by linear-light downsampling.
    struct KeyframeInnerRect
    {
        bool valid{};
        int x0{}, y0{}, x1{}, y1{};             // Skipped inner rectangle [x0, x1) x [y0, y1)
        int blendX0{}, blendY0{}, blendX1{}, blendY1{}; // Outer feather rectangle enclosing [x0, x1) x [y0, y1)
    };

    struct ZoomKeyframeSpec
    {
        FractalConfig camera;
        uint32_t firstFrame{};
        uint32_t lastFrame{};
    };

    struct FrameKeyframeAssignment
    {
        int primaryKeyframe{ -1 };
        int secondaryKeyframe{ -1 };
        float blendWeight{ 0.0f };
    };

    struct ZoomPyramidPlan
    {
        bool enabled{};
        bool zoomIn{ true };
        int keyframeWidth{};
        int keyframeHeight{};
        double keyframeScale{ 1.5 };
        std::vector<ZoomKeyframeSpec> keyframes;
        std::vector<FrameKeyframeAssignment> frames;
        uint64_t estimatedKeyframePixels{};
        uint64_t directPixels{};
    };

    KeyframeCameraTransform ComputeCameraToKeyframe(
        const FractalConfig& keyframe,
        const FractalConfig& camera) noexcept;

    bool KeyframeCoversCamera(
        const FractalConfig& keyframe,
        const FractalConfig& camera,
        double marginPx = 3.0,
        double minRho = 0.999) noexcept;

    KeyframeInnerRect ComputeInnerSkipRect(
        const FractalConfig& outerKeyframe,
        const FractalConfig& innerKeyframe,
        int featherPx = 4) noexcept;

    ZoomPyramidPlan PlanZoomKeyframes(const MoviePlan& plan);

    size_t ComputeZoomPyramidChunkSize(int keyframeWidth, int keyframeHeight) noexcept;

    // Resamples an output movie frame from one keyframe (or crossfades across two overlapping
    // keyframes in linear light), with optional fused radial shutter blur.
    void ResampleKeyframeFrame(
        const std::vector<uint32_t>& primaryPixels,
        const FractalConfig& primaryKf,
        const std::vector<uint32_t>* secondaryPixels,
        const FractalConfig* secondaryKf,
        float blendWeight,
        const FractalConfig& outputCamera,
        const GpuMovieShutterParams* shutter,
        std::vector<uint32_t>& outPixels,
        const std::atomic<bool>& cancel);

    // Composites a deeper, higher-resolution keyframe into an outer keyframe's skipped interior
    // rectangle using separable linear-light Mitchell-Netravali downsampling and a smooth border feather.
    void CompositeInnerKeyframe(
        std::vector<uint32_t>& outerPixels,
        const FractalConfig& outerKf,
        const std::vector<uint32_t>& innerPixels,
        const FractalConfig& innerKf,
        const KeyframeInnerRect& innerRect,
        const std::atomic<bool>& cancel);
}
