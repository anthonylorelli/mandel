#pragma once
#include "FractalFormulas.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#if defined(_M_ARM64)
#include <arm64_neon.h>
#endif

namespace Mandelbrot::detail
{
#if defined(_M_ARM64)
    // Resume a diverging SIMD lane without repeating iterations or losing the
    // scalar renderer's periodicity checkpoints. Keep the arithmetic order exact.
    inline float FinishMandelbrotLane(double cx, double cy, int maxIter, bool smooth,
        double zr, double zi, double zr2, double zi2, int iter,
        double checkZr, double checkZi, int checkPeriod, int stepsSinceCheck) noexcept
    {
        while (iter < maxIter && zr2 + zi2 <= 16.0)
        {
            zi = 2.0 * zr * zi + cy;
            zr = zr2 - zi2 + cx;
            zr2 = zr * zr;
            zi2 = zi * zi;
            ++iter;
            if (zr == checkZr && zi == checkZi) { iter = maxIter; break; }
            if (++stepsSinceCheck == checkPeriod)
            {
                checkZr = zr;
                checkZi = zi;
                stepsSinceCheck = 0;
                if (checkPeriod < 1024) checkPeriod <<= 1;
            }
        }
        if (iter >= maxIter) return -1.0f;
        if (!smooth) return static_cast<float>(iter);

        int smoothIter = iter;
        for (int extra = 0; extra < 3 && zr2 + zi2 < 1e100; ++extra)
        {
            zi = 2.0 * zr * zi + cy;
            zr = zr2 - zi2 + cx;
            zr2 = zr * zr;
            zi2 = zi * zi;
            ++smoothIter;
        }
        constexpr double invLn2 = 1.44269504088896340736;
        const double logZn = std::log(zr2 + zi2) * 0.5;
        const double nu = smoothIter + 1.0 - std::log(logZn) * invLn2;
        return static_cast<float>(nu > 0.0 ? nu : 0.0);
    }

    inline bool CertifyMandelbrotStandardInteriorStrip(double cxMid, double cy,
        double halfSpanX, int maxIter) noexcept
    {
        if (maxIter < 1 || !(halfSpanX >= 0.0)) return false;
        constexpr double eps = std::numeric_limits<double>::epsilon();
        const double cRadius = halfSpanX * (1.0 + 8.0 * eps) + 8.0 * eps * (std::abs(cxMid) + std::abs(cy));
        if (!std::isfinite(cRadius)) return false;

        double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0, rho = 0.0;
        for (int iter = 0; iter < maxIter; ++iter)
        {
            const double zNorm = std::sqrt(zr2 + zi2);
            if (!(zNorm + rho <= 3.999999)) return false;
            zi = 2.0 * zr * zi + cy;
            zr = zr2 - zi2 + cxMid;
            zr2 = zr * zr;
            zi2 = zi * zi;
            rho = (2.0 * zNorm + rho) * rho + cRadius + 64.0 * eps;
        }
        return std::isfinite(rho);
    }

