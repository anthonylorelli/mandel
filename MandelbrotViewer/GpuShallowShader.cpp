#include "pch.h"
#include "GpuFractalShaders.h"

namespace Mandelbrot::detail
{
    namespace
    {
        // Shader 1: Direct FP32 evaluation for shallow zooms (zoom < 16.0), all 8 FractalTypes + Julia.
        constexpr const char* kShallowShaderSrc = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint smoothFlag;
    uint fractalType;
    uint juliaFlag;
    uint rowOffset;
    uint subpixelTotal;
    float juliaCx;
    float juliaCy;
    float bailout2;
    float pad0;
    uint outRowOffset, coloringMode, pad3, pad4;
    uint skipX0, skipY0, skipX1, skipY1;
};

Buffer<float> ColCoords : register(t0);
Buffer<float> RowCoords : register(t1);
Buffer<uint> SubpixelIndices : register(t4);
Buffer<uint> SubpixelCountSrv : register(t7);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

float EncodeTrapPotential(float smoothPhase)
{
    float clamped = clamp(smoothPhase, 0.0f, 12.0f);
    return 10.0f * (exp(clamped * 0.625f) - 1.0f);
}

void SampleOrbitColor(
    float zr, float zi, float mod2,
    inout float stripeSum, inout float prevStripeSum, inout uint stripeCount, inout float minDist)
{
    if (coloringMode == 1u)
    {
        prevStripeSum = stripeSum;
        float angle = (zr == 0.0f && zi == 0.0f) ? 0.0f : atan2(zi, zr);
        stripeSum += 0.5f + 0.5f * sin(3.0f * angle);
        ++stripeCount;
    }
    else if (coloringMode == 2u)
    {
        minDist = min(minDist, mod2);
    }
    else if (coloringMode == 3u)
    {
        minDist = min(minDist, min(abs(zr), abs(zi)));
    }
    else if (coloringMode == 4u)
    {
        minDist = min(minDist, abs(sqrt(max(mod2, 0.0f)) - 1.0f));
    }
}

float FinishOrbitColor(
    uint fType, uint iter, float escapeMod2, float effBailout2,
    float stripeSum, float prevStripeSum, uint stripeCount, float minDist)
{
    if (coloringMode == 1u)
    {
        if (stripeCount == 0u) return 0.0f;
        float curAvg = stripeSum / float(stripeCount);
        float avg = curAvg;
        float t = 1.0f;
        if (smoothFlag != 0u && stripeCount >= 2u && escapeMod2 > 1.0f && effBailout2 > 1.0f)
        {
            float logZn = 0.5f * log(escapeMod2);
            float logR = 0.5f * log(effBailout2);
            if (logZn > 0.0f && logR > 0.0f)
            {
                float invLnDeg = (fType == 5u) ? 0.9102392266f : 1.4426950409f;
                t = saturate(1.0f - log(max(logZn / logR, 1e-12f)) * invLnDeg);
                float prevAvg = prevStripeSum / float(stripeCount - 1u);
                avg = lerp(prevAvg, curAvg, t);
            }
        }
        float contIter = max(0.0f, float(iter - 1u) + t);
        float smoothPhase = avg * 2.4f + 0.18f * log(1.0f + 0.1f * contIter);
        return EncodeTrapPotential(smoothPhase);
    }
    if (minDist >= 1e29f) return 0.0f;
    float d = (coloringMode == 2u) ? sqrt(max(minDist, 0.0f)) : minDist;
    float smoothPhase = 0.9f * log(1.0f + 1.0f / max(d, 1e-4f));
    return EncodeTrapPotential(smoothPhase);
}

float FinishSmoothEscape(uint fType, uint iter, float zr, float zi, float prevZr, float prevZi, float cx, float cy)
{
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    uint sIter = iter;
    float maxExtraMod2 = (fType == 5u) ? 1e11f : 1e18f;

    [loop]
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < maxExtraMod2; ++extra)
    {
        if (fType == 0u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 2u)
        {
            zi = -2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fType == 3u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fType == 4u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fType == 5u)
        {
            float nextZr = zr * (zr2 - 3.0f * zi2) + cx;
            zi = zi * (3.0f * zr2 - zi2) + cy;
            zr = nextZr;
        }
        else
        {
            float nextZr = zr2 - zi2 + 0.5667f - 0.5f * prevZr;
            float nextZi = 2.0f * zr * zi - 0.5f * prevZi;
            prevZr = zr;
            prevZi = zi;
            zr = nextZr;
            zi = nextZi;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }

    float logZn = log(zr2 + zi2) * 0.5f;
    float nu = (fType == 5u)
        ? (float(sIter) + 1.0f - log(logZn) * 0.9102392266f)
        : (float(sIter) + 1.0f - log2(logZn));
    return max(0.0f, nu);
}

float EvalShallowSample(float xCoord, float yCoord)
{
    if (juliaFlag != 0u)
    {
        float zr = xCoord;
        float zi = yCoord;
        float jcx = juliaCx;
        float jcy = juliaCy;
        if (jcx == 0.0f && jcy == 0.0f && (zr * zr + zi * zi) <= 1.0f)
            return -1.0f;

        float chkZr = zr, chkZi = zi;
        uint chkPeriod = 8u, stepsSince = 0u;
        float stripeSum = 0.0f, prevStripeSum = 0.0f, minDist = 1e30f;
        uint stripeCount = 0u;
        [loop]
        for (uint iter = 0u; iter < maxIter; ++iter)
        {
            float zr2 = zr * zr;
            float zi2 = zi * zi;
            float mod2 = zr2 + zi2;
            if (coloringMode != 0u && iter > 0u)
            {
                SampleOrbitColor(zr, zi, mod2, stripeSum, prevStripeSum, stripeCount, minDist);
            }
            if (mod2 > bailout2)
            {
                if (coloringMode != 0u)
                    return FinishOrbitColor(fractalType, iter, mod2, bailout2, stripeSum, prevStripeSum, stripeCount, minDist);
                if (smoothFlag == 0u) return float(iter);
                return FinishSmoothEscape(fractalType, iter, zr, zi, 0.0f, 0.0f, jcx, jcy);
            }
            float nextR = zr2 - zi2 + jcx;
            zi = (fractalType == 1u) ? (-2.0f * abs(zr * zi) + jcy) : (2.0f * zr * zi + jcy);
            zr = nextR;
            if (zr == chkZr && zi == chkZi) return -1.0f;
            if (++stepsSince == chkPeriod)
            {
                chkZr = zr; chkZi = zi; stepsSince = 0u;
                if (chkPeriod < 1024u) chkPeriod <<= 1u;
            }
        }
        return -1.0f;
    }

    float cx = xCoord;
    float cy = yCoord;

    if (fractalType == 0u)
    {
        float xm = cx - 0.25f;
        float y2 = cy * cy;
        float q = xm * xm + y2;
        if (q * (q + xm) <= 0.25f * y2 || ((cx + 1.0f) * (cx + 1.0f) + y2) <= 0.0625f)
            return -1.0f;
    }
    else if (fractalType == 7u)
    {
        float zr = cx;
        float zi = cy;
        uint iter = 0u;
        int rootIndex = -1;
        float distSq = 0.0f;
        const float ROOT1_Y = 0.8660254038f;
        const float TOL_SQ = 1e-8f;

        [loop]
        while (iter < maxIter)
        {
            float zr2 = zr * zr;
            float zi2 = zi * zi;
            float r2 = zr2 + zi2;
            if (r2 < 1e-36f) break;

            float d0 = (zr - 1.0f) * (zr - 1.0f) + zi2;
            if (d0 < TOL_SQ) { rootIndex = 0; distSq = d0; break; }

            float d1 = (zr + 0.5f) * (zr + 0.5f) + (zi - ROOT1_Y) * (zi - ROOT1_Y);
            if (d1 < TOL_SQ) { rootIndex = 1; distSq = d1; break; }

            float d2 = (zr + 0.5f) * (zr + 0.5f) + (zi + ROOT1_Y) * (zi + ROOT1_Y);
            if (d2 < TOL_SQ) { rootIndex = 2; distSq = d2; break; }

            float invR2 = 1.0f / r2;
            float nextZr = (2.0f / 3.0f) * zr + (1.0f / 3.0f) * ((zr2 - zi2) * invR2) * invR2;
            float nextZi = (2.0f / 3.0f) * zi - (1.0f / 3.0f) * ((2.0f * zr * zi) * invR2) * invR2;
            zr = nextZr;
            zi = nextZi;
            ++iter;
        }
        if (rootIndex < 0 || iter >= maxIter) return -1.0f;
        float smoothVal = float(iter);
        if (smoothFlag != 0u)
        {
            float dist = max(sqrt(distSq), 1e-12f);
            float ratio = log(dist) / (-9.21034037f);
            if (ratio > 1.0f)
            {
                smoothVal = max(0.0f, float(iter) - log2(ratio));
            }
        }
        return float(rootIndex) * 1000000.0f + smoothVal;
    }

    float effBailout2 = (coloringMode == 1u) ? 1e6f : 16.0f;
    float stripeSum = 0.0f, prevStripeSum = 0.0f, minDist = 1e30f;
    uint stripeCount = 0u;

    if (fractalType == 0u)
    {
        float zr = 0.0f, zi = 0.0f, zr2 = 0.0f, zi2 = 0.0f;
        float chkZr = 0.0f, chkZi = 0.0f;
        uint chkPeriod = 8u, stepsSince = 0u, iter = 0u;

        [loop]
        while (iter < maxIter && (zr2 + zi2) <= effBailout2)
        {
            zi = 2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
            zr2 = zr * zr;
            zi2 = zi * zi;
            ++iter;
            if (coloringMode != 0u)
            {
                SampleOrbitColor(zr, zi, zr2 + zi2, stripeSum, prevStripeSum, stripeCount, minDist);
            }
            if (zr == chkZr && zi == chkZi)
            {
                return -1.0f;
            }
            if (++stepsSince == chkPeriod)
            {
                chkZr = zr; chkZi = zi;
                stepsSince = 0u;
                if (chkPeriod < 1024u) chkPeriod <<= 1u;
            }
        }
        if (iter >= maxIter) return -1.0f;
        if (coloringMode != 0u)
            return FinishOrbitColor(0u, iter, zr2 + zi2, effBailout2, stripeSum, prevStripeSum, stripeCount, minDist);
        if (smoothFlag == 0u) return float(iter);
        return FinishSmoothEscape(0u, iter, zr, zi, 0.0f, 0.0f, cx, cy);
    }

    float zr = (fractalType == 6u) ? cy : 0.0f;
    float zi = (fractalType == 6u) ? cx : 0.0f;
    float zr2 = zr * zr;
    float zi2 = zi * zi;
    float prevZr = 0.0f, prevZi = 0.0f;
    float chkZr = 0.0f, chkZi = 0.0f, chkPrevZr = 0.0f, chkPrevZi = 0.0f;
    uint chkPeriod = 8u, stepsSince = 0u, iter = 0u;

    [loop]
    while (iter < maxIter && (zr2 + zi2) <= effBailout2)
    {
        if (fractalType == 1u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fractalType == 2u)
        {
            zi = -2.0f * zr * zi + cy;
            zr = zr2 - zi2 + cx;
        }
        else if (fractalType == 3u)
        {
            zi = 2.0f * zr * zi + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fractalType == 4u)
        {
            zi = -2.0f * abs(zr * zi) + cy;
            zr = abs(zr2 - zi2) + cx;
        }
        else if (fractalType == 5u)
        {
            float nextZr = zr * (zr2 - 3.0f * zi2) + cx;
            zi = zi * (3.0f * zr2 - zi2) + cy;
            zr = nextZr;
        }
        else
        {
            float nextZr = zr2 - zi2 + 0.5667f - 0.5f * prevZr;
            float nextZi = 2.0f * zr * zi - 0.5f * prevZi;
            prevZr = zr;
            prevZi = zi;
            zr = nextZr;
            zi = nextZi;
        }
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++iter;
        if (coloringMode != 0u)
        {
            SampleOrbitColor(zr, zi, zr2 + zi2, stripeSum, prevStripeSum, stripeCount, minDist);
        }
        if (zr == chkZr && zi == chkZi && (fractalType != 6u || (prevZr == chkPrevZr && prevZi == chkPrevZi)))
        {
            iter = maxIter;
            break;
        }
        if (++stepsSince == chkPeriod)
        {
            chkZr = zr; chkZi = zi;
            chkPrevZr = prevZr; chkPrevZi = prevZi;
            stepsSince = 0u;
            if (chkPeriod < 1024u) chkPeriod <<= 1u;
        }
    }

    if (iter >= maxIter) return -1.0f;
    if (coloringMode != 0u)
        return FinishOrbitColor(fractalType, iter, zr2 + zi2, effBailout2, stripeSum, prevStripeSum, stripeCount, minDist);
    if (smoothFlag == 0u) return float(iter);
    return FinishSmoothEscape(fractalType, iter, zr, zi, prevZr, prevZi, cx, cy);
}

[numthreads(16, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID)
{
    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    if (tid.x >= skipX0 && tid.x < skipX1 && py >= skipY0 && py < skipY1) return;
    float xc = ColCoords[tid.x * 3u + 1u];
    float yc = RowCoords[py * 3u + 1u];
    OutIters[(tid.y + outRowOffset) * width + tid.x] = EvalShallowSample(xc, yc);
}

[numthreads(256, 1, 1)]
void CSSubpixels(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    uint activeTotal = (subpixelTotal == 0xFFFFFFFFu) ? (SubpixelCountSrv[0] * 4u) : subpixelTotal;
    uint sIdx = (gid.y * 32768u + gid.x) * 256u + gi;
    if (sIdx >= activeTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalShallowSample(ColCoords[px * 3u + kx], RowCoords[py * 3u + ky]);
}
)";
    }

    const char* GetShallowShaderSource() noexcept
    {
        return kShallowShaderSrc;
    }
}
