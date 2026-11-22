#include "MovieZoomPyramid.h"
#include "ColorTheme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Mandelbrot
{
    namespace
    {
        constexpr int kMaxTaps1D = 8;

        struct FilterTaps1D
        {
            int start{};
            int count{};
            float weights[kMaxTaps1D]{};
        };

        // Mitchell-Netravali cubic filter (B = 1/3, C = 1/3): crisp downsampling with minimal ringing.
        inline float MitchellWeight(float x) noexcept
        {
            x = std::abs(x);
            if (x < 1.0f)
            {
                return ((7.0f * x - 12.0f) * x * x + (16.0f / 3.0f)) * (1.0f / 6.0f);
            }
            if (x < 2.0f)
            {
                return (((-7.0f / 3.0f) * x + 12.0f) * x - 20.0f) * x * (1.0f / 6.0f) + (32.0f / 18.0f);
            }
            return 0.0f;
        }

        void BuildAxisTaps(
            int outStart,
            int outCount,
            int outFullSize,
            int srcSize,
            double offset,
            double rho,
            std::vector<FilterTaps1D>& outTaps,
            int& outMinSrc,
            int& outMaxSrc)
        {
            outTaps.resize(static_cast<size_t>(outCount));
            const double scale = std::max(1.0, rho);
            const double invScale = 1.0 / scale;
            const double support = 2.0 * scale;
            const double outHalf = static_cast<double>(outFullSize) * 0.5;
            const double srcHalf = static_cast<double>(srcSize) * 0.5;

            int minSrc = srcSize - 1;
            int maxSrc = 0;

            for (int i = 0; i < outCount; ++i)
            {
                const double rel = static_cast<double>(outStart + i) + 0.5 - outHalf;
                const double center = srcHalf + offset + rho * rel - 0.5;

                int i0 = static_cast<int>(std::floor(center - support)) + 1;
                int i1 = static_cast<int>(std::floor(center + support));
                if (i1 - i0 + 1 > kMaxTaps1D)
                {
                    const int mid = static_cast<int>(std::floor(center));
                    i0 = mid - (kMaxTaps1D / 2 - 1);
                    i1 = i0 + kMaxTaps1D - 1;
                }
                i0 = std::clamp(i0, 0, srcSize - 1);
                i1 = std::clamp(i1, i0, srcSize - 1);

                FilterTaps1D taps{};
                taps.start = i0;
                taps.count = i1 - i0 + 1;

                float sum = 0.0f;
                for (int k = 0; k < taps.count; ++k)
                {
                    const float w = MitchellWeight(static_cast<float>((static_cast<double>(i0 + k) - center) * invScale));
                    taps.weights[k] = w;
                    sum += w;
                }
                if (sum > 1e-6f)
                {
                    const float invSum = 1.0f / sum;
                    for (int k = 0; k < taps.count; ++k)
                    {
                        taps.weights[k] *= invSum;
                    }
                }
                else
                {
                    taps.start = std::clamp(static_cast<int>(std::lround(center)), 0, srcSize - 1);
                    taps.count = 1;
                    taps.weights[0] = 1.0f;
                }

                minSrc = std::min(minSrc, taps.start);
                maxSrc = std::max(maxSrc, taps.start + taps.count - 1);
                outTaps[static_cast<size_t>(i)] = taps;
            }

            outMinSrc = minSrc;
            outMaxSrc = maxSrc;
        }

        // Resamples a sub-rectangle [outX0, outX0 + outW) x [outY0, outY0 + outH) of an output
        // camera from `srcPixels` into linear RGB float triples (B, G, R).
        void ResampleRectLinear(
            const std::vector<uint32_t>& srcPixels,
            const FractalConfig& srcKf,
            const FractalConfig& dstCam,
            int outX0,
            int outY0,
            int outW,
            int outH,
            std::vector<float>& outLinearBgr,
            const std::atomic<bool>& cancel)
        {
            if (outW <= 0 || outH <= 0 || srcKf.width <= 0 || srcKf.height <= 0 ||
                srcPixels.size() < static_cast<size_t>(srcKf.width) * static_cast<size_t>(srcKf.height))
            {
                outLinearBgr.clear();
                return;
            }
            outLinearBgr.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 3);

            const auto t = ComputeCameraToKeyframe(srcKf, dstCam);
            const auto& colors = detail::LinearSrgbTables::Instance();

            std::vector<FilterTaps1D> xTaps;
            std::vector<FilterTaps1D> yTaps;
            int minSrcX = 0, maxSrcX = 0;
            int minSrcY = 0, maxSrcY = 0;
            BuildAxisTaps(outX0, outW, dstCam.width, srcKf.width, t.offsetX, t.rho, xTaps, minSrcX, maxSrcX);
            BuildAxisTaps(outY0, outH, dstCam.height, srcKf.height, t.offsetY, t.rho, yTaps, minSrcY, maxSrcY);

            const int bandSrcH = maxSrcY - minSrcY + 1;
            std::vector<float> horiz(static_cast<size_t>(bandSrcH) * static_cast<size_t>(outW) * 3);

            // Pass 1: Horizontal linear-light Mitchell-Netravali over touched source rows.
            #pragma omp parallel for schedule(static)
            for (int ry = 0; ry < bandSrcH; ++ry)
            {
                if (cancel.load(std::memory_order_relaxed)) continue;
                const uint32_t* srcRow = srcPixels.data() + static_cast<size_t>(minSrcY + ry) * static_cast<size_t>(srcKf.width);
                float* dstRow = horiz.data() + static_cast<size_t>(ry) * static_cast<size_t>(outW) * 3;
                for (int ox = 0; ox < outW; ++ox)
                {
                    const auto& tap = xTaps[static_cast<size_t>(ox)];
                    float b = 0.0f, g = 0.0f, r = 0.0f;
                    for (int k = 0; k < tap.count; ++k)
                    {
                        const uint32_t px = srcRow[tap.start + k];
                        const float w = tap.weights[k];
                        b += colors.decode[px & 255u] * w;
                        g += colors.decode[(px >> 8) & 255u] * w;
                        r += colors.decode[(px >> 16) & 255u] * w;
                    }
                    float* out3 = dstRow + static_cast<size_t>(ox) * 3;
                    out3[0] = b;
                    out3[1] = g;
                    out3[2] = r;
                }
            }

            if (cancel.load(std::memory_order_relaxed)) return;

            // Pass 2: Vertical linear-light Mitchell-Netravali into outLinearBgr.
            const size_t horizRowFloats = static_cast<size_t>(outW) * 3;
            #pragma omp parallel for schedule(static)
            for (int oy = 0; oy < outH; ++oy)
            {
                if (cancel.load(std::memory_order_relaxed)) continue;
                const auto& tap = yTaps[static_cast<size_t>(oy)];
                float* dstRow = outLinearBgr.data() + static_cast<size_t>(oy) * horizRowFloats;
                for (int ox = 0; ox < outW; ++ox)
                {
                    float b = 0.0f, g = 0.0f, r = 0.0f;
                    const size_t colOff = static_cast<size_t>(ox) * 3;
                    for (int k = 0; k < tap.count; ++k)
                    {
                        const float* hPx = horiz.data() + static_cast<size_t>(tap.start + k - minSrcY) * horizRowFloats + colOff;
                        const float w = tap.weights[k];
                        b += hPx[0] * w;
                        g += hPx[1] * w;
                        r += hPx[2] * w;
                    }
                    dstRow[colOff + 0] = std::clamp(b, 0.0f, 1.0f);
                    dstRow[colOff + 1] = std::clamp(g, 0.0f, 1.0f);
                    dstRow[colOff + 2] = std::clamp(r, 0.0f, 1.0f);
                }
            }
        }

        inline uint32_t EncodeLinearBgr(float b, float g, float r, const detail::LinearSrgbTables& colors) noexcept
        {
            const uint32_t sb = colors.encode[static_cast<size_t>(std::clamp(b * 4096.0f + 0.5f, 0.0f, 4096.0f))];
            const uint32_t sg = colors.encode[static_cast<size_t>(std::clamp(g * 4096.0f + 0.5f, 0.0f, 4096.0f))];
            const uint32_t sr = colors.encode[static_cast<size_t>(std::clamp(r * 4096.0f + 0.5f, 0.0f, 4096.0f))];
            return 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        }

        inline void SampleBilinearLinear(
            const uint32_t* srcPixels,
            int srcW,
            int srcH,
            float fxPos,
            float fyPos,
            const detail::LinearSrgbTables& colors,
            float& outB,
            float& outG,
            float& outR) noexcept
        {
            const float cx = std::clamp(fxPos, 0.0f, static_cast<float>(srcW - 1));
            const float cy = std::clamp(fyPos, 0.0f, static_cast<float>(srcH - 1));
            const int x0 = static_cast<int>(cx);
            const int y0 = static_cast<int>(cy);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const int y1 = std::min(y0 + 1, srcH - 1);
            const float fx = cx - static_cast<float>(x0);
            const float fy = cy - static_cast<float>(y0);

            const size_t r0 = static_cast<size_t>(y0) * static_cast<size_t>(srcW);
            const size_t r1 = static_cast<size_t>(y1) * static_cast<size_t>(srcW);
            const uint32_t c00 = srcPixels[r0 + static_cast<size_t>(x0)];
            const uint32_t c10 = srcPixels[r0 + static_cast<size_t>(x1)];
            const uint32_t c01 = srcPixels[r1 + static_cast<size_t>(x0)];
            const uint32_t c11 = srcPixels[r1 + static_cast<size_t>(x1)];

            if (c00 == c10 && c00 == c01 && c00 == c11)
            {
                outB = colors.decode[c00 & 255u];
                outG = colors.decode[(c00 >> 8) & 255u];
                outR = colors.decode[(c00 >> 16) & 255u];
                return;
            }

            const float w00 = (1.0f - fx) * (1.0f - fy);
            const float w10 = fx * (1.0f - fy);
            const float w01 = (1.0f - fx) * fy;
            const float w11 = fx * fy;
            outB = colors.decode[c00 & 255u] * w00 + colors.decode[c10 & 255u] * w10 +
                   colors.decode[c01 & 255u] * w01 + colors.decode[c11 & 255u] * w11;
            outG = colors.decode[(c00 >> 8) & 255u] * w00 + colors.decode[(c10 >> 8) & 255u] * w10 +
                   colors.decode[(c01 >> 8) & 255u] * w01 + colors.decode[(c11 >> 8) & 255u] * w11;
            outR = colors.decode[(c00 >> 16) & 255u] * w00 + colors.decode[(c10 >> 16) & 255u] * w10 +
                   colors.decode[(c01 >> 16) & 255u] * w01 + colors.decode[(c11 >> 16) & 255u] * w11;
        }

        // Samples a single shutter tap from a keyframe in linear light; uses a 2x2 bilinear box
        // when rho > 1.15 so downsampled shutter taps stay alias-free perpendicular to motion.
        inline void SampleKeyframeTapLinear(
            const uint32_t* srcPixels,
            int srcW,
            int srcH,
            float kx,
            float ky,
            float boxOffset,
            const detail::LinearSrgbTables& colors,
            float& outB,
            float& outG,
            float& outR) noexcept
        {
            if (boxOffset <= 0.0f)
            {
                SampleBilinearLinear(srcPixels, srcW, srcH, kx, ky, colors, outB, outG, outR);
                return;
            }
            float b0, g0, r0, b1, g1, r1, b2, g2, r2, b3, g3, r3;
            SampleBilinearLinear(srcPixels, srcW, srcH, kx - boxOffset, ky - boxOffset, colors, b0, g0, r0);
            SampleBilinearLinear(srcPixels, srcW, srcH, kx + boxOffset, ky - boxOffset, colors, b1, g1, r1);
            SampleBilinearLinear(srcPixels, srcW, srcH, kx - boxOffset, ky + boxOffset, colors, b2, g2, r2);
            SampleBilinearLinear(srcPixels, srcW, srcH, kx + boxOffset, ky + boxOffset, colors, b3, g3, r3);
            outB = 0.25f * (b0 + b1 + b2 + b3);
            outG = 0.25f * (g0 + g1 + g2 + g3);
            outR = 0.25f * (r0 + r1 + r2 + r3);
        }

        inline void SampleMitchellLinear(
            const uint32_t* srcPixels,
            int srcW,
            int srcH,
            float kx,
            float ky,
            float rho,
            const detail::LinearSrgbTables& colors,
            float& outB,
            float& outG,
            float& outR) noexcept
        {
            const float scale = std::max(1.0f, rho);
            const float invScale = 1.0f / scale;
            const float support = 2.0f * scale;

            int x0 = static_cast<int>(std::floor(kx - support)) + 1;
            int x1 = static_cast<int>(std::floor(kx + support));
            if (x1 - x0 + 1 > kMaxTaps1D)
            {
                const int mid = static_cast<int>(std::floor(kx));
                x0 = mid - (kMaxTaps1D / 2 - 1);
                x1 = x0 + kMaxTaps1D - 1;
            }
            x0 = std::clamp(x0, 0, srcW - 1);
            x1 = std::clamp(x1, x0, srcW - 1);

            int y0 = static_cast<int>(std::floor(ky - support)) + 1;
            int y1 = static_cast<int>(std::floor(ky + support));
            if (y1 - y0 + 1 > kMaxTaps1D)
            {
                const int mid = static_cast<int>(std::floor(ky));
                y0 = mid - (kMaxTaps1D / 2 - 1);
                y1 = y0 + kMaxTaps1D - 1;
            }
            y0 = std::clamp(y0, 0, srcH - 1);
            y1 = std::clamp(y1, y0, srcH - 1);

            float wx[kMaxTaps1D]{};
            const int nx = x1 - x0 + 1;
            float sumWx = 0.0f;
            for (int i = 0; i < nx; ++i)
            {
                wx[i] = MitchellWeight((static_cast<float>(x0 + i) - kx) * invScale);
                sumWx += wx[i];
            }

            float sumB = 0.0f, sumG = 0.0f, sumR = 0.0f, sumWy = 0.0f;
            const int ny = y1 - y0 + 1;
            for (int j = 0; j < ny; ++j)
            {
                const float wy = MitchellWeight((static_cast<float>(y0 + j) - ky) * invScale);
                sumWy += wy;
                const uint32_t* row = srcPixels + static_cast<size_t>(y0 + j) * static_cast<size_t>(srcW);
                float rb = 0.0f, rg = 0.0f, rr = 0.0f;
                for (int i = 0; i < nx; ++i)
                {
                    const uint32_t px = row[x0 + i];
                    const float w = wx[i];
                    rb += colors.decode[px & 255u] * w;
                    rg += colors.decode[(px >> 8) & 255u] * w;
                    rr += colors.decode[(px >> 16) & 255u] * w;
                }
                sumB += rb * wy;
                sumG += rg * wy;
                sumR += rr * wy;
            }

            const float denom = sumWx * sumWy;
            if (denom > 1e-6f)
            {
                const float inv = 1.0f / denom;
                outB = std::clamp(sumB * inv, 0.0f, 1.0f);
                outG = std::clamp(sumG * inv, 0.0f, 1.0f);
                outR = std::clamp(sumR * inv, 0.0f, 1.0f);
            }
            else
            {
                SampleBilinearLinear(srcPixels, srcW, srcH, kx, ky, colors, outB, outG, outR);
            }
        }
    }

    void ResampleKeyframeFrame(
        const std::vector<uint32_t>& primaryPixels,
        const FractalConfig& primaryKf,
        const std::vector<uint32_t>* secondaryPixels,
        const FractalConfig* secondaryKf,
        float blendWeight,
        const FractalConfig& outputCamera,
        const GpuMovieShutterParams* shutter,
        std::vector<uint32_t>& outPixels,
        const std::atomic<bool>& cancel)
    {
        const int outW = outputCamera.width;
        const int outH = outputCamera.height;
        if (outW <= 0 || outH <= 0 || primaryKf.width <= 0 || primaryKf.height <= 0 ||
            primaryPixels.size() < static_cast<size_t>(primaryKf.width) * static_cast<size_t>(primaryKf.height) ||
            cancel.load(std::memory_order_relaxed))
        {
            outPixels.clear();
            return;
        }
        const size_t totalPixels = static_cast<size_t>(outW) * static_cast<size_t>(outH);
        outPixels.resize(totalPixels);

        const auto& colors = detail::LinearSrgbTables::Instance();
        const bool hasCrossfade =
            (secondaryPixels != nullptr && secondaryKf != nullptr &&
             blendWeight > 1e-4f && blendWeight < 1.0f - 1e-4f);
        const bool hasShutter = (shutter != nullptr && shutter->tapCount > 1);

        if (!hasShutter)
        {
            std::vector<float> linA;
            ResampleRectLinear(primaryPixels, primaryKf, outputCamera, 0, 0, outW, outH, linA, cancel);
            if (cancel.load(std::memory_order_relaxed)) return;

            if (hasCrossfade)
            {
                std::vector<float> linB;
                ResampleRectLinear(*secondaryPixels, *secondaryKf, outputCamera, 0, 0, outW, outH, linB, cancel);
                if (cancel.load(std::memory_order_relaxed)) return;

                const float wB = blendWeight;
                const float wA = 1.0f - wB;
                #pragma omp parallel for schedule(static)
                for (int y = 0; y < outH; ++y)
                {
                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(outW);
                    for (int x = 0; x < outW; ++x)
                    {
                        const size_t idx = rowBase + static_cast<size_t>(x);
                        const size_t c = idx * 3;
                        outPixels[idx] = EncodeLinearBgr(
                            linA[c + 0] * wA + linB[c + 0] * wB,
                            linA[c + 1] * wA + linB[c + 1] * wB,
                            linA[c + 2] * wA + linB[c + 2] * wB,
                            colors);
                    }
                }
            }
            else
            {
                #pragma omp parallel for schedule(static)
                for (int y = 0; y < outH; ++y)
                {
                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(outW);
                    for (int x = 0; x < outW; ++x)
                    {
                        const size_t idx = rowBase + static_cast<size_t>(x);
                        const size_t c = idx * 3;
                        outPixels[idx] = EncodeLinearBgr(linA[c + 0], linA[c + 1], linA[c + 2], colors);
                    }
                }
            }
            return;
        }

        // Fused shutter blur directly from keyframe(s) in linear light with zero edge clipping.
        const auto tA = ComputeCameraToKeyframe(primaryKf, outputCamera);
        const float kfHalfWA = static_cast<float>(primaryKf.width) * 0.5f;
        const float kfHalfHA = static_cast<float>(primaryKf.height) * 0.5f;
        const float offXA = static_cast<float>(tA.offsetX);
        const float offYA = static_cast<float>(tA.offsetY);
        const float rhoA = static_cast<float>(tA.rho);
        const float boxOffA = (rhoA > 1.15f) ? (0.25f * (rhoA - 1.0f)) : 0.0f;

        KeyframeCameraTransform tB{};
        float kfHalfWB = 0.0f, kfHalfHB = 0.0f, offXB = 0.0f, offYB = 0.0f, rhoB = 1.0f, boxOffB = 0.0f;
        if (hasCrossfade)
        {
            tB = ComputeCameraToKeyframe(*secondaryKf, outputCamera);
            kfHalfWB = static_cast<float>(secondaryKf->width) * 0.5f;
            kfHalfHB = static_cast<float>(secondaryKf->height) * 0.5f;
            offXB = static_cast<float>(tB.offsetX);
            offYB = static_cast<float>(tB.offsetY);
            rhoB = static_cast<float>(tB.rho);
            boxOffB = (rhoB > 1.15f) ? (0.25f * (rhoB - 1.0f)) : 0.0f;
        }

        const uint32_t tapCount = std::min<uint32_t>(shutter->tapCount, GpuMovieShutterParams::kMaxTaps);
        const float invTapCount = 1.0f / static_cast<float>(tapCount);
        const float halfW = static_cast<float>(outW) * 0.5f;
        const float halfH = static_cast<float>(outH) * 0.5f;
        const float firstSx = shutter->shiftX[0], firstSy = shutter->shiftY[0], firstSc = shutter->scale[0];
        const float lastSx = shutter->shiftX[tapCount - 1], lastSy = shutter->shiftY[tapCount - 1], lastSc = shutter->scale[tapCount - 1];
        const float wB = hasCrossfade ? blendWeight : 0.0f;
        const float wA = 1.0f - wB;

        #pragma omp parallel for schedule(static)
        for (int y = 0; y < outH; ++y)
        {
            if (cancel.load(std::memory_order_relaxed)) continue;
            const float relY = static_cast<float>(y) + 0.5f - halfH;
            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(outW);

            for (int x = 0; x < outW; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float relX = static_cast<float>(x) + 0.5f - halfW;
                const float spanX = std::abs((lastSx - firstSx) + relX * (lastSc - firstSc));
                const float spanY = std::abs((lastSy - firstSy) + relY * (lastSc - firstSc));

                if (std::max(spanX, spanY) < 0.125f)
                {
                    float mbA = 0.0f, mgA = 0.0f, mrA = 0.0f;
                    SampleMitchellLinear(
                        primaryPixels.data(), primaryKf.width, primaryKf.height,
                        kfHalfWA + offXA + rhoA * relX - 0.5f,
                        kfHalfHA + offYA + rhoA * relY - 0.5f,
                        rhoA, colors, mbA, mgA, mrA);
                    if (hasCrossfade)
                    {
                        float mbB = 0.0f, mgB = 0.0f, mrB = 0.0f;
                        SampleMitchellLinear(
                            secondaryPixels->data(), secondaryKf->width, secondaryKf->height,
                            kfHalfWB + offXB + rhoB * relX - 0.5f,
                            kfHalfHB + offYB + rhoB * relY - 0.5f,
                            rhoB, colors, mbB, mgB, mrB);
                        outPixels[idx] = EncodeLinearBgr(
                            mbA * wA + mbB * wB,
                            mgA * wA + mgB * wB,
                            mrA * wA + mrB * wB,
                            colors);
                    }
                    else
                    {
                        outPixels[idx] = EncodeLinearBgr(mbA, mgA, mrA, colors);
                    }
                    continue;
                }

                float sumB = 0.0f, sumG = 0.0f, sumR = 0.0f;
                for (uint32_t k = 0; k < tapCount; ++k)
                {
                    const float tapRelX = shutter->shiftX[k] + relX * shutter->scale[k];
                    const float tapRelY = shutter->shiftY[k] + relY * shutter->scale[k];

                    float tbA = 0.0f, tgA = 0.0f, trA = 0.0f;
                    SampleKeyframeTapLinear(
                        primaryPixels.data(), primaryKf.width, primaryKf.height,
                        kfHalfWA + offXA + rhoA * tapRelX - 0.5f,
                        kfHalfHA + offYA + rhoA * tapRelY - 0.5f,
                        boxOffA, colors, tbA, tgA, trA);

                    if (hasCrossfade)
                    {
                        float tbB = 0.0f, tgB = 0.0f, trB = 0.0f;
                        SampleKeyframeTapLinear(
                            secondaryPixels->data(), secondaryKf->width, secondaryKf->height,
                            kfHalfWB + offXB + rhoB * tapRelX - 0.5f,
                            kfHalfHB + offYB + rhoB * tapRelY - 0.5f,
                            boxOffB, colors, tbB, tgB, trB);
                        sumB += tbA * wA + tbB * wB;
                        sumG += tgA * wA + tgB * wB;
                        sumR += trA * wA + trB * wB;
                    }
                    else
                    {
                        sumB += tbA;
                        sumG += tgA;
                        sumR += trA;
                    }
                }

                outPixels[idx] = EncodeLinearBgr(sumB * invTapCount, sumG * invTapCount, sumR * invTapCount, colors);
            }
        }
    }

    void CompositeInnerKeyframe(
        std::vector<uint32_t>& outerPixels,
        const FractalConfig& outerKf,
        const std::vector<uint32_t>& innerPixels,
        const FractalConfig& innerKf,
        const KeyframeInnerRect& innerRect,
        const std::atomic<bool>& cancel)
    {
        if (!innerRect.valid || cancel.load(std::memory_order_relaxed))
        {
            return;
        }
        const int bw = innerRect.blendX1 - innerRect.blendX0;
        const int bh = innerRect.blendY1 - innerRect.blendY0;
        if (bw <= 0 || bh <= 0) return;

        std::vector<float> innerLin;
        ResampleRectLinear(
            innerPixels, innerKf, outerKf,
            innerRect.blendX0, innerRect.blendY0, bw, bh,
            innerLin, cancel);
        if (cancel.load(std::memory_order_relaxed)) return;

        const auto& colors = detail::LinearSrgbTables::Instance();
        const int featherLeft = innerRect.x0 - innerRect.blendX0;
        const int featherRight = innerRect.blendX1 - innerRect.x1;
        const int featherTop = innerRect.y0 - innerRect.blendY0;
        const int featherBottom = innerRect.blendY1 - innerRect.y1;

        #pragma omp parallel for schedule(static)
        for (int ly = 0; ly < bh; ++ly)
        {
            const int y = innerRect.blendY0 + ly;
            uint32_t* outRow = outerPixels.data() + static_cast<size_t>(y) * static_cast<size_t>(outerKf.width);
            const float* linRow = innerLin.data() + static_cast<size_t>(ly) * static_cast<size_t>(bw) * 3;

            for (int lx = 0; lx < bw; ++lx)
            {
                const int x = innerRect.blendX0 + lx;
                const float* in3 = linRow + static_cast<size_t>(lx) * 3;

                if (x >= innerRect.x0 && x < innerRect.x1 && y >= innerRect.y0 && y < innerRect.y1)
                {
                    outRow[x] = EncodeLinearBgr(in3[0], in3[1], in3[2], colors);
                    continue;
                }

                // Compute smooth feather factor from 0 at blend edge to 1 at skip-rect edge.
                float alpha = 1.0f;
                if (featherLeft > 0 && x < innerRect.x0)
                {
                    alpha = std::min(alpha, (static_cast<float>(x - innerRect.blendX0) + 0.5f) / static_cast<float>(featherLeft));
                }
                if (featherRight > 0 && x >= innerRect.x1)
                {
                    alpha = std::min(alpha, (static_cast<float>(innerRect.blendX1 - 1 - x) + 0.5f) / static_cast<float>(featherRight));
                }
                if (featherTop > 0 && y < innerRect.y0)
                {
                    alpha = std::min(alpha, (static_cast<float>(y - innerRect.blendY0) + 0.5f) / static_cast<float>(featherTop));
                }
                if (featherBottom > 0 && y >= innerRect.y1)
                {
                    alpha = std::min(alpha, (static_cast<float>(innerRect.blendY1 - 1 - y) + 0.5f) / static_cast<float>(featherBottom));
                }
                alpha = std::clamp(alpha, 0.0f, 1.0f);
                const float s = alpha * alpha * (3.0f - 2.0f * alpha);

                const uint32_t outerPx = outRow[x];
                const float ob = colors.decode[outerPx & 255u];
                const float og = colors.decode[(outerPx >> 8) & 255u];
                const float or_ = colors.decode[(outerPx >> 16) & 255u];
                const float invS = 1.0f - s;

                outRow[x] = EncodeLinearBgr(
                    ob * invS + in3[0] * s,
                    og * invS + in3[1] * s,
                    or_ * invS + in3[2] * s,
                    colors);
            }
        }
    }
}