    inline int ResumeMandelbrotPair(double cx0, double cx1, double cy,
        int maxIter, bool smooth, float64x2_t zr, float64x2_t zi,
        float64x2_t zr2, float64x2_t zi2, int iter,
        float64x2_t checkR, float64x2_t checkI, int checkPeriod, int steps,
        float* out) noexcept
    {
        const double coordinates[]{ cx0, cx1 };
        const float64x2_t cx = vld1q_f64(coordinates);
        const float64x2_t y = vdupq_n_f64(cy), two = vdupq_n_f64(2.0), limit = vdupq_n_f64(16.0);
        uint64x2_t periodic = vdupq_n_u64(0);
        const int startIter = iter;
        while (iter < maxIter)
        {
            // Separate multiply/add operations match /fp:precise scalar results;
            // fused arithmetic changes fine boundary detail between render paths.
            zi = vaddq_f64(vmulq_f64(vmulq_f64(two, zr), zi), y);
            zr = vaddq_f64(vsubq_f64(zr2, zi2), cx);
            zr2 = vmulq_f64(zr, zr);
            zi2 = vmulq_f64(zi, zi);
            ++iter;
            periodic = vandq_u64(vceqq_f64(zr, checkR), vceqq_f64(zi, checkI));
            if (++steps == checkPeriod)
            {
                checkR = zr;
                checkI = zi;
                steps = 0;
                if (checkPeriod < 1024) checkPeriod <<= 1;
            }
            const uint64x2_t bounded = vcleq_f64(vaddq_f64(zr2, zi2), limit);
            const uint64x2_t stopped = vorrq_u64(periodic, veorq_u64(bounded, vdupq_n_u64(~uint64_t(0))));
            // Stop as soon as either lane completes; finish the other in scalar
            // rather than iterating an escaped lane into infinities or NaNs.
            if (vmaxvq_u32(vreinterpretq_u32_u64(stopped))) break;
        }

        double r[2], i[2], r2[2], i2[2], cr[2], ci[2];
        uint64_t cycle[2];
        vst1q_f64(r, zr); vst1q_f64(i, zi);
        vst1q_f64(r2, zr2); vst1q_f64(i2, zi2);
        vst1q_f64(cr, checkR); vst1q_f64(ci, checkI);
        vst1q_u64(cycle, periodic);
        for (int lane = 0; lane < 2; ++lane)
            out[lane] = cycle[lane] || iter >= maxIter ? -1.0f :
                FinishMandelbrotLane(coordinates[lane], cy, maxIter, smooth,
                    r[lane], i[lane], r2[lane], i2[lane], iter, cr[lane], ci[lane], checkPeriod, steps);
        return iter - startIter;
    }

    inline void EvalMandelbrotPair(double cx0, double cx1, double cy,
        int maxIter, bool smooth, float* out) noexcept
    {
        if (IsInMandelbrotCardioidOrBulb(cx0, cy) || IsInMandelbrotCardioidOrBulb(cx1, cy) || maxIter < 1)
        {
            out[0] = EvalStandardPoint(FractalType::Mandelbrot, cx0, cy, maxIter, smooth);
            out[1] = EvalStandardPoint(FractalType::Mandelbrot, cx1, cy, maxIter, smooth);
            return;
        }

        const float64x2_t zero = vdupq_n_f64(0.0);
        ResumeMandelbrotPair(cx0, cx1, cy, maxIter, smooth,
            zero, zero, zero, zero, 0, zero, zero, 8, 0, out);
    }

