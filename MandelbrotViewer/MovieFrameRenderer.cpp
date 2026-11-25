#include "MovieFrameRenderer.h"
#include "MovieExport.h"
#include "MovieZoomPyramid.h"
#include "GpuFractalSolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace Mandelbrot
{
    namespace detail
    {
        bool BuildShutterParams(
            const MoviePlan& plan,
            double frameIndex,
            const FractalConfig& cam0,
            GpuMovieShutterParams& outShutter)
        {
            outShutter = {};
            if (!plan.motionSmoothing || plan.FrameCount() <= 1)
            {
                return false;
            }

            constexpr std::array<double, 7> kTapOffsets{
                -0.25, -1.0 / 6.0, -1.0 / 12.0, 0.0, 1.0 / 12.0, 1.0 / 6.0, 0.25
            };

            const BigFloat invStep0(cam0.zoom * (static_cast<double>(plan.width) / 3.0));
            const float halfW = static_cast<float>(plan.width) * 0.5f;
            const float halfH = static_cast<float>(plan.height) * 0.5f;
            float maxCornerMotionPx = 0.0f;

            outShutter.tapCount = static_cast<uint32_t>(kTapOffsets.size());
            for (size_t k = 0; k < kTapOffsets.size(); ++k)
            {
                if (kTapOffsets[k] == 0.0)
                {
                    outShutter.shiftX[k] = 0.0f;
                    outShutter.shiftY[k] = 0.0f;
                    outShutter.scale[k] = 1.0f;
                    continue;
                }

                const auto camTau = plan.CameraAt(frameIndex + kTapOffsets[k]);
                const float sx = static_cast<float>((camTau.hpCenterX - cam0.hpCenterX).Mul(invStep0).ToDouble());
                const float sy = static_cast<float>(-(camTau.hpCenterY - cam0.hpCenterY).Mul(invStep0).ToDouble());
                const float sc = static_cast<float>(cam0.zoom / camTau.zoom);

                outShutter.shiftX[k] = sx;
                outShutter.shiftY[k] = sy;
                outShutter.scale[k] = sc;

                const float ds = sc - 1.0f;
                maxCornerMotionPx = std::max({
                    maxCornerMotionPx,
                    std::abs(sx + halfW * ds),
                    std::abs(sx - halfW * ds),
                    std::abs(sy + halfH * ds),
                    std::abs(sy - halfH * ds) });
            }

            if (maxCornerMotionPx < 0.05f)
            {
                outShutter.tapCount = 0;
                return false;
            }
            return true;
        }
    }

    namespace
    {
        struct ScopedExecutionMode
        {
            MandelbrotEngine& engine;
            ExecutionMode previous;
            ~ScopedExecutionMode() { engine.SetExecutionMode(previous); }
        };

        int ComputeGpuBandRows(const FractalConfig& cam, int bandWidth, int bandHeight, int maxBandRows) noexcept
        {
            const int maxStripByMem = std::max(
                1,
                static_cast<int>((4ULL * 1024 * 1024) / static_cast<size_t>(std::max(1, bandWidth))) - 2);
            const bool gpuMultiPassEligible =
                (cam.fractalType == FractalType::Mandelbrot &&
                 !cam.juliaSet &&
                 cam.zoom >= GpuPerturbationThreshold);
            const uint64_t effectiveDispatchIters = static_cast<uint64_t>(
                gpuMultiPassEligible
                    ? std::min(std::max(1, cam.maxIterations), 12288)
                    : std::max(1, cam.maxIterations));
            const uint64_t maxStripByTdr64 = 12'000'000'000ULL /
                (static_cast<uint64_t>(std::max(1, bandWidth)) * effectiveDispatchIters);
            const int maxStripByTdr = static_cast<int>(
                std::clamp<uint64_t>(maxStripByTdr64, 16ULL, static_cast<uint64_t>(std::max(1, bandHeight))));
            return std::min({ maxBandRows, maxStripByMem, maxStripByTdr });
        }

        void ApplyCpuShutterBlur(
            const std::vector<uint32_t>& rawPixels,
            std::vector<uint32_t>& outPixels,
            int width,
            int height,
            const GpuMovieShutterParams& shutter,
            const detail::LinearSrgbTables& colors,
            const std::atomic<bool>& cancel)
        {
            const uint32_t tapCount = std::min<uint32_t>(shutter.tapCount, GpuMovieShutterParams::kMaxTaps);
            if (tapCount == 0 || width <= 0 || height <= 0) return;
            const float halfW = static_cast<float>(width) * 0.5f;
            const float halfH = static_cast<float>(height) * 0.5f;
            const float maxPx = static_cast<float>(width - 1);
            const float maxPy = static_cast<float>(height - 1);
            const float firstSx = shutter.shiftX[0], firstSy = shutter.shiftY[0], firstSc = shutter.scale[0];
            const float lastSx = shutter.shiftX[tapCount - 1], lastSy = shutter.shiftY[tapCount - 1], lastSc = shutter.scale[tapCount - 1];

            #pragma omp parallel for schedule(static)
            for (int y = 0; y < height; ++y)
            {
                if (cancel.load(std::memory_order_relaxed)) continue;
                const float relY = static_cast<float>(y) + 0.5f - halfH;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width);

                for (int x = 0; x < width; ++x)
                {
                    const size_t idx = rowBase + static_cast<size_t>(x);
                    const uint32_t centerColor = rawPixels[idx];
                    const float relX = static_cast<float>(x) + 0.5f - halfW;

                    const float spanX = std::abs((lastSx - firstSx) + relX * (lastSc - firstSc));
                    const float spanY = std::abs((lastSy - firstSy) + relY * (lastSc - firstSc));
                    if (std::max(spanX, spanY) < 0.125f)
                    {
                        outPixels[idx] = centerColor;
                        continue;
                    }

                    float sumB = 0.0f, sumG = 0.0f, sumR = 0.0f;
                    uint32_t validCount = 0;
                    bool allEqualCenter = true;

                    for (uint32_t k = 0; k < tapCount; ++k)
                    {
                        const float px = halfW + shutter.shiftX[k] + relX * shutter.scale[k] - 0.5f;
                        const float py = halfH + shutter.shiftY[k] + relY * shutter.scale[k] - 0.5f;
                        if (px < 0.0f || px > maxPx || py < 0.0f || py > maxPy)
                            continue;

                        const int x0 = static_cast<int>(px);
                        const int y0 = static_cast<int>(py);
                        const int x1 = std::min(x0 + 1, width - 1);
                        const int y1 = std::min(y0 + 1, height - 1);
                        const float fx = px - static_cast<float>(x0);
                        const float fy = py - static_cast<float>(y0);

                        const size_t r0 = static_cast<size_t>(y0) * static_cast<size_t>(width);
                        const size_t r1 = static_cast<size_t>(y1) * static_cast<size_t>(width);
                        const uint32_t c00 = rawPixels[r0 + static_cast<size_t>(x0)];
                        const uint32_t c10 = rawPixels[r0 + static_cast<size_t>(x1)];
                        const uint32_t c01 = rawPixels[r1 + static_cast<size_t>(x0)];
                        const uint32_t c11 = rawPixels[r1 + static_cast<size_t>(x1)];

                        if (c00 == c10 && c00 == c01 && c00 == c11)
                        {
                            if (c00 != centerColor) allEqualCenter = false;
                            sumB += colors.decode[c00 & 255];
                            sumG += colors.decode[(c00 >> 8) & 255];
                            sumR += colors.decode[(c00 >> 16) & 255];
                        }
                        else
                        {
                            allEqualCenter = false;
                            const float w00 = (1.0f - fx) * (1.0f - fy);
                            const float w10 = fx * (1.0f - fy);
                            const float w01 = (1.0f - fx) * fy;
                            const float w11 = fx * fy;
                            sumB += colors.decode[c00 & 255] * w00 + colors.decode[c10 & 255] * w10 +
                                    colors.decode[c01 & 255] * w01 + colors.decode[c11 & 255] * w11;
                            sumG += colors.decode[(c00 >> 8) & 255] * w00 + colors.decode[(c10 >> 8) & 255] * w10 +
                                    colors.decode[(c01 >> 8) & 255] * w01 + colors.decode[(c11 >> 8) & 255] * w11;
                            sumR += colors.decode[(c00 >> 16) & 255] * w00 + colors.decode[(c10 >> 16) & 255] * w10 +
                                    colors.decode[(c01 >> 16) & 255] * w01 + colors.decode[(c11 >> 16) & 255] * w11;
                        }
                        ++validCount;
                    }

                    if (validCount == 0 || allEqualCenter)
                    {
                        outPixels[idx] = centerColor;
                        continue;
                    }

                    const float inv = 1.0f / static_cast<float>(validCount);
                    const uint32_t b = colors.encode[static_cast<size_t>(std::clamp(sumB * inv * 4096.0f + 0.5f, 0.0f, 4096.0f))];
                    const uint32_t g = colors.encode[static_cast<size_t>(std::clamp(sumG * inv * 4096.0f + 0.5f, 0.0f, 4096.0f))];
                    const uint32_t r = colors.encode[static_cast<size_t>(std::clamp(sumR * inv * 4096.0f + 0.5f, 0.0f, 4096.0f))];
                    outPixels[idx] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }

        GpuBandSkipRect ComputeBandSkipRect(
            const KeyframeInnerRect& skipRect,
            int fullW,
            int fullH,
            int globalRow,
            int topHalo,
            int visibleRows,
            int bandHeight) noexcept
        {
            GpuBandSkipRect out{};
            if (!skipRect.valid ||
                skipRect.x0 >= skipRect.x1 || skipRect.y0 >= skipRect.y1)
            {
                return out;
            }

            const int sx0 = std::clamp(skipRect.x0, 0, fullW);
            const int sx1 = std::clamp(skipRect.x1, 0, fullW);
            const int sy0 = std::clamp(skipRect.y0, 0, fullH);
            const int sy1 = std::clamp(skipRect.y1, 0, fullH);
            if (sx0 >= sx1 || sy0 >= sy1) return out;

            const int visY0 = std::clamp(sy0 - globalRow, 0, visibleRows);
            const int visY1 = std::clamp(sy1 - globalRow, 0, visibleRows);
            if (visY0 < visY1)
            {
                out.visible = { sx0, visY0, sx1, visY1 };
            }

            // Primary shader skips the 1-px inset of skipRect on every non-border edge so every
            // non-skipped pixel keeps an exact 3x3 primary neighborhood for boundary detection.
            const int primGlobalX0 = (sx0 > 0) ? (sx0 + 1) : 0;
            const int primGlobalX1 = (sx1 < fullW) ? (sx1 - 1) : fullW;
            const int primGlobalY0 = (sy0 > 0) ? (sy0 + 1) : 0;
            const int primGlobalY1 = (sy1 < fullH) ? (sy1 - 1) : fullH;
            if (primGlobalX0 < primGlobalX1 && primGlobalY0 < primGlobalY1)
            {
                const int firstGlobalY = globalRow - topHalo;
                const int primY0 = std::clamp(primGlobalY0 - firstGlobalY, 0, bandHeight);
                const int primY1 = std::clamp(primGlobalY1 - firstGlobalY, 0, bandHeight);
                if (primY0 < primY1)
                {
                    out.primary = { primGlobalX0, primY0, primGlobalX1, primY1 };
                }
            }
            return out;
        }

        bool TryRenderGpuMovieKeyframe(
            const MoviePlan& plan,
            const FractalConfig& keyframeCamera,
            const KeyframeInnerRect* skipRect,
            bool keyframeOversampled,
            uint32_t* dstPixels,
            MandelbrotEngine& engine,
            const std::atomic<bool>& cancel,
            int bandRows,
            uint64_t totalWorkPixels,
            const std::function<void(float)>& progress)
        {
            const int kfW = keyframeCamera.width;
            const int kfH = keyframeCamera.height;
            if (!engine.CanUseGpu(keyframeCamera) || !engine.BeginGpuMovieFrame(kfW, kfH, false))
            {
                return false;
            }

            const int gpuBandRows = ComputeGpuBandRows(keyframeCamera, kfW, kfH, bandRows);
            const BigFloat step = BigFloat(3.0 / keyframeCamera.zoom).Mul(BigFloat(1.0 / static_cast<double>(kfW)));

            bool gpuOk = true;
            uint64_t donePixels = 0;
            for (int globalRow = 0; globalRow < kfH && gpuOk; globalRow += gpuBandRows)
            {
                if (cancel.load(std::memory_order_relaxed)) return false;
                const int rows = std::min(gpuBandRows, kfH - globalRow);
                const int topHalo = (globalRow > 0) ? 1 : 0;
                const int bottomHalo = (globalRow + rows < kfH) ? 1 : 0;
                const int firstGlobalY = globalRow - topHalo;
                const int bandHeight = rows + topHalo + bottomHalo;
                const BigFloat rowOffset((kfH - bandHeight) * 0.5 - firstGlobalY);

                auto band = keyframeCamera;
                band.height = bandHeight;
                band.renderStride = 1;
                band.edgeRefine = false;
                band.SetCenter(keyframeCamera.hpCenterX, keyframeCamera.hpCenterY + step.Mul(rowOffset));

                GpuBandSkipRect bandSkip{};
                const GpuBandSkipRect* bandSkipPtr = nullptr;
                if (skipRect)
                {
                    bandSkip = ComputeBandSkipRect(*skipRect, kfW, kfH, globalRow, topHalo, rows, bandHeight);
                    if (bandSkip.visible.IsValid() || bandSkip.primary.IsValid())
                    {
                        bandSkipPtr = &bandSkip;
                    }
                }

                uint32_t* outBand = dstPixels + static_cast<size_t>(globalRow) * static_cast<size_t>(kfW);
                gpuOk = engine.RenderGpuMovieBand(
                    band, topHalo, rows, globalRow,
                    plan.theme, plan.frequency, plan.phase, plan.invert, plan.interior,
                    outBand, &cancel, keyframeOversampled, bandSkipPtr);
                if (cancel.load(std::memory_order_relaxed)) return false;

                if (gpuOk)
                {
                    uint64_t bandWorkPixels = static_cast<uint64_t>(kfW) * static_cast<uint64_t>(rows);
                    if (bandSkip.visible.IsValid())
                    {
                        bandWorkPixels -= static_cast<uint64_t>(bandSkip.visible.x1 - bandSkip.visible.x0) *
                                          static_cast<uint64_t>(bandSkip.visible.y1 - bandSkip.visible.y0);
                    }
                    donePixels += bandWorkPixels;
                    if (progress && totalWorkPixels > 0)
                    {
                        progress(static_cast<float>(static_cast<double>(donePixels) / static_cast<double>(totalWorkPixels)));
                    }
                }
            }

            if (cancel.load(std::memory_order_relaxed)) return false;
            return gpuOk && engine.FinishGpuMovieFrame(kfW, kfH, dstPixels, nullptr);
        }

        bool RenderMovieCameraRect(
            const MoviePlan& plan,
            const FractalConfig& fullCamera,
            int rx0,
            int ry0,
            int rw,
            int rh,
            uint32_t* dstPixels,
            MandelbrotEngine& engine,
            ExecutionMode effectiveMode,
            const std::atomic<bool>& cancel,
            int bandRows,
            uint64_t& completedPixels,
            uint64_t totalWorkPixels,
            const std::function<void(float)>& progress,
            bool keyframeOversampled = false)
        {
            if (rw <= 0 || rh <= 0) return true;
            if (cancel.load(std::memory_order_relaxed)) return false;

            const int leftHalo = (rx0 > 0) ? 1 : 0;
            const int rightHalo = (rx0 + rw < fullCamera.width) ? 1 : 0;
            const int subFirstX = rx0 - leftHalo;
            const int subW = rw + leftHalo + rightHalo;
            const bool fullWidthDirect = (leftHalo == 0 && rightHalo == 0 && rw == fullCamera.width);

            const BigFloat step = BigFloat(3.0 / fullCamera.zoom).Mul(BigFloat(1.0 / static_cast<double>(fullCamera.width)));
            auto subCam = fullCamera;
            subCam.width = subW;
            subCam.height = rh;
            subCam.renderStride = 1;
            subCam.edgeRefine = false;
            subCam.zoom = fullWidthDirect
                ? fullCamera.zoom
                : (fullCamera.zoom * (static_cast<double>(fullCamera.width) / static_cast<double>(subW)));
            const BigFloat subCenterX = fullWidthDirect
                ? fullCamera.hpCenterX
                : (fullCamera.hpCenterX + step.Mul(BigFloat(static_cast<double>(subFirstX) + static_cast<double>(subW) * 0.5 - static_cast<double>(fullCamera.width) * 0.5)));
            subCam.SetCenter(subCenterX, fullCamera.hpCenterY);

            const size_t subVisiblePixels = static_cast<size_t>(subW) * static_cast<size_t>(rh);
            const bool gpuFullRect = (subVisiblePixels <= engine.GetGpuFullFrameAccumMaxPixels());

            auto copySubRows = [&](const uint32_t* srcBase, int localStartRow, int rowCount)
            {
                for (int y = 0; y < rowCount; ++y)
                {
                    const uint32_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(subW) + leftHalo;
                    uint32_t* dstRow = dstPixels + static_cast<size_t>(ry0 + localStartRow + y) * static_cast<size_t>(fullCamera.width) + rx0;
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(rw) * sizeof(uint32_t));
                }
            };

            if (effectiveMode == ExecutionMode::Gpu &&
                engine.CanUseGpu(subCam) &&
                engine.BeginGpuMovieFrame(subW, rh, false))
            {
                const int gpuBandRows = ComputeGpuBandRows(subCam, subW, rh, bandRows);

                std::vector<uint32_t> gpuRectScratch;
                uint32_t* gpuRectTarget = nullptr;
                if (gpuFullRect)
                {
                    if (fullWidthDirect)
                    {
                        gpuRectTarget = dstPixels + static_cast<size_t>(ry0) * static_cast<size_t>(fullCamera.width);
                    }
                    else
                    {
                        gpuRectScratch.resize(subVisiblePixels);
                        gpuRectTarget = gpuRectScratch.data();
                    }
                }
                else if (!fullWidthDirect)
                {
                    gpuRectScratch.resize(static_cast<size_t>(subW) * static_cast<size_t>(gpuBandRows));
                }

                bool gpuOk = true;
                uint64_t rectProgressDone = 0;
                for (int localRow = 0; localRow < rh && gpuOk; localRow += gpuBandRows)
                {
                    if (cancel.load(std::memory_order_relaxed)) return false;
                    const int rows = std::min(gpuBandRows, rh - localRow);
                    const int globalRow = ry0 + localRow;
                    const int topHalo = (globalRow > 0) ? 1 : 0;
                    const int bottomHalo = (globalRow + rows < fullCamera.height) ? 1 : 0;
                    const int firstGlobalY = globalRow - topHalo;
                    const int bandHeight = rows + topHalo + bottomHalo;
                    const BigFloat rowOffset((fullCamera.height - bandHeight) * 0.5 - firstGlobalY);

                    auto band = subCam;
                    band.height = bandHeight;
                    band.SetCenter(subCenterX, fullCamera.hpCenterY + step.Mul(rowOffset));

                    uint32_t* outBand = nullptr;
                    if (!gpuFullRect)
                    {
                        outBand = fullWidthDirect
                            ? (dstPixels + static_cast<size_t>(globalRow) * static_cast<size_t>(fullCamera.width))
                            : gpuRectScratch.data();
                    }
                    else
                    {
                        outBand = gpuRectTarget + static_cast<size_t>(localRow) * static_cast<size_t>(subW);
                    }

                    gpuOk = engine.RenderGpuMovieBand(
                        band, topHalo, rows, localRow,
                        plan.theme, plan.frequency, plan.phase, plan.invert, plan.interior,
                        outBand, &cancel, keyframeOversampled, nullptr);
                    if (cancel.load(std::memory_order_relaxed)) return false;

                    if (gpuOk)
                    {
                        if (!gpuFullRect && !fullWidthDirect)
                        {
                            copySubRows(gpuRectScratch.data(), localRow, rows);
                        }
                        rectProgressDone += static_cast<uint64_t>(rw) * static_cast<uint64_t>(rows);
                        if (progress && totalWorkPixels > 0)
                        {
                            progress(static_cast<float>(static_cast<double>(completedPixels + rectProgressDone) / static_cast<double>(totalWorkPixels)));
                        }
                    }
                }

                if (cancel.load(std::memory_order_relaxed)) return false;
                if (gpuOk)
                {
                    if (gpuFullRect)
                    {
                        if (!engine.FinishGpuMovieFrame(subW, rh, gpuRectTarget, nullptr))
                        {
                            gpuOk = false;
                        }
                        else if (!fullWidthDirect)
                        {
                            copySubRows(gpuRectTarget, 0, rh);
                        }
                    }
                    else
                    {
                        gpuOk = engine.FinishGpuMovieFrame(subW, rh, dstPixels, nullptr);
                    }
                }
                if (gpuOk)
                {
                    completedPixels += static_cast<uint64_t>(rw) * static_cast<uint64_t>(rh);
                    return true;
                }
            }

            // CPU fallback path
            const ExecutionMode savedRectMode = engine.GetExecutionMode();
            ScopedExecutionMode rectModeGuard{ engine, savedRectMode };
            if (savedRectMode == ExecutionMode::Gpu)
            {
                engine.SetExecutionMode(ExecutionMode::Automatic);
            }
            std::vector<float> iterations;
            std::vector<SubpixelSamples> subpixels;
            std::vector<uint32_t> painted;
            for (int localRow = 0; localRow < rh; localRow += bandRows)
            {
                if (cancel.load(std::memory_order_relaxed)) return false;
                const int rows = std::min(bandRows, rh - localRow);
                const int globalRow = ry0 + localRow;
                const int topHalo = (globalRow > 0) ? 1 : 0;
                const int bottomHalo = (globalRow + rows < fullCamera.height) ? 1 : 0;
                const int firstGlobalY = globalRow - topHalo;
                const int bandHeight = rows + topHalo + bottomHalo;
                const size_t bandCount = static_cast<size_t>(subW) * static_cast<size_t>(bandHeight);
                painted.resize(bandCount);
                const BigFloat rowOffset((fullCamera.height - bandHeight) * 0.5 - firstGlobalY);

                auto band = subCam;
                band.height = bandHeight;
                band.SetCenter(subCenterX, fullCamera.hpCenterY + step.Mul(rowOffset));
                engine.Compute(band, iterations, &cancel, &subpixels);
                if (cancel.load(std::memory_order_relaxed)) return false;

                plan.theme.Recolor(iterations, painted.data(), bandCount, plan.frequency, plan.phase,
                    plan.invert, plan.interior, band.fractalType, band.width, &subpixels, false);

                const uint32_t* visBase = painted.data() + static_cast<size_t>(topHalo) * static_cast<size_t>(subW);
                if (fullWidthDirect)
                {
                    uint32_t* dstRow = dstPixels + static_cast<size_t>(globalRow) * static_cast<size_t>(fullCamera.width);
                    std::memcpy(dstRow, visBase, static_cast<size_t>(rw) * static_cast<size_t>(rows) * sizeof(uint32_t));
                }
                else
                {
                    copySubRows(visBase, localRow, rows);
                }

                completedPixels += static_cast<uint64_t>(rw) * static_cast<uint64_t>(rows);
                if (progress && totalWorkPixels > 0)
                {
                    progress(static_cast<float>(static_cast<double>(completedPixels) / static_cast<double>(totalWorkPixels)));
                }
            }
            return true;
        }
    }

    std::vector<uint32_t> RenderMovieFrame(const MoviePlan& plan, double frameIndex,
        const std::atomic<bool>& cancel, const std::function<void(float)>& progress,
        int bandRows, MandelbrotEngine* sharedEngine)
    {
        plan.Validate();
        if (bandRows < 1) throw std::invalid_argument("Invalid movie render band size.");
        if (cancel.load(std::memory_order_relaxed)) return {};
        const auto& colors = detail::LinearSrgbTables::Instance();
        const ExecutionMode requestedMode = plan.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic;
        std::optional<MandelbrotEngine> localEngine;
        if (!sharedEngine)
        {
            localEngine.emplace(requestedMode);
        }
        MandelbrotEngine& engine = sharedEngine ? *sharedEngine : *localEngine;
        const ExecutionMode previousMode = engine.GetExecutionMode();
        const ExecutionMode effectiveMode = (previousMode == ExecutionMode::Scalar) ? ExecutionMode::Scalar : requestedMode;
        ScopedExecutionMode modeGuard{ engine, previousMode };
        engine.SetExecutionMode(effectiveMode);

        const size_t totalPixels = static_cast<size_t>(plan.width) * plan.height;
        std::vector<uint32_t> output(totalPixels), rawOutput, painted;
        std::vector<float> iterations;
        std::vector<SubpixelSamples> subpixels;
        const MandelbrotConfig camera = plan.CameraAt(frameIndex);
        const BigFloat step(3.0 / camera.zoom / plan.width);
        GpuMovieShutterParams shutter{};
        const bool hasShutter = detail::BuildShutterParams(plan, frameIndex, camera, shutter);
        const bool gpuFullFrame = (totalPixels <= engine.GetGpuFullFrameAccumMaxPixels());

        if (effectiveMode == ExecutionMode::Gpu &&
            engine.CanUseGpu(camera) &&
            engine.BeginGpuMovieFrame(plan.width, plan.height, hasShutter && gpuFullFrame))
        {
            const int gpuBandRows = ComputeGpuBandRows(camera, plan.width, plan.height, bandRows);

            if (hasShutter && !gpuFullFrame)
            {
                rawOutput.resize(totalPixels);
            }
            uint32_t* bandTargetBase = (hasShutter && !gpuFullFrame) ? rawOutput.data() : output.data();

            bool gpuOk = true;
            engine.PrepareDeepReference(camera, &cancel);
            if (cancel.load(std::memory_order_relaxed)) return {};

            for (int row = 0; row < plan.height && gpuOk; row += gpuBandRows)
            {
                if (cancel.load(std::memory_order_relaxed)) return {};
                const int rows = std::min(gpuBandRows, plan.height - row);
                const int topHalo = row > 0 ? 1 : 0;
                const int bottomHalo = (row + rows < plan.height) ? 1 : 0;
                const int firstRow = row - topHalo;
                const int bandHeight = rows + topHalo + bottomHalo;
                const BigFloat rowOffset((plan.height - bandHeight) * .5 - firstRow);

                auto band = camera;
                band.height = bandHeight;
                band.SetCenter(camera.hpCenterX,
                    camera.hpCenterY + step.Mul(rowOffset));

                uint32_t* outBand = bandTargetBase + static_cast<size_t>(row) * plan.width;
                gpuOk = engine.RenderGpuMovieBand(
                    band, topHalo, rows, row,
                    plan.theme, plan.frequency, plan.phase, plan.invert, plan.interior,
                    outBand, &cancel);
                if (cancel.load(std::memory_order_relaxed)) return {};

                if (gpuOk && progress)
                {
                    progress(static_cast<float>(static_cast<double>(row + rows) / plan.height));
                }
            }

            if (cancel.load(std::memory_order_relaxed)) return {};
            if (gpuOk && engine.FinishGpuMovieFrame(
                plan.width,
                plan.height,
                output.data(),
                (hasShutter && gpuFullFrame) ? &shutter : nullptr))
            {
                if (hasShutter && !gpuFullFrame)
                {
                    ApplyCpuShutterBlur(rawOutput, output, plan.width, plan.height, shutter, colors, cancel);
                    if (cancel.load(std::memory_order_relaxed)) return {};
                }
                return output;
            }
        }

        if (engine.GetExecutionMode() == ExecutionMode::Gpu)
        {
            engine.SetExecutionMode(ExecutionMode::Automatic);
        }

        if (hasShutter)
        {
            rawOutput.resize(totalPixels);
        }
        uint32_t* cpuTargetBase = hasShutter ? rawOutput.data() : output.data();

        engine.PrepareDeepReference(camera, &cancel);
        if (cancel.load(std::memory_order_relaxed)) return {};

        for (int row = 0; row < plan.height; row += bandRows)
        {
            if (cancel.load(std::memory_order_relaxed)) return {};
            const int rows = std::min(bandRows, plan.height - row);
            const int topHalo = row > 0 ? 1 : 0;
            const int bottomHalo = (row + rows < plan.height) ? 1 : 0;
            const int firstRow = row - topHalo;
            const int bandHeight = rows + topHalo + bottomHalo;
            const size_t count = static_cast<size_t>(plan.width) * rows;
            const size_t bandCount = static_cast<size_t>(plan.width) * bandHeight;
            painted.resize(bandCount);
            const BigFloat rowOffset((plan.height - bandHeight) * .5 - firstRow);

            auto band = camera;
            band.height = bandHeight;
            band.SetCenter(camera.hpCenterX,
                camera.hpCenterY + step.Mul(rowOffset));
            engine.Compute(band, iterations, &cancel, &subpixels);
            if (cancel.load(std::memory_order_relaxed)) return {};
            // Halo rows provide complete 3x3 neighborhoods across band boundaries, making
            // adaptive subpixel refinement and colors independent of render bands.
            plan.theme.Recolor(iterations, painted.data(), bandCount, plan.frequency, plan.phase,
                plan.invert, plan.interior, band.fractalType, band.width, &subpixels, false);
            const uint32_t* bandPixels = painted.data() + static_cast<size_t>(topHalo) * plan.width;
            uint32_t* outBand = cpuTargetBase + static_cast<size_t>(row) * plan.width;
            std::memcpy(outBand, bandPixels, count * sizeof(uint32_t));
            if (progress)
            {
                progress(static_cast<float>(static_cast<double>(row + rows) / plan.height));
            }
        }

        if (hasShutter)
        {
            ApplyCpuShutterBlur(rawOutput, output, plan.width, plan.height, shutter, colors, cancel);
            if (cancel.load(std::memory_order_relaxed)) return {};
        }
        return output;
    }

    std::vector<uint32_t> RenderMovieKeyframe(
        const MoviePlan& plan,
        const FractalConfig& keyframeCamera,
        const KeyframeInnerRect* skipRect,
        const std::atomic<bool>& cancel,
        const std::function<void(float)>& progress,
        int bandRows,
        MandelbrotEngine* sharedEngine)
    {
        if (bandRows < 1 || keyframeCamera.width < 1 || keyframeCamera.height < 1)
            throw std::invalid_argument("Invalid keyframe render dimensions.");
        if (cancel.load(std::memory_order_relaxed)) return {};

        const ExecutionMode requestedMode = plan.useGpu ? ExecutionMode::Gpu : ExecutionMode::Automatic;
        std::optional<MandelbrotEngine> localEngine;
        if (!sharedEngine)
        {
            localEngine.emplace(requestedMode);
        }
        MandelbrotEngine& engine = sharedEngine ? *sharedEngine : *localEngine;
        const ExecutionMode previousMode = engine.GetExecutionMode();
        const ExecutionMode effectiveMode = (previousMode == ExecutionMode::Scalar) ? ExecutionMode::Scalar : requestedMode;
        ScopedExecutionMode modeGuard{ engine, previousMode };
        engine.SetExecutionMode(effectiveMode);

        const int kfW = keyframeCamera.width;
        const int kfH = keyframeCamera.height;
        std::vector<uint32_t> output(static_cast<size_t>(kfW) * static_cast<size_t>(kfH), 0);

        engine.PrepareDeepReference(keyframeCamera, &cancel);
        if (cancel.load(std::memory_order_relaxed)) return {};

        const bool useSkip = (skipRect != nullptr && skipRect->valid &&
                              skipRect->x0 >= 0 && skipRect->y0 >= 0 &&
                              skipRect->x1 <= kfW && skipRect->y1 <= kfH &&
                              skipRect->x0 < skipRect->x1 && skipRect->y0 < skipRect->y1);
        const bool keyframeOversampled =
            engine.IsGpuKeyframeSubpixelBudgetEnabled() &&
            plan.width > 0 && plan.height > 0 &&
            kfW >= plan.width + (plan.width / 4) &&
            kfH >= plan.height + (plan.height / 4);

        uint64_t totalWorkPixels = static_cast<uint64_t>(kfW) * static_cast<uint64_t>(kfH);
        if (useSkip)
        {
            totalWorkPixels -= static_cast<uint64_t>(skipRect->x1 - skipRect->x0) *
                               static_cast<uint64_t>(skipRect->y1 - skipRect->y0);
        }

        ExecutionMode rectMode = effectiveMode;
        if (effectiveMode == ExecutionMode::Gpu &&
            (!useSkip || engine.IsGpuSinglePassSkipRectEnabled()))
        {
            if (TryRenderGpuMovieKeyframe(
                    plan, keyframeCamera, useSkip ? skipRect : nullptr, keyframeOversampled,
                    output.data(), engine, cancel, bandRows, totalWorkPixels, progress))
            {
                return output;
            }
            if (cancel.load(std::memory_order_relaxed)) return {};
            // Single-pass GPU path failed (not canceled); switch to CPU and prepare the full
            // keyframe CPU deep reference once before rendering fallback strips.
            rectMode = ExecutionMode::Automatic;
            engine.SetExecutionMode(ExecutionMode::Automatic);
            engine.PrepareDeepReference(keyframeCamera, &cancel);
            if (cancel.load(std::memory_order_relaxed)) return {};
        }

        uint64_t completedPixels = 0;

        if (!useSkip)
        {
            if (!RenderMovieCameraRect(plan, keyframeCamera, 0, 0, kfW, kfH,
                output.data(), engine, rectMode, cancel, bandRows, completedPixels, totalWorkPixels, progress,
                keyframeOversampled))
            {
                return {};
            }
            return output;
        }

        // Render the 4 outer picture-frame strips around the skipped interior rectangle.
        // Order Top & Bottom first (both at full keyframe zoom), then Left & Right.
        const std::array<std::array<int, 4>, 4> strips{{
            { 0, 0, kfW, skipRect->y0 },
            { 0, skipRect->y1, kfW, kfH - skipRect->y1 },
            { 0, skipRect->y0, skipRect->x0, skipRect->y1 - skipRect->y0 },
            { skipRect->x1, skipRect->y0, kfW - skipRect->x1, skipRect->y1 - skipRect->y0 }
        }};

        for (const auto& s : strips)
        {
            if (s[2] <= 0 || s[3] <= 0) continue;
            if (!RenderMovieCameraRect(plan, keyframeCamera, s[0], s[1], s[2], s[3],
                output.data(), engine, rectMode, cancel, bandRows, completedPixels, totalWorkPixels, progress,
                keyframeOversampled))
            {
                return {};
            }
        }
        return output;
    }

    MoviePreview MakeMoviePreview(const std::vector<uint32_t>& pixels, int width, int height, uint32_t frame)
    {
        if (width < 1 || height < 1 || width > 7680 || height > 4320 || pixels.size() != static_cast<size_t>(width) * height)
            throw std::invalid_argument("Invalid movie preview dimensions.");
        MoviePreview preview;
        preview.width = std::min(960, width);
        preview.height = std::max(1, height * preview.width / width);
        preview.frame = frame;
        preview.pixels.resize(static_cast<size_t>(preview.width) * preview.height);
        #pragma omp parallel for schedule(static)
        for (int y = 0; y < preview.height; ++y)
            for (int x = 0; x < preview.width; ++x)
            {
                uint32_t sums[3]{}; uint32_t count = 0;
                for (int sy = y * height / preview.height; sy < (y + 1) * height / preview.height; ++sy)
                    for (int sx = x * width / preview.width; sx < (x + 1) * width / preview.width; ++sx)
                    {
                        const auto c = pixels[static_cast<size_t>(sy) * width + sx];
                        for (int ch = 0; ch < 3; ++ch) sums[ch] += (c >> (8 * ch)) & 255;
                        ++count;
                    }
                uint32_t color = 0xFF000000;
                for (int ch = 0; ch < 3; ++ch) color |= ((sums[ch] + count / 2) / count) << (8 * ch);
                preview.pixels[static_cast<size_t>(y) * preview.width + x] = color;
            }
        return preview;
    }
}
