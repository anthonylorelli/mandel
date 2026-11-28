#include "MovieZoomPyramid.h"
#include <algorithm>
#include <cmath>

namespace Mandelbrot
{
    namespace
    {
        constexpr double kKeyframeScale = 1.5;
        constexpr int kKeyframePadPerSide = 6; // 3px Mitchell filter support + 3px shutter guard
        constexpr double kCoverageMarginPx = 3.0;

        bool KeyframeCoversMovieFrame(
            const FractalConfig& kf,
            const MoviePlan& plan,
            uint32_t frameIndex,
            double marginPx) noexcept
        {
            const auto cam = plan.CameraAt(static_cast<double>(frameIndex));
            if (!KeyframeCoversCamera(kf, cam, marginPx, 0.999))
            {
                return false;
            }
            if (plan.motionSmoothing && plan.FrameCount() > 1)
            {
                const double f = static_cast<double>(frameIndex);
                const double fMax = static_cast<double>(plan.FrameCount() - 1);
                if (f > 0.0 && !KeyframeCoversCamera(kf, plan.CameraAt(std::max(0.0, f - 0.25)), marginPx, 0.94))
                {
                    return false;
                }
                if (f < fMax && !KeyframeCoversCamera(kf, plan.CameraAt(std::min(fMax, f + 0.25)), marginPx, 0.94))
                {
                    return false;
                }
            }
            return true;
        }

        FractalConfig MakeKeyframeCamera(
            const MoviePlan& plan,
            double anchorFrame,
            int kfWidth,
            int kfHeight,
            double scale) noexcept
        {
            const auto anchorCam = plan.CameraAt(anchorFrame);
            FractalConfig kf = anchorCam;
            kf.width = kfWidth;
            kf.height = kfHeight;
            kf.renderStride = 1;
            kf.edgeRefine = false;
            // Set zoom so pixel spacing at anchorCam satisfies step_anchor / step_kf == scale.
            kf.zoom = std::clamp(
                anchorCam.zoom * (static_cast<double>(plan.width) * scale / static_cast<double>(kfWidth)),
                MinZoom,
                MaxZoom);
            kf.SetCenter(anchorCam.hpCenterX, anchorCam.hpCenterY);
            return kf;
        }
    }

    KeyframeCameraTransform ComputeCameraToKeyframe(
        const FractalConfig& keyframe,
        const FractalConfig& camera) noexcept
    {
        KeyframeCameraTransform t{};
        if (keyframe.width <= 0 || keyframe.height <= 0 ||
            camera.width <= 0 || camera.height <= 0 ||
            !(keyframe.zoom > 0.0) || !(camera.zoom > 0.0))
        {
            return t;
        }
        t.rho = (keyframe.zoom / camera.zoom) *
                (static_cast<double>(keyframe.width) / static_cast<double>(camera.width));
        const BigFloat invStepKf = BigFloat(keyframe.zoom).Mul(BigFloat(static_cast<double>(keyframe.width) / 3.0));
        t.offsetX = (camera.hpCenterX - keyframe.hpCenterX).Mul(invStepKf).ToDouble();
        t.offsetY = -(camera.hpCenterY - keyframe.hpCenterY).Mul(invStepKf).ToDouble();
        return t;
    }

    bool KeyframeCoversCamera(
        const FractalConfig& keyframe,
        const FractalConfig& camera,
        double marginPx,
        double minRho) noexcept
    {
        if (keyframe.fractalType != camera.fractalType ||
            keyframe.juliaSet != camera.juliaSet ||
            keyframe.width <= 0 || keyframe.height <= 0 ||
            camera.width <= 0 || camera.height <= 0)
        {
            return false;
        }
        const auto t = ComputeCameraToKeyframe(keyframe, camera);
        if (!std::isfinite(t.rho) || t.rho < minRho ||
            !std::isfinite(t.offsetX) || !std::isfinite(t.offsetY))
        {
            return false;
        }
        const double hx = t.rho * (static_cast<double>(camera.width) * 0.5);
        const double hy = t.rho * (static_cast<double>(camera.height) * 0.5);
        const double limitX = static_cast<double>(keyframe.width) * 0.5 - marginPx;
        const double limitY = static_cast<double>(keyframe.height) * 0.5 - marginPx;
        return (std::abs(t.offsetX) + hx <= limitX) && (std::abs(t.offsetY) + hy <= limitY);
    }

