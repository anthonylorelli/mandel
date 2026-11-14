#include "pch.h"
#include "GpuFractalShaders.h"

namespace Mandelbrot::detail
{
    namespace
    {
        // Shader 4: Ultra-Deep FloatExp Perturbation + FloatExp BLA (zoom >= 1e32 .. 1e300).
        constexpr const char* kPerturbDeepExpShaderSrc = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint maxIter;
    uint refLen;
    uint smoothFlag;
    uint useBla;
    uint blaNumLevels;
    uint rowOffset;
    uint subpixelTotal;
    float blaMaxRadMant;
    int blaMaxRadExp;
    float refCx;
    float refCy;
    uint outRowOffset;
    float pad1, pad2;
};

struct GpuOrbitPointExp
{
    float rx, ry;
    float rmx, rmy;
    int rExp;
    int pad0, pad1, pad2;
};

struct GpuCoordExp
{
    float mant;
    int exp;
};

struct GpuBlaBlockExp
{
    float ar, ai;
    float br, bi;
    float radMant;
    int aExp, bExp, radExp;
};

Buffer<float2> OrbitFast : register(t0);
StructuredBuffer<GpuOrbitPointExp> OrbitFull : register(t1);
StructuredBuffer<GpuCoordExp> ColCoordsExp : register(t2);
StructuredBuffer<GpuCoordExp> RowCoordsExp : register(t3);
StructuredBuffer<GpuBlaBlockExp> BlaBlocks : register(t4);
StructuredBuffer<uint2> BlaMeta : register(t5);
Buffer<uint> SubpixelIndices : register(t6);
Buffer<uint> SubpixelCountSrv : register(t7);
RWStructuredBuffer<float> OutIters : register(u0);
RWStructuredBuffer<float> OutSubpixelsFlat : register(u1);

void Renorm2(inout float mx, inout float my, inout int e)
{
    float a = max(abs(mx), abs(my));
    if (a == 0.0f) { mx = 0.0f; my = 0.0f; e = -1073741824; return; }
    if (a < 1.1754944e-38f)
    {
        mx *= 16777216.0f;
        my *= 16777216.0f;
        e -= 24;
        a = max(abs(mx), abs(my));
    }
    uint bits = asuint(a);
    int biasedExp = int((bits >> 23) & 255u);
    int rawExp = biasedExp - 126;
    int scaleBiased = clamp(253 - biasedExp, 1, 254);
    float scale = asfloat(uint(scaleBiased) << 23);
    mx *= scale;
    my *= scale;
    e += rawExp;
}

void FloatExpAdd(float ax, float ay, int ae, float bx, float by, int be, out float ox, out float oy, out int oe)
{
    if (ae <= -100000000) { ox = bx; oy = by; oe = be; return; }
    if (be <= -100000000) { ox = ax; oy = ay; oe = ae; return; }
    if (ae >= be)
    {
        int diff = ae - be;
        if (diff >= 30) { ox = ax; oy = ay; oe = ae; return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = ax + bx * s; oy = ay + by * s; oe = ae;
    }
    else
    {
        int diff = be - ae;
        if (diff >= 30) { ox = bx; oy = by; oe = be; return; }
        float s = asfloat(uint(127 - diff) << 23);
        ox = bx + ax * s; oy = by + ay * s; oe = be;
    }
    Renorm2(ox, oy, oe);
}

bool FloatExpLess(float dmx, float dmy, int de, float radMant, int radExp)
{
    if (radExp <= -100000000) return false;
    if (de <= -100000000) return true;
    int diff = de - radExp;
    if (diff < -1) return true;
    if (diff > 1) return false;
    float s = asfloat(uint(127 + diff) << 23);
    return sqrt(dmx * dmx + dmy * dmy) * s < radMant;
}

float EvalDeepExp(GpuCoordExp cx, GpuCoordExp cy)
{
    float dcMx = 0.0f, dcMy = 0.0f;
    int dcExp = -1073741824;
    if (cx.exp > -100000000 || cy.exp > -100000000)
    {
        dcExp = max(cx.exp, cy.exp);
        dcMx = (cx.exp > -100000000 && (dcExp - cx.exp) < 30)
            ? (cx.mant * asfloat(uint(127 - (dcExp - cx.exp)) << 23))
            : 0.0f;
        dcMy = (cy.exp > -100000000 && (dcExp - cy.exp) < 30)
            ? (cy.mant * asfloat(uint(127 - (dcExp - cy.exp)) << 23))
            : 0.0f;
        Renorm2(dcMx, dcMy, dcExp);
    }

    float dcxF = (dcExp > -126) ? (dcMx * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;
    float dcyF = (dcExp > -126) ? (dcMy * asfloat(uint(clamp(127 + dcExp, 1, 254)) << 23)) : 0.0f;

    uint m = 0u;
    uint iter = 0u;
    float escZx = 0.0f, escZy = 0.0f;
    float dmx = 0.0f, dmy = 0.0f;
    int dExp = -1073741824;

    [loop]
    while (iter < maxIter)
    {
        if (dExp >= -45 && dExp <= 10 && m > 0u)
        {
            float initScale = asfloat(uint(127 + dExp) << 23);
            float dx = dmx * initScale;
            float dy = dmy * initScale;
            bool escaped = false;
            [loop]
            while (iter < maxIter)
            {
                float2 r = OrbitFast[m];
                float rx = r.x, ry = r.y;
                float zx = rx + dx;
                float zy = ry + dy;
                float mod2 = zx * zx + zy * zy;
                if (mod2 > 16.0f) { escZx = zx; escZy = zy; escaped = true; break; }
                float dz2 = dx * dx + dy * dy;
                if (m == refLen || (m > 0u && mod2 < dz2))
                {
                    dx = zx; dy = zy; m = 0u; rx = 0.0f; ry = 0.0f;
                    if (max(abs(dx), abs(dy)) < 1e-12f)
                    {
                        dmx = dx * 1.0842022e19f; dmy = dy * 1.0842022e19f; dExp = -63;
                        Renorm2(dmx, dmy, dExp);
                        break;
                    }
                }
                float sx = rx + zx;
                float sy = ry + zy;
                float nextDx = sx * dx - sy * dy + dcxF;
                dy = sx * dy + sy * dx + dcyF;
                dx = nextDx;
                ++m;
                ++iter;
                if (max(abs(dx), abs(dy)) < 2.842171e-14f)
                {
                    dmx = dx * 1.0842022e19f; dmy = dy * 1.0842022e19f; dExp = -63;
                    Renorm2(dmx, dmy, dExp);
                    break;
                }
            }
            if (escaped || iter >= maxIter) break;
            continue;
        }

        GpuOrbitPointExp op = OrbitFull[m];
        if (op.rExp >= 2)
        {
            float dScale = (dExp >= -125 && dExp <= 10) ? asfloat(uint(127 + dExp) << 23) : 0.0f;
            float zx = op.rx + dmx * dScale;
            float zy = op.ry + dmy * dScale;
            if (zx * zx + zy * zy > 16.0f) { escZx = zx; escZy = zy; break; }
        }

        bool doRebase = (m == refLen);
        float zmx = 0.0f, zmy = 0.0f;
        int zExp = -1073741824;
        if (doRebase || (m > 0u && op.rExp <= dExp + 2))
        {
            FloatExpAdd(op.rmx, op.rmy, op.rExp, dmx, dmy, dExp, zmx, zmy, zExp);
            if (!doRebase && m > 0u)
            {
                int cExp = max(zExp, dExp);
                if (cExp > -100000000)
                {
                    float zs = (zExp > -100000000 && (cExp - zExp) < 30) ? asfloat(uint(127 - (cExp - zExp)) << 23) : 0.0f;
                    float ds = (dExp > -100000000 && (cExp - dExp) < 30) ? asfloat(uint(127 - (cExp - dExp)) << 23) : 0.0f;
                    float zxS = zmx * zs, zyS = zmy * zs;
                    float dxS = dmx * ds, dyS = dmy * ds;
                    if (zxS * zxS + zyS * zyS < dxS * dxS + dyS * dyS) doRebase = true;
                }
            }
        }
        if (doRebase)
        {
            dmx = zmx; dmy = zmy; dExp = zExp; m = 0u;
            op.rx = 0.0f; op.ry = 0.0f; op.rmx = 0.0f; op.rmy = 0.0f; op.rExp = -1073741824;
        }

        if (useBla != 0u && m >= 1u && (maxIter - iter) >= 4u && ((m - 1u) & 3u) == 0u && m + 4u <= refLen &&
            FloatExpLess(dmx, dmy, dExp, blaMaxRadMant, blaMaxRadExp))
        {
            uint start = m - 1u;
            uint aligned = (start != 0u) ? firstbitlow(start) : 30u;
            uint top = min(aligned, blaNumLevels - 1u);
            uint skipped = 0u;
            for (int lv = int(top); lv >= 2; --lv)
            {
                uint len = 1u << uint(lv);
                if (len > (maxIter - iter) || m + len > refLen) continue;
                uint2 meta = BlaMeta[lv];
                uint idx = start >> uint(lv);
                if (idx >= meta.y) continue;
                GpuBlaBlockExp blk = BlaBlocks[meta.x + idx];
                if (!FloatExpLess(dmx, dmy, dExp, blk.radMant, blk.radExp)) continue;

                float azx = blk.ar * dmx - blk.ai * dmy;
                float azy = blk.ar * dmy + blk.ai * dmx;
                int azExp = (dExp > -100000000) ? (blk.aExp + dExp) : -1073741824;
                Renorm2(azx, azy, azExp);

                float bcx = blk.br * dcMx - blk.bi * dcMy;
                float bcy = blk.br * dcMy + blk.bi * dcMx;
                int bcExp = (dcExp > -100000000) ? (blk.bExp + dcExp) : -1073741824;
                Renorm2(bcx, bcy, bcExp);

                FloatExpAdd(azx, azy, azExp, bcx, bcy, bcExp, dmx, dmy, dExp);
                skipped = len;
                break;
            }
            if (skipped != 0u) { m += skipped; iter += skipped; continue; }
        }

        float smx = dmx, smy = dmy;
        int sExp = dExp;
        if (op.rExp > -100000000)
        {
            FloatExpAdd(op.rmx, op.rmy, op.rExp + 1, dmx, dmy, dExp, smx, smy, sExp);
        }
        float prodX = smx * dmx - smy * dmy;
        float prodY = smx * dmy + smy * dmx;
        int prodExp = (sExp > -100000000 && dExp > -100000000) ? (sExp + dExp) : -1073741824;
        Renorm2(prodX, prodY, prodExp);
        FloatExpAdd(prodX, prodY, prodExp, dcMx, dcMy, dcExp, dmx, dmy, dExp);
        ++m;
        ++iter;
    }

    if (iter >= maxIter) return -1.0f;
    if (smoothFlag == 0u) return float(iter);

    float zr = escZx, zi = escZy;
    float zr2 = zr * zr, zi2 = zi * zi;
    uint sIter = iter;
    for (uint extra = 0u; extra < 3u && (zr2 + zi2) < 1e18f; ++extra)
    {
        zi = 2.0f * zr * zi + refCy;
        zr = zr2 - zi2 + refCx;
        zr2 = zr * zr;
        zi2 = zi * zi;
        ++sIter;
    }
    return max(0.0f, float(sIter) + 1.0f - log2(log(zr2 + zi2) * 0.5f));
}

[numthreads(16, 16, 1)]
void CSPrimary(uint3 tid : SV_DispatchThreadID)
{
    uint py = tid.y + rowOffset;
    if (tid.x >= width || py >= height) return;
    OutIters[(tid.y + outRowOffset) * width + tid.x] = EvalDeepExp(ColCoordsExp[tid.x * 3u + 1u], RowCoordsExp[py * 3u + 1u]);
}

[numthreads(256, 1, 1)]
void CSSubpixels(uint3 tid : SV_DispatchThreadID)
{
    uint activeTotal = (subpixelTotal == 0xFFFFFFFFu) ? (SubpixelCountSrv[0] * 4u) : subpixelTotal;
    uint sIdx = tid.x;
    if (sIdx >= activeTotal) return;
    uint pix = SubpixelIndices[sIdx >> 2];
    uint corner = sIdx & 3u;
    uint py = pix / width;
    uint px = pix - py * width;
    uint kx = (corner & 1u) ? 2u : 0u;
    uint ky = (corner & 2u) ? 2u : 0u;
    OutSubpixelsFlat[sIdx] = EvalDeepExp(ColCoordsExp[px * 3u + kx], RowCoordsExp[py * 3u + ky]);
}
)";

        // Shader 5: GPU Boundary Detection + 16x16 Tile Compaction + Indirect Dispatch Argument Builder.
        constexpr const char* kPostDetectShaderSrc = R"(
cbuffer PostParams : register(b0)
{
    uint width;
    uint bandHeight;
    uint visibleRows;
    uint topHalo;
    uint destRowOffset;
    uint fractalType;
    uint smoothFlag;
    uint invertFlag;
    float frequency;
    float phaseOffset;
    uint interiorColor;
    uint avgRgb;
    uint temporalMode;
    uint subGroupThreads;
    uint pad0, pad1;
};

struct GpuPixelSubInfo
{
    uint slot;
    float weight;
};

StructuredBuffer<float> InIters : register(t0);
Buffer<uint> InSubCounter : register(t5);

RWBuffer<uint> OutSubpixelIndices : register(u0);
RWStructuredBuffer<GpuPixelSubInfo> OutPixelSubInfo : register(u1);
RWBuffer<uint> OutSubCounter : register(u2);

groupshared uint s_groupCount;
groupshared uint s_groupBase;

[numthreads(16, 16, 1)]
void CSDetectBoundaries(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0u) s_groupCount = 0u;
    GroupMemoryBarrierWithGroupSync();

    bool valid = (tid.x < width && tid.y < visibleRows);
    float weight = 0.0f;
    uint fullIdx = 0u;

    if (valid)
    {
        uint y = tid.y + topHalo;
        uint x = tid.x;
        fullIdx = y * width + x;
        float center = InIters[fullIdx];
        uint x0 = (x > 0u) ? (x - 1u) : 0u;
        uint x1 = min(width - 1u, x + 1u);
        uint y0 = (y > 0u) ? (y - 1u) : 0u;
        uint y1 = min(bandHeight - 1u, y + 1u);

        if (center < 0.0f)
        {
            bool boundary = false;
            [loop]
            for (uint ny = y0; ny <= y1 && !boundary; ++ny)
            {
                uint rowBase = ny * width;
                [loop]
                for (uint nx = x0; nx <= x1; ++nx)
                {
                    if (InIters[rowBase + nx] >= 0.0f) { boundary = true; break; }
                }
            }
            weight = boundary ? 1.0f : 0.0f;
        }
        else
        {
            float variation = 0.0f;
            bool boundary = false;
            float a = (fractalType == 7u) ? fmod(center, 1000000.0f) : center;
            [loop]
            for (uint ny = y0; ny <= y1; ++ny)
            {
                uint rowBase = ny * width;
                [loop]
                for (uint nx = x0; nx <= x1; ++nx)
                {
                    if (nx == x && ny == y) continue;
                    float neighbor = InIters[rowBase + nx];
                    if (neighbor < 0.0f) { boundary = true; continue; }
                    bool sameBasin = (fractalType != 7u) || (int(center / 1000000.0f) == int(neighbor / 1000000.0f));
                    if (!sameBasin || (smoothFlag == 0u && center != neighbor))
                    {
                        boundary = true;
                    }
                    float b = (fractalType == 7u) ? fmod(neighbor, 1000000.0f) : neighbor;
                    variation = max(variation, abs(a - b) / (10.0f + min(a, b)));
                }
            }
            float t = saturate((variation - 0.015f) / 0.065f);
            weight = boundary ? 1.0f : (t * t * (3.0f - 2.0f * t));
        }
    }

    uint localOffset = 0u;
    if (valid && weight > 0.0f)
    {
        InterlockedAdd(s_groupCount, 1u, localOffset);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0u && s_groupCount > 0u)
    {
        InterlockedAdd(OutSubCounter[0], s_groupCount, s_groupBase);
    }
    GroupMemoryBarrierWithGroupSync();

    if (valid)
    {
        GpuPixelSubInfo info;
        if (weight > 0.0f)
        {
            uint slot = s_groupBase + localOffset;
            OutSubpixelIndices[slot] = fullIdx;
            info.slot = slot;
            info.weight = weight;
        }
        else
        {
            info.slot = 0xFFFFFFFFu;
            info.weight = 0.0f;
        }
        OutPixelSubInfo[tid.y * width + tid.x] = info;
    }
}

[numthreads(1, 1, 1)]
void CSBuildIndirectArgs(uint3 tid : SV_DispatchThreadID)
{
    uint subTotal = InSubCounter[0] * 4u;
    OutSubpixelIndices[0] = (subTotal > 0u) ? ((subTotal + subGroupThreads - 1u) / subGroupThreads) : 0u;
    OutSubpixelIndices[1] = 1u;
    OutSubpixelIndices[2] = 1u;
}
)";

        // Shader 6: Fused GPU 4x Supersampled Palette Coloring + Linear-Light Temporal Blending.
        constexpr const char* kPostResolveShaderSrc = R"(