    inline int EvalMandelbrotQuad(const double cxCoords[4], double cy01, double cy23,
        int maxIter, bool smooth, float* out) noexcept
    {
        if (maxIter < 1)
        {
            out[0] = out[1] = out[2] = out[3] = -1.0f;
            return 0;
        }
        const bool in0 = IsInMandelbrotCardioidOrBulb(cxCoords[0], cy01);
        const bool in1 = IsInMandelbrotCardioidOrBulb(cxCoords[1], cy01);
        const bool in2 = IsInMandelbrotCardioidOrBulb(cxCoords[2], cy23);
        const bool in3 = IsInMandelbrotCardioidOrBulb(cxCoords[3], cy23);
        if (in0 && in1 && in2 && in3)
        {
            out[0] = out[1] = out[2] = out[3] = -1.0f;
            return 0;
        }
        if (in0 || in1 || in2 || in3)
        {
            EvalMandelbrotPair(cxCoords[0], cxCoords[1], cy01, maxIter, smooth, out);
            EvalMandelbrotPair(cxCoords[2], cxCoords[3], cy23, maxIter, smooth, out + 2);
            return 0;
        }

        const float64x2_t cx01 = vld1q_f64(cxCoords);
        const float64x2_t cx23 = vld1q_f64(cxCoords + 2);
        const float64x2_t y01 = vdupq_n_f64(cy01), y23 = vdupq_n_f64(cy23);
        const float64x2_t two = vdupq_n_f64(2.0), limit = vdupq_n_f64(16.0);
        const uint64x2_t allOnes = vdupq_n_u64(~uint64_t(0));
        float64x2_t zr01 = vdupq_n_f64(0.0), zi01 = zr01, zr2_01 = zr01, zi2_01 = zr01, checkR01 = zr01, checkI01 = zr01;
        float64x2_t zr23 = zr01, zi23 = zr01, zr2_23 = zr01, zi2_23 = zr01, checkR23 = zr01, checkI23 = zr01;
        uint64x2_t periodic01 = vdupq_n_u64(0), periodic23 = periodic01;
        uint64x2_t stopped01 = periodic01, stopped23 = periodic01;
        int iter = 0, checkPeriod = 8, steps = 0;

        while (iter < maxIter)
        {
            zi01 = vaddq_f64(vmulq_f64(vmulq_f64(two, zr01), zi01), y01);
            zi23 = vaddq_f64(vmulq_f64(vmulq_f64(two, zr23), zi23), y23);
            zr01 = vaddq_f64(vsubq_f64(zr2_01, zi2_01), cx01);
            zr23 = vaddq_f64(vsubq_f64(zr2_23, zi2_23), cx23);
            zr2_01 = vmulq_f64(zr01, zr01);
            zr2_23 = vmulq_f64(zr23, zr23);
            zi2_01 = vmulq_f64(zi01, zi01);
            zi2_23 = vmulq_f64(zi23, zi23);
            ++iter;
            periodic01 = vandq_u64(vceqq_f64(zr01, checkR01), vceqq_f64(zi01, checkI01));
            periodic23 = vandq_u64(vceqq_f64(zr23, checkR23), vceqq_f64(zi23, checkI23));
            if (++steps == checkPeriod)
            {
                checkR01 = zr01; checkI01 = zi01;
                checkR23 = zr23; checkI23 = zi23;
                steps = 0;
                if (checkPeriod < 1024) checkPeriod <<= 1;
            }
            const uint64x2_t bounded01 = vcleq_f64(vaddq_f64(zr2_01, zi2_01), limit);
            const uint64x2_t bounded23 = vcleq_f64(vaddq_f64(zr2_23, zi2_23), limit);
            stopped01 = vorrq_u64(periodic01, veorq_u64(bounded01, allOnes));
            stopped23 = vorrq_u64(periodic23, veorq_u64(bounded23, allOnes));
            if (vmaxvq_u32(vreinterpretq_u32_u64(vorrq_u64(stopped01, stopped23)))) break;
        }

        if (iter >= maxIter)
        {
            out[0] = out[1] = out[2] = out[3] = -1.0f;
            return iter;
        }

        int extra01 = 0, extra23 = 0;
        if (vmaxvq_u32(vreinterpretq_u32_u64(stopped01)))
        {
            double r[2], i[2], r2[2], i2[2], cr[2], ci[2];
            uint64_t cycle[2];
            vst1q_f64(r, zr01); vst1q_f64(i, zi01);
            vst1q_f64(r2, zr2_01); vst1q_f64(i2, zi2_01);
            vst1q_f64(cr, checkR01); vst1q_f64(ci, checkI01);
            vst1q_u64(cycle, periodic01);
            for (int lane = 0; lane < 2; ++lane)
                out[lane] = cycle[lane] ? -1.0f :
                    FinishMandelbrotLane(cxCoords[lane], cy01, maxIter, smooth,
                        r[lane], i[lane], r2[lane], i2[lane], iter, cr[lane], ci[lane], checkPeriod, steps);
        }
        else
        {
            extra01 = ResumeMandelbrotPair(cxCoords[0], cxCoords[1], cy01, maxIter, smooth,
                zr01, zi01, zr2_01, zi2_01, iter, checkR01, checkI01, checkPeriod, steps, out);
        }

        if (vmaxvq_u32(vreinterpretq_u32_u64(stopped23)))
        {
            double r[2], i[2], r2[2], i2[2], cr[2], ci[2];
            uint64_t cycle[2];
            vst1q_f64(r, zr23); vst1q_f64(i, zi23);
            vst1q_f64(r2, zr2_23); vst1q_f64(i2, zi2_23);
            vst1q_f64(cr, checkR23); vst1q_f64(ci, checkI23);
            vst1q_u64(cycle, periodic23);
            for (int lane = 0; lane < 2; ++lane)
                out[2 + lane] = cycle[lane] ? -1.0f :
                    FinishMandelbrotLane(cxCoords[2 + lane], cy23, maxIter, smooth,
                        r[lane], i[lane], r2[lane], i2[lane], iter, cr[lane], ci[lane], checkPeriod, steps);
        }
        else
        {
            extra23 = ResumeMandelbrotPair(cxCoords[2], cxCoords[3], cy23, maxIter, smooth,
                zr23, zi23, zr2_23, zi2_23, iter, checkR23, checkI23, checkPeriod, steps, out + 2);
        }
        return iter + std::max(extra01, extra23);
    }
#endif
}