    KeyframeInnerRect ComputeInnerSkipRect(
        const FractalConfig& outerKeyframe,
        const FractalConfig& innerKeyframe,
        int featherPx) noexcept
    {
        KeyframeInnerRect rect{};
        if (outerKeyframe.fractalType != innerKeyframe.fractalType ||
            outerKeyframe.juliaSet != innerKeyframe.juliaSet ||
            outerKeyframe.width < 32 || outerKeyframe.height < 32 ||
            innerKeyframe.width < 16 || innerKeyframe.height < 16)
        {
            return rect;
        }

        const auto t = ComputeCameraToKeyframe(outerKeyframe, innerKeyframe);
        // Inner keyframe must be strictly higher resolution than outer keyframe so downsampling
        // into the outer keyframe preserves full sharpness.
        if (!std::isfinite(t.rho) || t.rho <= 0.0 || t.rho > 0.85 ||
            !std::isfinite(t.offsetX) || !std::isfinite(t.offsetY))
        {
            return rect;
        }

        const double usableHalfW = std::max(0.0, (static_cast<double>(innerKeyframe.width) * 0.5 - kCoverageMarginPx) * t.rho);
        const double usableHalfH = std::max(0.0, (static_cast<double>(innerKeyframe.height) * 0.5 - kCoverageMarginPx) * t.rho);
        const double cx = static_cast<double>(outerKeyframe.width) * 0.5 + t.offsetX;
        const double cy = static_cast<double>(outerKeyframe.height) * 0.5 + t.offsetY;

        const int bx0 = std::clamp(static_cast<int>(std::ceil(cx - usableHalfW)), 0, outerKeyframe.width);
        const int bx1 = std::clamp(static_cast<int>(std::floor(cx + usableHalfW)), 0, outerKeyframe.width);
        const int by0 = std::clamp(static_cast<int>(std::ceil(cy - usableHalfH)), 0, outerKeyframe.height);
        const int by1 = std::clamp(static_cast<int>(std::floor(cy + usableHalfH)), 0, outerKeyframe.height);

        const int clampedFeather = std::clamp(featherPx, 1, 8);
        // Inset the skipped region by featherPx + 2 so the outer ring has complete 3x3 fractal
        // neighborhoods for boundary detection and gradient AA right up to the feather edge.
        const int inset = clampedFeather + 2;
        int sx0 = (bx0 > 0) ? (bx0 + inset) : 0;
        int sx1 = (bx1 < outerKeyframe.width) ? (bx1 - inset) : outerKeyframe.width;
        int sy0 = (by0 > 0) ? (by0 + inset) : 0;
        int sy1 = (by1 < outerKeyframe.height) ? (by1 - inset) : outerKeyframe.height;

        if (sx1 - sx0 < 16 || sy1 - sy0 < 16)
        {
            return rect;
        }

        // Guard against narrow left/right strips exceeding MaxZoom when subZoom = zoom * (kfW / stripW).
        auto stripZoomOk = [&](int stripW) noexcept -> bool
        {
            if (stripW <= 0) return true;
            if (stripW < 4) return false;
            const double subZoom = outerKeyframe.zoom * (static_cast<double>(outerKeyframe.width) / static_cast<double>(stripW));
            return std::isfinite(subZoom) && subZoom <= MaxZoom * 0.99;
        };
        const bool leftOk = (sx0 == 0) || stripZoomOk(sx0 + 1);
        const bool rightOk = (sx1 == outerKeyframe.width) || stripZoomOk(outerKeyframe.width - sx1 + 1);
        if (!leftOk || !rightOk)
        {
            return rect;
        }

        const uint64_t skipArea = static_cast<uint64_t>(sx1 - sx0) * static_cast<uint64_t>(sy1 - sy0);
        const uint64_t fullArea = static_cast<uint64_t>(outerKeyframe.width) * static_cast<uint64_t>(outerKeyframe.height);
        if (skipArea * 100 < fullArea * 15)
        {
            return rect;
        }

        rect.valid = true;
        rect.x0 = sx0;
        rect.y0 = sy0;
        rect.x1 = sx1;
        rect.y1 = sy1;
        rect.blendX0 = bx0;
        rect.blendY0 = by0;
        rect.blendX1 = bx1;
        rect.blendY1 = by1;
        return rect;
    }