cbuffer PostParams : register(b0)
{
    uint width;
    uint bandHeight;
    uint visibleRows;
    uint topHalo;
    uint destRowOffset;
    uint fractalType;
    uint smoothFlag;
    uint invertFlag;
    float frequency;
    float phaseOffset;
    uint interiorColor;
    uint avgRgb;
    uint temporalMode;
    uint subGroupThreads;
    uint pad0, pad1;
};

struct GpuPixelSubInfo
{
    uint slot;
    float weight;
};

StructuredBuffer<float> InIters : register(t0);
StructuredBuffer<GpuPixelSubInfo> InPixelSubInfo : register(t1);
StructuredBuffer<float> InSubpixelsFlat : register(t2);
Buffer<uint> PaletteLut : register(t3);
StructuredBuffer<uint> PrevBandPixels : register(t4);

RWStructuredBuffer<uint> OutAccumPixels : register(u0);

float MapIterToPhase(float iterVal)
{
    if (iterVal <= 0.0f) return 0.0f;
    return 1.6f * log(1.0f + iterVal * 0.1f) * frequency;
}

uint SamplePaletteLut(float phase)
{
    float u = phase - floor(phase);
    if (u >= 1.0f) u = 0.0f;
    if (invertFlag != 0u) u = 1.0f - u;
    return PaletteLut[uint(u * 4095.0f) & 4095u];
}

