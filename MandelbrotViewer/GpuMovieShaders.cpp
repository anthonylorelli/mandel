#include "pch.h"
#include "GpuFractalShaders.h"

namespace Mandelbrot::detail
{
    namespace
    {
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
    uint subGroupThreads;
    uint pad0, pad1, pad2;
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
    uint groups = (subTotal > 0u) ? ((subTotal + subGroupThreads - 1u) / subGroupThreads) : 0u;
    OutSubpixelIndices[0] = (groups > 32768u) ? 32768u : groups;
    OutSubpixelIndices[1] = (groups > 32768u) ? ((groups + 32767u) / 32768u) : 1u;
    OutSubpixelIndices[2] = 1u;
}

[numthreads(1, 1, 1)]
void CSBuildActiveIndirectArgs(uint3 tid : SV_DispatchThreadID)
{
    uint activeTotal = InSubCounter[0];
    uint groups = (activeTotal > 0u) ? ((activeTotal + subGroupThreads - 1u) / subGroupThreads) : 0u;
    OutSubpixelIndices[0] = (groups > 32768u) ? 32768u : groups;
    OutSubpixelIndices[1] = (groups > 32768u) ? ((groups + 32767u) / 32768u) : 1u;
    OutSubpixelIndices[2] = 1u;
}
)";

#define MANDEL_HLSL_SRGB_HELPERS \
"\nfloat SrgbByteToLinear(uint byteVal)\n" \
"{\n" \
"    float v = float(byteVal) * (1.0f / 255.0f);\n" \
"    return (v <= 0.04045f) ? (v * (1.0f / 12.92f)) : pow((v + 0.055f) * (1.0f / 1.055f), 2.4f);\n" \
"}\n\n" \
"uint LinearToSrgbByte(float lin)\n" \
"{\n" \
"    float v = floor(clamp(lin * 4096.0f + 0.5f, 0.0f, 4096.0f)) * (1.0f / 4096.0f);\n" \
"    float s = (v <= 0.0031308f) ? (12.92f * v) : (1.055f * pow(max(v, 0.0f), 1.0f / 2.4f) - 0.055f);\n" \
"    return uint(clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));\n" \
"}\n"

        // Shader 6: Fused GPU 4x Supersampled Palette Coloring.
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
    uint subGroupThreads;
    uint pad0, pad1, pad2;
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
)" MANDEL_HLSL_SRGB_HELPERS R"(
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

    uint dstIdx = (destRowOffset + tid.y) * width + tid.x;
    OutAccumPixels[dstIdx] = resolvedColor;
}
)";

        // Shader 7: Analytical 180-degree camera-trajectory shutter motion blur in linear light.
        constexpr const char* kPostShutterShaderSrc = R"(
cbuffer ShutterParams : register(b0)
{
    uint width;
    uint height;
    uint tapCount;
    uint pad0;
    float4 taps[7];
};

StructuredBuffer<uint> InRawPixels : register(t0);
RWStructuredBuffer<uint> OutShutterPixels : register(u0);
)" MANDEL_HLSL_SRGB_HELPERS R"(
float3 DecodeBgraLinear(uint c)
{
    return float3(
        SrgbByteToLinear(c & 255u),
        SrgbByteToLinear((c >> 8) & 255u),
        SrgbByteToLinear((c >> 16) & 255u));
}

[numthreads(16, 16, 1)]
void CSShutterMotionBlur(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= width || tid.y >= height) return;

    uint idx = tid.y * width + tid.x;
    uint centerColor = InRawPixels[idx];
    if (tapCount <= 1u)
    {
        OutShutterPixels[idx] = centerColor;
        return;
    }

    float halfW = float(width) * 0.5f;
    float halfH = float(height) * 0.5f;
    float relX = float(tid.x) + 0.5f - halfW;
    float relY = float(tid.y) + 0.5f - halfH;

    float4 firstTap = taps[0];
    float4 lastTap = taps[tapCount - 1u];
    float spanX = abs((lastTap.x - firstTap.x) + relX * (lastTap.z - firstTap.z));
    float spanY = abs((lastTap.y - firstTap.y) + relY * (lastTap.z - firstTap.z));
    if (max(spanX, spanY) < 0.125f)
    {
        OutShutterPixels[idx] = centerColor;
        return;
    }

    float maxPx = float(width - 1u);
    float maxPy = float(height - 1u);
    float3 sumLin = float3(0.0f, 0.0f, 0.0f);
    uint validCount = 0u;
    bool allEqualCenter = true;

    [loop]
    for (uint k = 0u; k < tapCount; ++k)
    {
        float4 tap = taps[k];
        float px = halfW + tap.x + relX * tap.z - 0.5f;
        float py = halfH + tap.y + relY * tap.z - 0.5f;
        if (px < 0.0f || px > maxPx || py < 0.0f || py > maxPy)
            continue;

        uint x0 = uint(px);
        uint y0 = uint(py);
        uint x1 = min(x0 + 1u, width - 1u);
        uint y1 = min(y0 + 1u, height - 1u);
        float fx = px - float(x0);
        float fy = py - float(y0);

        uint c00 = InRawPixels[y0 * width + x0];
        uint c10 = InRawPixels[y0 * width + x1];
        uint c01 = InRawPixels[y1 * width + x0];
        uint c11 = InRawPixels[y1 * width + x1];

        if (c00 == c10 && c00 == c01 && c00 == c11)
        {
            if (c00 != centerColor) allEqualCenter = false;
            sumLin += DecodeBgraLinear(c00);
        }
        else
        {
            allEqualCenter = false;
            float w00 = (1.0f - fx) * (1.0f - fy);
            float w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy;
            float w11 = fx * fy;
            sumLin += DecodeBgraLinear(c00) * w00
                    + DecodeBgraLinear(c10) * w10
                    + DecodeBgraLinear(c01) * w01
                    + DecodeBgraLinear(c11) * w11;
        }
        ++validCount;
    }

    if (validCount == 0u || allEqualCenter)
    {
        OutShutterPixels[idx] = centerColor;
        return;
    }

    float3 avgLin = sumLin * (1.0f / float(validCount));
    OutShutterPixels[idx] = 0xFF000000u
        | (LinearToSrgbByte(avgLin.z) << 16)
        | (LinearToSrgbByte(avgLin.y) << 8)
        | LinearToSrgbByte(avgLin.x);
}
)";
#undef MANDEL_HLSL_SRGB_HELPERS
    }

    const char* GetMovieDetectShaderSource() noexcept
    {
        return kPostDetectShaderSrc;
    }

    const char* GetMovieResolveShaderSource() noexcept
    {
        return kPostResolveShaderSrc;
    }

    const char* GetMovieShutterShaderSource() noexcept
    {
        return kPostShutterShaderSrc;
    }
}