    size_t ComputeZoomPyramidChunkSize(int keyframeWidth, int keyframeHeight) noexcept
    {
        if (keyframeWidth <= 0 || keyframeHeight <= 0) return 1;
        const uint64_t keyframeBytes = static_cast<uint64_t>(keyframeWidth) * static_cast<uint64_t>(keyframeHeight) * sizeof(uint32_t);
        const uint64_t ramBudget = (keyframeBytes > 200ULL * 1024 * 1024)
            ? (640ULL * 1024 * 1024)
            : (384ULL * 1024 * 1024);
        return static_cast<size_t>(std::clamp<uint64_t>(ramBudget / std::max<uint64_t>(1, keyframeBytes), 1, 6));
    }

    ZoomPyramidPlan PlanZoomKeyframes(const MoviePlan& plan)
    {
        ZoomPyramidPlan result{};
        const uint32_t totalFrames = plan.FrameCount();
        result.directPixels = static_cast<uint64_t>(totalFrames) *
                              static_cast<uint64_t>(plan.width) *
                              static_cast<uint64_t>(plan.height);

        if (totalFrames < 8 || plan.width < 8 || plan.height < 8 || plan.histogramEqualize)
        {
            return result;
        }

        const double log2Ratio = std::log2(plan.endZoom / plan.start.zoom);
        if (!std::isfinite(log2Ratio) || std::abs(log2Ratio) < 0.75)
        {
            return result;
        }

        const bool zoomIn = (log2Ratio > 0.0);
        const int kfWidth = ((static_cast<int>(std::ceil(plan.width * kKeyframeScale)) + 1) & ~1) + 2 * kKeyframePadPerSide;
        const int kfHeight = ((static_cast<int>(std::ceil(plan.height * kKeyframeScale)) + 1) & ~1) + 2 * kKeyframePadPerSide;

        result.zoomIn = zoomIn;
        result.keyframeWidth = kfWidth;
        result.keyframeHeight = kfHeight;
        result.keyframeScale = kKeyframeScale;

        const double maxFrameD = static_cast<double>(totalFrames - 1);

        if (zoomIn)
        {
            uint32_t fStart = 0;
            while (fStart < totalFrames)
            {
                const double anchor = plan.motionSmoothing ? std::max(0.0, static_cast<double>(fStart) - 0.25) : static_cast<double>(fStart);
                FractalConfig kf = MakeKeyframeCamera(plan, anchor, kfWidth, kfHeight, kKeyframeScale);
                if (!KeyframeCoversMovieFrame(kf, plan, fStart, kCoverageMarginPx))
                {
                    return result;
                }
                uint32_t fEnd = fStart;
                while (fEnd + 1 < totalFrames && KeyframeCoversMovieFrame(kf, plan, fEnd + 1, kCoverageMarginPx))
                {
                    ++fEnd;
                }
                if (fEnd == fStart && fEnd + 1 < totalFrames)
                {
                    return result;
                }
                kf.maxIterations = std::max(
                    plan.CameraAt(static_cast<double>(fStart)).maxIterations,
                    plan.CameraAt(static_cast<double>(fEnd)).maxIterations);
                result.keyframes.push_back(ZoomKeyframeSpec{ kf, fStart, fEnd });
                if (fEnd + 1 >= totalFrames) break;
                const uint32_t span = fEnd - fStart;
                const uint32_t overlap = std::min<uint32_t>(3u, span / 8u);
                fStart = std::max(fStart + 1, fEnd + 1 - overlap);
            }
        }
        else
        {
            uint32_t fEnd = totalFrames - 1;
            for (;;)
            {
                const double anchor = plan.motionSmoothing ? std::min(maxFrameD, static_cast<double>(fEnd) + 0.25) : static_cast<double>(fEnd);
                FractalConfig kf = MakeKeyframeCamera(plan, anchor, kfWidth, kfHeight, kKeyframeScale);
                if (!KeyframeCoversMovieFrame(kf, plan, fEnd, kCoverageMarginPx))
                {
                    return result;
                }
                uint32_t fStart = fEnd;
                while (fStart > 0 && KeyframeCoversMovieFrame(kf, plan, fStart - 1, kCoverageMarginPx))
                {
                    --fStart;
                }
                if (fStart == fEnd && fStart > 0)
                {
                    return result;
                }
                kf.maxIterations = std::max(
                    plan.CameraAt(static_cast<double>(fStart)).maxIterations,
                    plan.CameraAt(static_cast<double>(fEnd)).maxIterations);
                result.keyframes.push_back(ZoomKeyframeSpec{ kf, fStart, fEnd });
                if (fStart == 0) break;
                const uint32_t span = fEnd - fStart;
                const uint32_t overlap = std::min<uint32_t>(3u, span / 8u);
                fEnd = std::min(fEnd - 1, fStart - 1 + overlap);
            }
            std::reverse(result.keyframes.begin(), result.keyframes.end());
        }

        if (result.keyframes.empty())
        {
            return result;
        }

        // Assign each movie frame to its covering keyframe (and smooth crossfade partner on overlap frames).
        result.frames.assign(totalFrames, FrameKeyframeAssignment{});
        const int numKf = static_cast<int>(result.keyframes.size());
        for (int k = 0; k < numKf; ++k)
        {
            const auto& spec = result.keyframes[static_cast<size_t>(k)];
            for (uint32_t f = spec.firstFrame; f <= spec.lastFrame; ++f)
            {
                auto& assign = result.frames[f];
                if (assign.primaryKeyframe < 0)
                {
                    assign.primaryKeyframe = k;
                }
                else if (assign.secondaryKeyframe < 0 && k == assign.primaryKeyframe + 1)
                {
                    assign.secondaryKeyframe = k;
                }
            }
        }

        // Compute smooth cubic crossfade weights across each overlap run.
        for (int k = 0; k + 1 < numKf; ++k)
        {
            const uint32_t ovStart = result.keyframes[static_cast<size_t>(k + 1)].firstFrame;
            const uint32_t ovEnd = result.keyframes[static_cast<size_t>(k)].lastFrame;
            if (ovStart <= ovEnd)
            {
                const double denom = static_cast<double>(ovEnd - ovStart + 2);
                for (uint32_t f = ovStart; f <= ovEnd; ++f)
                {
                    if (result.frames[f].primaryKeyframe == k && result.frames[f].secondaryKeyframe == k + 1)
                    {
                        const double u = static_cast<double>(f - ovStart + 1) / denom;
                        result.frames[f].blendWeight = static_cast<float>(u * u * (3.0 - 2.0 * u));
                    }
                }
            }
        }

        for (uint32_t f = 0; f < totalFrames; ++f)
        {
            if (result.frames[f].primaryKeyframe < 0)
            {
                return result;
            }
        }

        // Estimate rendered pixels including concentric inner-rectangle reuse.
        const uint64_t kfFullPixels = static_cast<uint64_t>(kfWidth) * static_cast<uint64_t>(kfHeight);
        uint64_t estPixels = 0;
        if (zoomIn)
        {
            const size_t maxChunk = ComputeZoomPyramidChunkSize(kfWidth, kfHeight);
            for (size_t cStart = 0; cStart < result.keyframes.size();)
            {
                const size_t chunkCap = (cStart == 0) ? std::min<size_t>(maxChunk, 3) : maxChunk;
                const size_t cEnd = std::min(result.keyframes.size(), cStart + chunkCap);
                for (size_t k = cStart; k < cEnd; ++k)
                {
                    uint64_t pixels = kfFullPixels;
                    if (k + 1 < cEnd)
                    {
                        const auto inner = ComputeInnerSkipRect(result.keyframes[k].camera, result.keyframes[k + 1].camera);
                        if (inner.valid)
                        {
                            pixels -= static_cast<uint64_t>(inner.x1 - inner.x0) * static_cast<uint64_t>(inner.y1 - inner.y0);
                        }
                    }
                    estPixels += pixels;
                }
                cStart = cEnd;
            }
        }
        else
        {
            for (size_t k = 0; k < result.keyframes.size(); ++k)
            {
                uint64_t pixels = kfFullPixels;
                if (k > 0)
                {
                    const auto inner = ComputeInnerSkipRect(result.keyframes[k].camera, result.keyframes[k - 1].camera);
                    if (inner.valid)
                    {
                        pixels -= static_cast<uint64_t>(inner.x1 - inner.x0) * static_cast<uint64_t>(inner.y1 - inner.y0);
                    }
                }
                estPixels += pixels;
            }
        }

        result.estimatedKeyframePixels = estPixels;
        // Require at least ~28% pixel savings to outweigh resampling overhead.
        if (estPixels * 100 < result.directPixels * 72)
        {
            result.enabled = true;
        }
        return result;
    }
}