uint ColorAtPhaseGpu(float basePhase, float deltaU)
{
    float uCenter = basePhase + phaseOffset;
    if (deltaU <= 0.04f) return SamplePaletteLut(uCenter);
    float span = min(deltaU, 1.0f);
    uint c0 = SamplePaletteLut(uCenter - 0.375f * span);
    uint c1 = SamplePaletteLut(uCenter - 0.125f * span);
    uint c2 = SamplePaletteLut(uCenter + 0.125f * span);
    uint c3 = SamplePaletteLut(uCenter + 0.375f * span);
    float r = float(((c0 >> 16) & 255u) + ((c1 >> 16) & 255u) + ((c2 >> 16) & 255u) + ((c3 >> 16) & 255u)) * 0.25f;
    float g = float(((c0 >> 8) & 255u)  + ((c1 >> 8) & 255u)  + ((c2 >> 8) & 255u)  + ((c3 >> 8) & 255u))  * 0.25f;
    float b = float((c0 & 255u)         + (c1 & 255u)         + (c2 & 255u)         + (c3 & 255u))         * 0.25f;
    if (deltaU > 0.20f)
    {
        float damp = clamp(deltaU - 0.20f, 0.0f, 0.72f);
        float avgR = float((avgRgb >> 16) & 255u);
        float avgG = float((avgRgb >> 8) & 255u);
        float avgB = float(avgRgb & 255u);
        r = r * (1.0f - damp) + avgR * damp;
        g = g * (1.0f - damp) + avgG * damp;
        b = b * (1.0f - damp) + avgB * damp;
    }
    return 0xFF000000u |
        (uint(clamp(r + 0.5f, 0.0f, 255.0f)) << 16) |
        (uint(clamp(g + 0.5f, 0.0f, 255.0f)) << 8) |
        uint(clamp(b + 0.5f, 0.0f, 255.0f));
}

uint ColorIterationGpu(float value)
{
    if (value < 0.0f) return interiorColor;
    if (fractalType == 7u)
    {
        int root = int(value / 1000000.0f);
        float iter = value - float(root) * 1000000.0f;
        float rootBase = float(root % 3) * (1.0f / 3.0f);
        float compressedIter = 12.0f * log(1.0f + iter * (1.0f / 12.0f));
        float rawRing = compressedIter * 0.06f * frequency + phaseOffset;
        float ring = rawRing - floor(rawRing);
        if (ring >= 1.0f) ring = 0.0f;
        float u = rootBase + ring * (1.0f / 3.0f);
        if (u >= 1.0f) u -= 1.0f;
        if (invertFlag != 0u) u = 1.0f - u;
        uint color = PaletteLut[uint(u * 4095.0f) & 4095u];
        float shading = max(0.35f, 1.0f / (1.0f + 0.025f * compressedIter));
        if (shading < 0.99f)
        {
            uint a = (color >> 24) & 255u;
            uint r = uint(float((color >> 16) & 255u) * shading);
            uint g = uint(float((color >> 8) & 255u) * shading);
            uint b = uint(float(color & 255u) * shading);
            color = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return color;
    }
    return SamplePaletteLut(MapIterToPhase(value) + phaseOffset);
}

float SrgbByteToLinear(uint byteVal)
{
    float v = float(byteVal) * (1.0f / 255.0f);
    return (v <= 0.04045f) ? (v * (1.0f / 12.92f)) : pow((v + 0.055f) * (1.0f / 1.055f), 2.4f);
}

uint LinearToSrgbByte(float lin)
{
    float v = floor(clamp(lin * 4096.0f + 0.5f, 0.0f, 4096.0f)) * (1.0f / 4096.0f);
    float s = (v <= 0.0031308f) ? (12.92f * v) : (1.055f * pow(max(v, 0.0f), 1.0f / 2.4f) - 0.055f);
    return uint(clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
}

[numthreads(16, 16, 1)]
void CSResolveAndBlend(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= visibleRows) return;

    uint fullIdx = (tid.y + topHalo) * width + tid.x;
    uint centerColor = ColorIterationGpu(InIters[fullIdx]);
    uint resolvedColor = centerColor;

    GpuPixelSubInfo info = InPixelSubInfo[tid.y * width + tid.x];
    if (info.slot != 0xFFFFFFFFu && info.weight > 0.0f)
    {
        uint baseIdx = info.slot * 4u;
        float sIter[4] = {
            InSubpixelsFlat[baseIdx + 0u],
            InSubpixelsFlat[baseIdx + 1u],
            InSubpixelsFlat[baseIdx + 2u],
            InSubpixelsFlat[baseIdx + 3u]
        };
        float weight = min(info.weight, 1.0f);

        if (sIter[0] < 0.0f && sIter[1] < 0.0f && sIter[2] < 0.0f && sIter[3] < 0.0f &&
            (centerColor == interiorColor || weight >= 1.0f))
        {
            resolvedColor = interiorColor;
        }
        else
        {
            float phases[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (fractalType != 7u)
            {
                [unroll]
                for (uint s = 0u; s < 4u; ++s)
                    if (sIter[s] >= 0.0f)
                        phases[s] = MapIterToPhase(sIter[s]);
            }

            uint colors[4];
            [unroll]
            for (uint s = 0u; s < 4u; ++s)
            {
                if (fractalType != 7u && sIter[s] >= 0.0f)
                {
                    float deltaU = 0.0f;
                    uint n0 = s ^ 1u;
                    uint n1 = s ^ 2u;
                    if (sIter[n0] >= 0.0f) deltaU = max(deltaU, abs(phases[s] - phases[n0]));
                    if (sIter[n1] >= 0.0f) deltaU = max(deltaU, abs(phases[s] - phases[n1]));
                    colors[s] = ColorAtPhaseGpu(phases[s], deltaU);
                }
                else
                {
                    colors[s] = ColorIterationGpu(sIter[s]);
                }
            }

            if (colors[0] == colors[1] && colors[0] == colors[2] && colors[0] == colors[3] &&
                (colors[0] == centerColor || weight >= 1.0f))
            {
                resolvedColor = colors[0];
            }
            else
            {
                uint outRgb = 0u;
                [unroll]
                for (uint ch = 0u; ch < 3u; ++ch)
                {
                    uint sh = ch * 8u;
                    float avg = (SrgbByteToLinear((colors[0] >> sh) & 255u) +
                                 SrgbByteToLinear((colors[1] >> sh) & 255u) +
                                 SrgbByteToLinear((colors[2] >> sh) & 255u) +
                                 SrgbByteToLinear((colors[3] >> sh) & 255u)) * 0.25f;
                    float lin = (weight >= 1.0f)
                        ? avg
                        : (avg * weight + SrgbByteToLinear((centerColor >> sh) & 255u) * (1.0f - weight));
                    outRgb |= LinearToSrgbByte(lin) << sh;
                }
                float alphaAvg = float((colors[0] >> 24) + (colors[1] >> 24) + (colors[2] >> 24) + (colors[3] >> 24)) * 0.25f;
                float alpha = (weight >= 1.0f)
                    ? alphaAvg
                    : (alphaAvg * weight + float(centerColor >> 24) * (1.0f - weight));
                resolvedColor = outRgb | (uint(alpha + 0.5f) << 24);
            }
        }
    }

    uint localIdx = tid.y * width + tid.x;
    uint dstIdx = (destRowOffset + tid.y) * width + tid.x;

    if (temporalMode == 0u)
    {
        OutAccumPixels[dstIdx] = resolvedColor;
    }
    else
    {
        uint c0 = (temporalMode == 1u) ? OutAccumPixels[dstIdx] : PrevBandPixels[localIdx];
        if (c0 != resolvedColor)
        {
            uint blended = 0xFF000000u;
            [unroll]
            for (uint ch = 0u; ch < 3u; ++ch)
            {
                uint sh = ch * 8u;
                float avg = (SrgbByteToLinear((c0 >> sh) & 255u) +
                             SrgbByteToLinear((resolvedColor >> sh) & 255u)) * 0.5f;
                blended |= LinearToSrgbByte(avg) << sh;
            }
            OutAccumPixels[dstIdx] = blended;
        }
        else if (temporalMode == 2u)
        {
            OutAccumPixels[dstIdx] = c0;
        }
    }
}
)";
    }

    const char* GetPerturbDeepExpShaderSource() noexcept
    {
        return kPerturbDeepExpShaderSrc;
    }

    const char* GetMovieDetectShaderSource() noexcept
    {
        return kPostDetectShaderSrc;
    }

    const char* GetMovieResolveShaderSource() noexcept
    {
        return kPostResolveShaderSrc;
    }
}
